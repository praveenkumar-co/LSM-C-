// Phase 7 tests: scanDocuments() (batched vLog reads) against scanSlow() (pin + scan() + get()
// per key) and an in-memory model. SC1 random ranges and edge cases, SC2 entries relocated by GC,
// SC3 checkpoint, reopen and a range ending in the active segment, SC4 kill -9 and recovery,
// SC5 corruption and a missing segment (same VLogReadError as get()), SC6 scans while 8 writer
// threads and GC run, SC7 the pin is held for the whole call.
//
// The guarantee both scans give (and SC6 checks): every returned document is a complete,
// committed version of its key that was the key's current version at some instant during the
// call, and a key is left out only if it was absent at some instant during the call.
// scanDocuments additionally reads every key at one instant (one tree walk).
#include "test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexedStore.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using bpt::IndexedStore;
using Model = std::map<std::string, std::string>; // key -> what get() must return
using DocScan = IndexedStore::DocScanResult;

// ------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------
// 16 KiB segments: a range of a few dozen documents spans several segments.
static IndexedStore::Options storeOpts(uint32_t seg_limit = 16 * 1024) {
    IndexedStore::Options o;
    o.cache_frames = 64;
    o.vlog_opts = vlog::VLogWriter::Options(
        seg_limit, std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index.db / superblock.db write"); };
    o.auto_checkpoint = false;
    o.gc_rate_mb_s = 0;
    return o;
}

static std::string key(int i) {
    char b[16];
    std::snprintf(b, sizeof(b), "k%05d", i);
    return b;
}
static int keyId(const std::string& k) {
    return std::stoi(k.substr(1));
}

// Canonical JSON (doc::serialize), so a Full entry and a materialized chain compare alike.
static std::string makeDoc(int id, int64_t score, size_t pad) {
    doc::Value v = doc::Value::object();
    v.set("id", doc::Value::integer(id));
    v.set("score", doc::Value::integer(score));
    v.set("pad", doc::Value::string(std::string(pad, static_cast<char>('a' + id % 26))));
    return doc::serialize(v);
}
static int64_t scoreOf(const std::string& d) {
    return doc::parse(d).find("score")->asInt();
}

// Store + model kept in step.
struct Fixture {
    std::unique_ptr<IndexedStore> st;
    Model model;
    void put(int id, int64_t score, size_t pad) {
        const std::string d = makeDoc(id, score, pad);
        st->put(key(id), d);
        model[key(id)] = d;
    }
    void inc(int id) {
        bpt::UpdateResult r = st->update(key(id), {doc::inc("score", 1)});
        CHECK(r.written);
        doc::Value v = doc::parse(model[key(id)]);
        doc::applyOps(v, {doc::inc("score", 1)});
        model[key(id)] = doc::serialize(v);
    }
    void erase(int id) {
        st->erase(key(id));
        model.erase(key(id));
    }
};

static DocScan modelScan(const Model& m, const std::string& a, const std::string& b,
                         size_t limit) {
    DocScan out;
    if (limit == 0 || a >= b) return out;
    for (auto it = m.lower_bound(a); it != m.end() && it->first < b && out.size() < limit; ++it)
        out.emplace_back(it->first, it->second);
    return out;
}

static void expectSame(const DocScan& got, const DocScan& want, const std::string& what) {
    if (got == want) return;
    std::cerr << what << ": " << got.size() << " results, expected " << want.size() << "\n";
    for (size_t i = 0; i < std::min(got.size(), want.size()); ++i)
        if (got[i] != want[i]) {
            std::cerr << "  first difference at " << i << ": key " << got[i].first << " vs "
                      << want[i].first << "\n";
            break;
        }
    CHECK(got == want);
}

// scanDocuments == scanSlow == model, for one range.
static void compareRange(Fixture& f, const std::string& a, const std::string& b, size_t limit,
                         const std::string& what) {
    const DocScan fast = f.st->scanDocuments(a, b, limit);
    const DocScan slow = f.st->scanSlow(a, b, limit);
    expectSame(fast, slow, what + " (scanDocuments vs scanSlow)");
    expectSame(fast, modelScan(f.model, a, b, limit), what + " (scanDocuments vs model)");
}

static std::set<uint32_t> segmentsOf(IndexedStore& st, const std::string& a,
                                     const std::string& b, size_t limit) {
    std::set<uint32_t> s;
    for (const auto& kv : st.scan(a, b, limit)) s.insert(kv.second.vptr.segment_id);
    return s;
}

static uint16_t depthOf(IndexedStore& st, const std::string& k) {
    bpt::ReadStats rs;
    CHECK(st.get(k, nullptr, &rs).has_value());
    return rs.chain_depth;
}

// What scanSlow() reports for [a, b): get()'s error for the first key (in key order) that fails.
static std::string firstGetError(IndexedStore& st, const std::string& a, const std::string& b);

// Message of the VLogReadError fn throws ("" if it throws none).
template <typename F> static std::string readErrorOf(F fn) {
    try {
        fn();
    } catch (const bpt::VLogReadError& e) {
        return e.what();
    }
    return "";
}

static std::string firstGetError(IndexedStore& st, const std::string& a, const std::string& b) {
    for (const auto& kv : st.scan(a, b, 1000000)) {
        const std::string e = readErrorOf([&] { (void)st.get(kv.first); });
        if (!e.empty()) return e;
    }
    return "";
}

static void flipByte(const std::string& path, uint64_t off) {
    const int fd = ::open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    uint8_t b = 0;
    CHECK(::pread(fd, &b, 1, static_cast<off_t>(off)) == 1);
    b ^= 0x5a;
    CHECK(::pwrite(fd, &b, 1, static_cast<off_t>(off)) == 1);
    CHECK(::close(fd) == 0);
    accountBytes(1, "corrupted byte");
}

static std::string segPath(IndexedStore& st, uint32_t id) {
    char name[32];
    std::snprintf(name, sizeof(name), "/vlog_%08u.log", id);
    return st.vlogDir() + name;
}

// ------------------------------------------------------------------------------------------
// SC1 random ranges and edge cases
// ------------------------------------------------------------------------------------------
static void testSC1RandomRanges(const std::string& dir) {
    banner("SC1 scanDocuments == scanSlow == model on random ranges");
    Fixture f;
    f.st = IndexedStore::open(dir, storeOpts());
    const int n = kQuick ? 160 : 1200;
    std::mt19937 rng(71);
    for (int i = 0; i < n; ++i) f.put(i, 0, 60 + rng() % 400);
    // Chains of depth 1..4 on keys 0..3 (key i gets i + 1 updates).
    for (int i = 0; i < 4; ++i)
        for (int u = 0; u <= i; ++u) f.inc(i);
    for (int i = 0; i < 4; ++i) CHECK(depthOf(*f.st, key(i)) == i + 1);
    // Random updates (more chains, some consolidated), erases and re-puts.
    const int ops = kQuick ? 260 : 3000;
    for (int o = 0; o < ops; ++o) {
        const int id = 4 + static_cast<int>(rng() % (n - 4));
        const int r = static_cast<int>(rng() % 10);
        if (f.model.count(key(id)) == 0) {
            f.put(id, o, 60 + rng() % 400);
        } else if (r < 7) {
            f.inc(id);
        } else if (r < 9) {
            f.erase(id);
        } else {
            f.put(id, o, 60 + rng() % 400);
        }
    }
    // Edge cases.
    compareRange(f, key(0), key(n), 100000, "whole range");
    compareRange(f, key(10), key(10), 100, "start == end");
    compareRange(f, key(20), key(10), 100, "start > end");
    compareRange(f, "z", "zz", 100, "range with no keys");
    compareRange(f, key(0), key(0) + std::string(1, '\0'), 100, "single key (depth 1 chain)");
    compareRange(f, key(3), key(4), 100, "single key (depth 4 chain)");
    compareRange(f, key(0), key(n), 0, "limit 0");
    compareRange(f, key(0), key(n), 1, "limit 1");
    compareRange(f, key(0), key(n), 100, "limit 100");
    compareRange(f, key(0), key(4), 100, "keys with chains of depth 1-4");
    // An erased key inside the range is skipped.
    f.erase(50);
    compareRange(f, key(45), key(56), 100, "range with an erased key");
    CHECK(segmentsOf(*f.st, key(0), key(n), 100000).size() >= 3);
    // Random ranges and limits.
    const int ranges = kQuick ? 150 : 1000;
    for (int r = 0; r < ranges; ++r) {
        const int a = static_cast<int>(rng() % (n + 10));
        const int b = a + static_cast<int>(rng() % 150);
        const size_t limit = rng() % 4 == 0 ? rng() % 3 : rng() % 160;
        compareRange(f, key(a), key(b), limit, "random range");
    }
    accountBytes(f.st->vlogBytesWritten(), "vLog");
    f.st->close();
    pass("SC1 " + std::to_string(ranges) +
         " random ranges + edge cases (empty, single key, limit 0/1/100, erased key, chains of "
         "depth 1-4, ranges across segments): scanDocuments == scanSlow == model");
}

// ------------------------------------------------------------------------------------------
// SC2 entries relocated by GC
// ------------------------------------------------------------------------------------------
static void testSC2Relocated(const std::string& dir) {
    banner("SC2 entries relocated by GC");
    Fixture f;
    f.st = IndexedStore::open(dir, storeOpts());
    const int n = kQuick ? 120 : 600;
    for (int i = 0; i < n; ++i) f.put(i, 0, 300);
    for (int i = 0; i < n; i += 5) f.inc(i); // chains whose base is in an old segment
    // Overwrite 80% of the keys: the old segments become mostly dead.
    for (int i = 0; i < n; ++i)
        if (i % 5 != 0) f.put(i, 1, 300);
    std::map<std::string, vlog::IndexValueRef> before;
    for (const auto& kv : f.st->scan(key(0), key(n), n)) before[kv.first] = kv.second;
    uint64_t relocated = 0;
    for (int r = 0; r < 100; ++r) {
        bpt::GcReport g = f.st->runGcPass();
        relocated += g.keys_relocated;
        if (g.action == bpt::GcReport::Action::None && !f.st->gcPending()) break;
        f.st->checkpoint();
    }
    CHECK(relocated > 0);
    int moved = 0;
    for (const auto& kv : f.st->scan(key(0), key(n), n)) {
        const vlog::IndexValueRef& old = before[kv.first];
        if (old.seq == kv.second.seq &&
            std::memcmp(&old.vptr, &kv.second.vptr, sizeof(vlog::VPtr)) != 0)
            moved++;
    }
    CHECK(moved > 0);
    compareRange(f, key(0), key(n), n, "after GC relocation");
    std::mt19937 rng(72);
    for (int r = 0; r < 50; ++r) {
        const int a = static_cast<int>(rng() % n);
        compareRange(f, key(a), key(a + 40), 100, "random range after GC");
    }
    accountBytes(f.st->vlogBytesWritten(), "vLog");
    f.st->close();
    pass("SC2 " + std::to_string(moved) +
         " keys whose entries GC relocated (same seq, new location): scanDocuments == "
         "scanSlow == model");
}

// ------------------------------------------------------------------------------------------
// SC3 checkpoint, reopen, range ending in the active segment
// ------------------------------------------------------------------------------------------
static void testSC3CheckpointReopenActive(const std::string& dir) {
    banner("SC3 checkpoint, reopen, range ending in the active segment");
    Fixture f;
    f.st = IndexedStore::open(dir, storeOpts());
    const int n = kQuick ? 100 : 500;
    for (int i = 0; i < n; ++i) f.put(i, 0, 200);
    for (int i = 0; i < n; i += 3) f.inc(i);
    f.st->checkpoint();
    compareRange(f, key(0), key(n), n, "after a checkpoint");
    for (int i = 0; i < n; i += 7) f.inc(i); // applied after the checkpoint (replayed at open)
    accountBytes(f.st->vlogBytesWritten(), "vLog");
    f.st->close();
    f.st = IndexedStore::open(dir, storeOpts());
    compareRange(f, key(0), key(n), n, "after reopen");
    // New keys at the end of the range land in the active segment (the writer opens a new one).
    for (int i = n; i < n + 5; ++i) f.put(i, 0, 200);
    f.inc(n + 4);
    uint32_t active = 0;
    for (const bpt::SegmentInfo& s : f.st->gcStats().segments)
        if (s.state == bpt::SegmentState::Active) active = s.id;
    CHECK(active != 0);
    CHECK(segmentsOf(*f.st, key(n - 20), key(n + 5), 100).count(active) == 1);
    compareRange(f, key(n - 20), key(n + 5), 100, "range ending in the active segment");
    compareRange(f, key(n + 4), key(n + 5), 100, "last key, in the active segment");
    accountBytes(f.st->vlogBytesWritten(), "vLog");
    f.st->close();
    pass("SC3 after a checkpoint, after reopen (replayed updates) and for a range ending in the "
         "active segment: scanDocuments == scanSlow == model");
}

// ------------------------------------------------------------------------------------------
// SC4 kill -9 and recovery
// ------------------------------------------------------------------------------------------
#ifndef TSAN_BUILD // fork + kill -9 are not run under ThreadSanitizer
// Deterministic workload; with f == nullptr only the model is built.
static void crashWorkload(Fixture* f, Model* model) {
    Fixture local;
    Fixture& x = f ? *f : local;
    const int n = kQuick ? 80 : 400;
    std::mt19937 rng(74);
    for (int i = 0; i < n; ++i) {
        const size_t pad = 100 + rng() % 200;
        if (f) {
            x.put(i, 0, pad);
        } else {
            x.model[key(i)] = makeDoc(i, 0, pad);
        }
    }
    for (int o = 0; o < n * 2; ++o) {
        const int id = static_cast<int>(rng() % n);
        if (x.model.count(key(id)) == 0) continue;
        const bool erase = rng() % 10 == 0;
        if (f) {
            if (erase) {
                x.erase(id);
            } else {
                x.inc(id);
            }
        } else if (erase) {
            x.model.erase(key(id));
        } else {
            doc::Value v = doc::parse(x.model[key(id)]);
            doc::applyOps(v, {doc::inc("score", 1)});
            x.model[key(id)] = doc::serialize(v);
        }
        if (f && o == n / 2) f->st->checkpoint(); // part replayed from the checkpoint
    }
    *model = x.model;
}
#endif

static void testSC4KillRecovery(const std::string& dir) {
    banner("SC4 kill -9, then recovery");
#ifdef TSAN_BUILD
    skip("SC4 kill -9 and recovery", "fork + kill -9 not run under ThreadSanitizer");
    (void)dir;
#else
    std::cout.flush();
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        childExpectsAbort();
        Fixture f;
        f.st = IndexedStore::open(dir, storeOpts());
        Model m;
        crashWorkload(&f, &m);
        accountBytes(f.st->vlogBytesWritten(), "crash child vLog");
        raise(SIGKILL); // every write above was acked: all of it must be there after recovery
        _exit(3);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    Fixture f;
    crashWorkload(nullptr, &f.model);
    f.st = IndexedStore::open(dir, storeOpts());
    const int n = kQuick ? 80 : 400;
    compareRange(f, key(0), key(n), n, "after kill -9 and recovery");
    std::mt19937 rng(75);
    for (int r = 0; r < 40; ++r) {
        const int a = static_cast<int>(rng() % n);
        compareRange(f, key(a), key(a + 30), 100, "random range after recovery");
    }
    accountBytes(f.st->vlogBytesWritten(), "vLog");
    f.st->close();
    pass("SC4 kill -9 after acked writes, reopen (replay from a checkpoint): scanDocuments == "
         "scanSlow == model of every acked write");
#endif
}

// ------------------------------------------------------------------------------------------
// SC5 corruption and a missing segment
// ------------------------------------------------------------------------------------------
static void testSC5Corruption(const std::string& dir) {
    banner("SC5 corruption: scanDocuments throws what get() throws");
    // 4 KiB segments: about 10 documents per segment.
    Fixture f;
    f.st = IndexedStore::open(dir, storeOpts(4 * 1024));
    const int n = 60;
    for (int i = 0; i < n; ++i) f.put(i, 0, 300);
    // Key 20: a chain of depth 2 whose Full base is in an older segment than its Deltas.
    const vlog::VPtr base20 = f.st->tree().get(key(20))->vptr;
    for (int i = 40; i < 50; ++i) f.put(i, 1, 300); // later segments
    f.inc(20);
    f.inc(20);
    CHECK(depthOf(*f.st, key(20)) == 2);
    compareRange(f, key(0), key(n), n, "before corruption");

    // (a) A byte inside the head entry of key 10 (in its payload).
    const vlog::VPtr p10 = f.st->tree().get(key(10))->vptr;
    flipByte(segPath(*f.st, p10.segment_id), p10.offset + p10.length - 16);
    const std::string e10 = readErrorOf([&] { (void)f.st->get(key(10)); });
    CHECK(!e10.empty());
    CHECK(readErrorOf([&] { (void)f.st->scanSlow(key(0), key(n), n); }) == e10);
    CHECK(readErrorOf([&] { (void)f.st->scanDocuments(key(0), key(n), n); }) == e10);
    CHECK(readErrorOf([&] { (void)f.st->scanDocuments(key(5), key(15), 100); }) == e10);
    // Ranges without key 10 still read (a neighbour in the same batched read is fine).
    f.model.erase(key(10)); // only so modelScan skips it below; the ranges exclude it
    compareRange(f, key(0), key(10), 100, "range just before the corrupt entry");
    compareRange(f, key(11), key(19), 100, "range just after the corrupt entry");

    CHECK(firstGetError(*f.st, key(0), key(n)) == e10);
    // (b) The Full base of key 20's chain (the Deltas are fine).
    flipByte(segPath(*f.st, base20.segment_id), base20.offset + base20.length - 16);
    const std::string e20 = readErrorOf([&] { (void)f.st->get(key(20)); });
    CHECK(!e20.empty() && e20 != e10);
    CHECK(readErrorOf([&] { (void)f.st->scanDocuments(key(15), key(25), 100); }) == e20);
    CHECK(readErrorOf([&] { (void)f.st->scanSlow(key(15), key(25), 100); }) == e20);
    // (c) Two corrupt keys in one range: the first in key order is reported.
    CHECK(readErrorOf([&] { (void)f.st->scanDocuments(key(0), key(n), n); }) == e10);

    // (d) A missing segment: unlink the segment of a key in [30, 40) that holds neither
    // corrupt entry. Several keys may live in it: the first in key order is reported.
    int victim = -1;
    vlog::VPtr pv{};
    for (int i = 30; i < 40 && victim < 0; ++i) {
        pv = f.st->tree().get(key(i))->vptr;
        if (pv.segment_id != p10.segment_id && pv.segment_id != base20.segment_id) victim = i;
    }
    CHECK(victim >= 0);
    CHECK(::unlink(segPath(*f.st, pv.segment_id).c_str()) == 0);
    CHECK(!readErrorOf([&] { (void)f.st->get(key(victim)); }).empty());
    const std::string ed = firstGetError(*f.st, key(25), key(victim + 1));
    CHECK(!ed.empty());
    CHECK(readErrorOf([&] { (void)f.st->scanDocuments(key(25), key(victim + 1), 100); }) == ed);
    CHECK(readErrorOf([&] { (void)f.st->scanSlow(key(25), key(victim + 1), 100); }) == ed);
    accountBytes(f.st->vlogBytesWritten(), "vLog");
    f.st.reset(); // no close(): the checkpoint would not mind, but the store is damaged on purpose
    pass("SC5 flipped byte in a head entry, in a chain's Full base, two corrupt keys, missing "
         "segment: scanDocuments throws get()'s VLogReadError for the first bad key");
}

// ------------------------------------------------------------------------------------------
// SC6 scans during 8 writer threads and GC
// ------------------------------------------------------------------------------------------
static void testSC6Concurrent(const std::string& dir) {
    banner("SC6 scans while 8 writers update and GC runs");
    IndexedStore::Options o = storeOpts();
    o.gc_enabled = true;
    o.gc_interval_ms = 5;
    o.auto_checkpoint = true;
    o.checkpoint_bytes = 64 * 1024; // checkpoints move retired segments on to unlink
    o.checkpoint_interval_ms = 50;
    auto st = IndexedStore::open(dir, o);
    const int n = kQuick ? 96 : 400;     // keys; key i belongs to writer i % 8
    const int erase_from = n - n / 8;    // keys [erase_from, n) are erased once by their writer
    for (int i = 0; i < n; ++i) st->put(key(i), makeDoc(i, 0, 150));
    std::vector<std::atomic<int64_t>> acked(n), started(n);
    std::vector<std::atomic<bool>> erase_started(n), erase_acked(n);
    for (int i = 0; i < n; ++i) {
        acked[i] = 0;
        started[i] = 0;
        erase_started[i] = false;
        erase_acked[i] = false;
    }
    const int ops_per_writer = kQuick ? 60 : 600;
    std::atomic<int> writers_left{8};
    std::atomic<uint64_t> scans{0}, docs_checked{0};
    std::atomic<int> bad{0};
    std::vector<std::thread> th;
    for (int w = 0; w < 8; ++w)
        th.emplace_back([&, w] {
            std::mt19937 rng(100 + w);
            std::vector<int> mine;
            for (int i = w; i < n; i += 8) mine.push_back(i);
            for (int op = 0; op < ops_per_writer; ++op) {
                if (op == ops_per_writer / 2) {
                    for (int i : mine) {
                        if (i < erase_from) continue;
                        erase_started[i] = true; // before the erase: a scan may miss it from now
                        st->erase(key(i));
                        erase_acked[i] = true;
                    }
                }
                const int i = mine[rng() % mine.size()];
                if (erase_started[i].load()) continue;
                const int64_t next = started[i].load() + 1;
                started[i] = next; // before the write: a scan may see it from now on
                if (rng() % 6 == 0) {
                    st->put(key(i), makeDoc(i, next, 150)); // a new Full entry ends the chain
                } else {
                    CHECK(st->update(key(i), {doc::inc("score", 1)}).written);
                }
                acked[i] = next;
            }
            writers_left--;
        });
    for (int s = 0; s < 2; ++s)
        th.emplace_back([&, s] {
            std::mt19937 rng(200 + s);
            while (writers_left.load() > 0) {
                const int a = static_cast<int>(rng() % n);
                const int b = std::min(n, a + 1 + static_cast<int>(rng() % 60));
                std::vector<int64_t> lo(b - a);
                std::vector<bool> gone_before(b - a);
                for (int i = a; i < b; ++i) {
                    lo[i - a] = acked[i].load();
                    gone_before[i - a] = erase_acked[i].load();
                }
                const bool batched = (scans.load() + s) % 4 != 0; // mostly scanDocuments
                const DocScan res = batched ? st->scanDocuments(key(a), key(b), 100)
                                            : st->scanSlow(key(a), key(b), 100);
                std::set<int> seen;
                for (const auto& [k, d] : res) {
                    const int i = keyId(k);
                    if (i < a || i >= b || seen.count(i)) bad++;
                    seen.insert(i);
                    const doc::Value v = doc::parse(d); // a half-written document would not parse
                    const int64_t score = v.find("score")->asInt();
                    if (v.find("id")->asInt() != i) bad++;
                    if (score < lo[i - a] || score > started[i].load()) bad++;
                    if (gone_before[i - a]) bad++; // erased before the scan started
                    docs_checked++;
                }
                for (int i = a; i < b; ++i)
                    if (!seen.count(i) && !erase_started[i].load()) bad++; // never absent
                for (size_t x = 1; x < res.size(); ++x)
                    if (!(res[x - 1].first < res[x].first)) bad++;
                scans++;
            }
        });
    for (auto& t : th) t.join();
    CHECK(bad.load() == 0);
    CHECK(scans.load() > 0);
    const bpt::GcStats g = st->gcStats();
    // Final state: both scans equal the writers' last acked versions.
    Model m;
    for (int i = 0; i < n; ++i) {
        if (erase_acked[i].load()) continue;
        std::optional<std::string> d = st->get(key(i));
        CHECK(d && scoreOf(*d) == acked[i].load());
        m[key(i)] = *d;
    }
    const DocScan fast = st->scanDocuments(key(0), key(n), n);
    expectSame(fast, st->scanSlow(key(0), key(n), n), "SC6 final (scanDocuments vs scanSlow)");
    expectSame(fast, modelScan(m, key(0), key(n), n), "SC6 final (scanDocuments vs model)");
    accountBytes(st->vlogBytesWritten(), "vLog");
    st->close();
    pass("SC6 " + std::to_string(scans.load()) + " scans (" + std::to_string(docs_checked.load()) +
         " documents) during 8 writers + GC (" + std::to_string(g.passes) + " passes, " +
         std::to_string(g.relocated_entries) +
         " entries relocated): every document committed and current during the scan, erased keys "
         "absent, present keys never missing");
}

// ------------------------------------------------------------------------------------------
// SC7 the pin is held for the whole call
// ------------------------------------------------------------------------------------------
static void testSC7PinHeld(const std::string& dir) {
    banner("SC7 scanDocuments holds its pin from the tree walk to the last read");
    std::mutex mu;
    std::condition_variable cv;
    bool reached = false, release = false, armed = true;
    IndexedStore::Options o = storeOpts(8 * 1024); // one ~3 KB document per segment
    o.on_scan_after_index_read = [&] {
        std::unique_lock<std::mutex> lk(mu);
        if (!armed) return;
        armed = false;
        reached = true;
        cv.notify_all();
        cv.wait(lk, [&] { return release; });
    };
    Fixture f;
    f.st = IndexedStore::open(dir, o);
    f.put(1, 0, 3000);
    const uint32_t x = f.st->tree().get(key(1))->vptr.segment_id;
    f.put(2, 0, 3000);
    const DocScan want = modelScan(f.model, key(0), key(9), 100); // the versions at the walk
    DocScan got;
    std::thread scanner([&] { got = f.st->scanDocuments(key(0), key(9), 100); });
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return reached; });
    }
    // The scanner holds a VPtr into segment x. Overwrite key 1 so x is dead, retire x, and let
    // more than two checkpoints pass: x must not be condemned while the scan runs.
    f.put(1, 1, 3000);
    f.put(3, 0, 3000);
    f.st->retireSegmentForTest(x);
    for (int i = 0; i < 3; ++i) {
        f.st->checkpoint();
        CHECK(f.st->advanceLifecycleForTest().condemned == 0);
    }
    struct stat sb;
    CHECK(::stat(segPath(*f.st, x).c_str(), &sb) == 0);
    {
        std::lock_guard<std::mutex> lk(mu);
        release = true;
    }
    cv.notify_all();
    scanner.join();
    expectSame(got, want, "SC7 scan across the retirement");
    CHECK(f.st->activeReadPinsForTest() == 0);
    // Released: now x is condemned, then unlinked.
    CHECK(f.st->advanceLifecycleForTest().condemned == 1);
    f.st->checkpoint();
    CHECK(f.st->advanceLifecycleForTest().unlinked == 1);
    CHECK(::stat(segPath(*f.st, x).c_str(), &sb) != 0);
    compareRange(f, key(0), key(9), 100, "after the unlink");
    accountBytes(f.st->vlogBytesWritten(), "vLog");
    f.st->close();
    pass("SC7 a segment retired during a scan is not condemned until the scan returns; the scan "
         "returns the versions of its tree walk; unlinked afterwards");
}

int main() {
    std::string dir = setupRun("scan_test");
    Cleaner cleaner{dir};
    testSC1RandomRanges(dir + "/sc1");
    testSC2Relocated(dir + "/sc2");
    testSC3CheckpointReopenActive(dir + "/sc3");
    testSC4KillRecovery(dir + "/sc4");
    testSC5Corruption(dir + "/sc5");
    testSC6Concurrent(dir + "/sc6");
    testSC7PinHeld(dir + "/sc7");
    printBudget();
    return 0;
}
