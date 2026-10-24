// Phase 2 edge-case audit. Each case runs in its own forked child, so one failing case
// (CHECK failure or abort) does not hide the others. The parent prints a result table and
// exits non-zero if any case failed.
#include "test_common.hpp"

#include "index/BPTree.hpp"
#include "index/IndexRecovery.hpp"
#include "index/IndexedStore.hpp"
#include "index/Pager.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using bpt::BPTree;
using bpt::IndexedStore;
using bpt::OpenStatus;
using bpt::Pager;
using Ref = vlog::IndexValueRef;
using Oracle = std::map<std::string, Ref>;

// ------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------
static IndexedStore::Options storeOpts(size_t frames = 256) {
    IndexedStore::Options o;
    o.cache_frames = frames;
    o.vlog_opts = vlog::VLogWriter::Options(64 * 1024 * 1024, std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index.db page write"); };
    return o;
}

static bpt::SuperblockManager::Options sbOpts() {
    bpt::SuperblockManager::Options o;
    o.on_write = [](uint64_t n) { accountBytes(n, "superblock.db write"); };
    return o;
}

static Pager::Options pagerOpts(size_t frames) {
    Pager::Options o;
    o.num_frames = frames;
    o.on_write = [](uint64_t n) { accountBytes(n, "index.db page write"); };
    return o;
}

// Closes (clean = true) or drops the store, and accounts its vLog bytes.
static void finishStore(std::unique_ptr<IndexedStore>& st, bool clean) {
    uint64_t vb = st->vlogBytesWritten();
    if (clean) st->close();
    st.reset();
    accountBytes(vb, "IndexedStore vLog writes");
}

static uint64_t fileSize(const std::string& path) {
    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0);
    return static_cast<uint64_t>(st.st_size);
}

static std::string segPath(const std::string& vdir, uint32_t id) {
    char b[32];
    std::snprintf(b, sizeof(b), "/vlog_%08u.log", id);
    return vdir + b;
}

static void copyFile(const std::string& from, const std::string& to) {
    std::error_code ec;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    accountBytes(fileSize(to), "test file copy");
}

static void pwriteByte(const std::string& path, uint64_t off, uint8_t xor_mask) {
    int fd = open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    uint8_t b = 0;
    CHECK(pread(fd, &b, 1, static_cast<off_t>(off)) == 1);
    b ^= xor_mask;
    CHECK(pwrite(fd, &b, 1, static_cast<off_t>(off)) == 1);
    accountBytes(1, "test pwrite (corruption)");
    CHECK(vlog::full_fsync(fd) == 0);
    CHECK(close(fd) == 0);
}

// Reference: replay the vLog into a std::map with should_apply().
static Oracle vlogOracle(const std::string& vdir) {
    Oracle o;
    std::vector<uint32_t> segs = bpt::listVLogSegments(vdir);
    vlog::VLogReader reader(vdir);
    for (size_t i = 0; i < segs.size(); ++i) {
        vlog::ScanResult sr = reader.scanSegment(segs[i], i + 1 == segs.size());
        for (const auto& e : sr.entries) {
            auto it = o.find(e.key);
            bool has = it != o.end();
            const uint64_t seq = e.header.seq;
            if (!vlog::should_apply(has, has ? it->second.seq : 0, seq, e.header.flags)) continue;
            if (e.header.type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) {
                o.erase(e.key);
            } else {
                Ref r{};
                r.seq = seq;
                r.kind = 1;
                r.vptr = e.vptr;
                o[e.key] = r;
            }
        }
    }
    return o;
}

static void checkIndexEqualsVLog(IndexedStore& st, const char* what) {
    Oracle ref = vlogOracle(st.vlogDir());
    BPTree::ScanResult got = st.tree().scanFrom("", SIZE_MAX);
    if (got.size() != ref.size()) {
        std::cerr << what << ": index has " << got.size() << " keys, vLog replay has " << ref.size() << "\n";
        CHECK(got.size() == ref.size());
    }
    size_t i = 0;
    for (const auto& [k, r] : ref) {
        CHECK(got[i].first == k && got[i].second.seq == r.seq);
        ++i;
    }
    std::string err = st.tree().checkInvariants();
    if (!err.empty()) std::cerr << err << "\n";
    CHECK(err.empty());
}

// "Fail loudly or serve only what is in the vLog": open must throw, or the index must equal the
// vLog replay. Returns a short description of what happened.
static std::string openMustNotServeGhosts(const std::string& dir) {
    std::unique_ptr<IndexedStore> st;
    try {
        st = IndexedStore::open(dir, storeOpts());
    } catch (const std::exception& e) {
        return std::string("open threw: ") + e.what();
    }
    checkIndexEqualsVLog(*st, "open after vLog change");
    std::string r = std::string("opened; first index open ") + bpt::toString(st->openInfo().initial_status) +
                    ", rebuilt " + (st->openInfo().rebuilt ? "yes" : "no") + ", keys " +
                    std::to_string(st->tree().size()) + " == vLog replay";
    finishStore(st, true);
    return r;
}

static void putKeys(IndexedStore& st, const std::string& prefix, int n) {
    for (int i = 0; i < n; ++i) st.put(prefix + std::to_string(i), "doc-" + prefix + std::to_string(i));
}

// ------------------------------------------------------------------------------------------
// Case runner: fork per case
// ------------------------------------------------------------------------------------------
struct CaseResult {
    std::string name;
    bool passed;
    std::string detail;
};
static std::vector<CaseResult> g_results;

static void runCase(const std::string& name, const std::function<void()>& fn) {
    banner(name);
    std::cout.flush();
    std::cerr.flush();
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        childExpectsAbort();  // a failing case must not print the parent's "rm -rf" message
        fn();
        std::cout.flush();
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::string detail = ok ? "exit 0"
                            : WIFSIGNALED(status) ? "killed by signal " + std::to_string(WTERMSIG(status))
                                                  : "exit " + std::to_string(WEXITSTATUS(status));
    std::cout << (ok ? "CASE PASS: " : "CASE FAIL: ") << name << " (" << detail << ")\n";
    g_results.push_back({name, ok, detail});
}

// ------------------------------------------------------------------------------------------
// 1. Index ahead of the vLog
// ------------------------------------------------------------------------------------------
static void case1IndexAheadOfVLog(const std::string& base) {
    CHECK(mkdir(base.c_str(), 0755) == 0);
    // 1a: vLog directory deleted.
    {
        std::string d = base + "/missing";
        auto st = IndexedStore::open(d, storeOpts());
        putKeys(*st, "a", 50);
        finishStore(st, true);
        std::filesystem::remove_all(d + "/vlog");
        std::cout << "1a vLog dir deleted: " << openMustNotServeGhosts(d) << "\n";
    }
    // 1b: vLog directory present but empty.
    {
        std::string d = base + "/empty";
        auto st = IndexedStore::open(d, storeOpts());
        putKeys(*st, "b", 50);
        finishStore(st, true);
        for (uint32_t id : bpt::listVLogSegments(d + "/vlog")) CHECK(unlink(segPath(d + "/vlog", id).c_str()) == 0);
        std::cout << "1b vLog dir emptied: " << openMustNotServeGhosts(d) << "\n";
    }
    // 1c: vLog max seq < superblock checkpoint_seq (newest segment deleted). Must also hold on a second open.
    {
        std::string d = base + "/behind";
        auto st = IndexedStore::open(d, storeOpts());
        putKeys(*st, "c", 30);  // segment 1
        finishStore(st, true);
        st = IndexedStore::open(d, storeOpts());
        putKeys(*st, "d", 30);  // segment 2
        finishStore(st, true);
        bpt::SuperblockManager sbm(d + "/superblock.db");
        bpt::SuperblockManager::LoadResult sl = sbm.load();
        CHECK(sl.status == bpt::SuperblockManager::LoadResult::Status::Ok);
        CHECK(unlink(segPath(d + "/vlog", 2).c_str()) == 0);
        std::cout << "1c newest segment deleted (superblock checkpoint_seq " << sl.current.sb.checkpoint_seq
                  << ", vLog max seq 30): " << openMustNotServeGhosts(d) << "\n";
        std::cout << "1c second open: " << openMustNotServeGhosts(d) << "\n";
    }
}

// ------------------------------------------------------------------------------------------
// 2. index.db paired with a different vLog
// ------------------------------------------------------------------------------------------
static void case2WrongVLog(const std::string& base) {
    CHECK(mkdir(base.c_str(), 0755) == 0);
    std::string a = base + "/A", b = base + "/B";
    auto sa = IndexedStore::open(a, storeOpts());
    putKeys(*sa, "alpha", 40);
    finishStore(sa, true);
    auto sb = IndexedStore::open(b, storeOpts());
    putKeys(*sb, "beta", 25);
    finishStore(sb, true);
    copyFile(a + "/index.db", b + "/index.db");  // B's directory now holds A's clean index
    std::cout << "A's index.db with B's vLog: " << openMustNotServeGhosts(b) << "\n";
    // B must now return B's documents and none of A's keys.
    auto s = IndexedStore::open(b, storeOpts());
    CHECK(!s->get("alpha0").has_value());
    CHECK(s->get("beta0") == std::optional<std::string>("doc-beta0"));
    finishStore(s, true);
}

// ------------------------------------------------------------------------------------------
// 3. get() when the VPtr points to a missing segment or a corrupt entry
// ------------------------------------------------------------------------------------------
static int expectGetError(IndexedStore& st, const std::string& key, const char* what) {
    try {
        std::optional<std::string> doc = st.get(key);
        std::cerr << what << ": get returned " << (doc ? "a document" : "nullopt") << " instead of an error\n";
        CHECK(false);
    } catch (const std::invalid_argument&) {
        CHECK(false);
    } catch (const std::exception& e) {
        std::cout << "  " << what << ": get threw: " << e.what() << "\n";
    }
    return 1;
}

static void case3GetErrors(const std::string& base) {
    std::string d = base;
    auto st = IndexedStore::open(d, storeOpts());
    putKeys(*st, "early", 5);  // segment 1
    finishStore(st, true);
    st = IndexedStore::open(d, storeOpts());
    putKeys(*st, "k", 20);  // segment 2
    finishStore(st, true);
    st = IndexedStore::open(d, storeOpts());  // writes would go to segment 3
    int errors = 0;

    // 3a: flip one payload byte of k5's entry.
    Ref r5 = *st->tree().get("k5");
    pwriteByte(segPath(st->vlogDir(), r5.vptr.segment_id), r5.vptr.offset + r5.vptr.length - 9, 0x40);
    errors += expectGetError(*st, "k5", "3a corrupt payload byte");
    CHECK(st->get("k6") == std::optional<std::string>("doc-k6"));

    // 3b: index entries pointing at wrong places.
    Ref r8 = *st->tree().get("k8");
    Ref r9 = *st->tree().get("k9");
    CHECK(st->tree().put("k7", r8) == BPTree::PutStatus::Updated);  // points at another key's entry
    errors += expectGetError(*st, "k7", "3b VPtr to another key's entry");
    Ref bad = r9;
    bad.seq += 1000;
    CHECK(st->tree().put("k9", bad) == BPTree::PutStatus::Updated);  // seq mismatch
    errors += expectGetError(*st, "k9", "3c seq mismatch");
    bad = r8;
    bad.vptr.offset += 8;
    CHECK(st->tree().put("k10", bad) == BPTree::PutStatus::Updated);  // middle of an entry
    errors += expectGetError(*st, "k10", "3d VPtr into the middle of an entry");
    bad = r8;
    bad.vptr.length += 8;
    CHECK(st->tree().put("k11", bad) == BPTree::PutStatus::Updated);  // length mismatch
    errors += expectGetError(*st, "k11", "3e VPtr length mismatch");
    bad = r8;
    bad.vptr.segment_id = 999;
    CHECK(st->tree().put("k12", bad) == BPTree::PutStatus::Updated);  // segment does not exist
    errors += expectGetError(*st, "k12", "3f VPtr to a segment that does not exist");

    // 3g: segment 2 deleted while the store is open.
    CHECK(unlink(segPath(st->vlogDir(), 2).c_str()) == 0);
    errors += expectGetError(*st, "k1", "3g segment file deleted");
    CHECK(st->get("early1") == std::optional<std::string>("doc-early1"));  // segment 1 still served
    finishStore(st, false);
    std::cout << errors << " bad references returned errors; valid keys in other entries still read correctly.\n";
}

// ------------------------------------------------------------------------------------------
// 4. Document size limits
// ------------------------------------------------------------------------------------------
static void case4DocSize(const std::string& d) {
    // No automatic checkpoints: the 16 MiB put would cross the 4 MiB trigger, and the crash below
    // must leave that entry for recovery to replay.
    IndexedStore::Options o4 = storeOpts();
    o4.auto_checkpoint = false;
    auto st = IndexedStore::open(d, o4);
    const std::string key = "big";  // 3 bytes -> padded to 8
    st->put("empty", "");
    CHECK(st->get("empty") == std::optional<std::string>(""));
    // Largest doc with entry_size(key, doc) == kMaxEntryLen: 16 MiB - 32 (header) - 8 (padded key).
    const size_t max_doc = vlog::kMaxEntryLen - sizeof(vlog::EntryHeader) - vlog::align_up(3, vlog::kEntryAlign);
    uint64_t before = st->vlogBytesWritten();
    int rejected = 0;
    for (size_t n : {max_doc + 1, max_doc + 8, size_t(vlog::kMaxEntryLen), size_t(vlog::kMaxEntryLen) + 1,
                     size_t(64) * 1024 * 1024}) {
        std::string doc(n, 'x');
        try {
            st->put(key, doc);
            std::cerr << "doc of " << n << " bytes was accepted\n";
            CHECK(false);
        } catch (const std::invalid_argument&) {
            rejected++;
        }
    }
    CHECK(st->vlogBytesWritten() == before);
    CHECK(!st->get(key).has_value());
    std::cout << "rejected " << rejected << " docs above the limit (" << max_doc + 1 << " .. 64 MiB); vLog bytes "
              << "unchanged.\n";
    if (!kQuick) {
        std::string doc(max_doc, 'm');
        doc[0] = 'A';
        doc[max_doc - 1] = 'Z';
        uint64_t s = st->put(key, doc);
        CHECK(st->get(key) == std::optional<std::string>(doc));
        finishStore(st, false);  // crash: recovery replays the vLog, which must read the 16 MiB entry
        st = IndexedStore::open(d, o4);
        CHECK(!st->openInfo().rebuilt && st->openInfo().vlog_entries_applied >= 1);
        uint64_t seq = 0;
        CHECK(st->get(key, &seq) == std::optional<std::string>(doc) && seq == s);
        std::cout << "doc of " << max_doc << " bytes (entry exactly 16 MiB) accepted, read back, and recovered by "
                  << "vLog replay after a crash.\n";
    } else {
        std::cout << "[quick] skipped writing the 16 MiB document.\n";
    }
    finishStore(st, true);
}

// ------------------------------------------------------------------------------------------
// 5. Two threads put the same key at the same time
// ------------------------------------------------------------------------------------------
static void case5SameKeyRace(const std::string& d) {
    const int kRounds = kQuick ? 40 : 150;
    auto st = IndexedStore::open(d, storeOpts());
    struct W { uint64_t seq; std::string doc; };
    std::vector<W> res(2 * kRounds);
    for (int r = 0; r < kRounds; ++r) {
        std::string key = "race:" + std::to_string(r);
        std::atomic<int> ready{0};
        auto fn = [&](int t) {
            std::string doc = "round " + std::to_string(r) + " thread " + std::to_string(t);
            ready++;
            while (ready.load() < 2) {}
            res[2 * r + t] = W{st->put(key, doc), doc};
        };
        std::thread a(fn, 0), b(fn, 1);
        a.join();
        b.join();
    }
    auto verify = [&](IndexedStore& s, const char* when) {
        int t1_won = 0;
        for (int r = 0; r < kRounds; ++r) {
            const W& x = res[2 * r];
            const W& y = res[2 * r + 1];
            CHECK(x.seq != y.seq);
            const W& win = x.seq > y.seq ? x : y;
            if (&win == &y) t1_won++;
            uint64_t seq = 0;
            CHECK(s.get("race:" + std::to_string(r), &seq) == std::optional<std::string>(win.doc));
            CHECK(seq == win.seq);
        }
        std::cout << when << ": all " << kRounds << " keys hold the higher-seq value (thread 1 had the higher seq in "
                  << t1_won << " rounds).\n";
    };
    verify(*st, "live");
    finishStore(st, true);
    st = IndexedStore::open(d, storeOpts());
    CHECK(!st->openInfo().rebuilt);
    verify(*st, "after clean reopen");
    finishStore(st, false);
    st = IndexedStore::open(d, storeOpts());
    CHECK(!st->openInfo().rebuilt);
    verify(*st, "after crash reopen (superblock + vLog replay)");
    finishStore(st, true);
}

// ------------------------------------------------------------------------------------------
// 6. Concurrent get + put on one key: old or new value, never a mix
// ------------------------------------------------------------------------------------------
static std::string versionDoc(uint32_t v) {
    uint32_t len = 64 + (v * 2654435761u) % (kQuick ? 4096 : 16384);
    std::string head = "V" + std::to_string(v) + "|" + std::to_string(len) + "|";
    std::string tail = "|E" + std::to_string(v);
    std::string d = head;
    d.append(len - head.size() - tail.size(), static_cast<char>('a' + v % 26));
    return d + tail;
}

// Returns the version if the doc is exactly versionDoc(v) for the v in its header, else -1.
static long parseVersionDoc(const std::string& d) {
    if (d.size() < 3 || d[0] != 'V') return -1;
    long v = std::strtol(d.c_str() + 1, nullptr, 10);
    if (v < 0) return -1;
    return versionDoc(static_cast<uint32_t>(v)) == d ? v : -1;
}

static void case6GetPutMix(const std::string& d) {
    auto st = IndexedStore::open(d, storeOpts());
    const uint32_t kVersions = kQuick ? 120 : 400;
    st->put("hot", versionDoc(0));
    std::atomic<bool> done{false};
    std::atomic<uint64_t> reads{0}, bad{0}, backwards{0};
    std::vector<std::set<long>> seen(3);
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&, t]() {
            long last = -1;
            while (!done.load()) {
                std::optional<std::string> doc = st->get("hot");
                if (!doc) { bad++; continue; }
                long v = parseVersionDoc(*doc);
                if (v < 0) bad++;
                if (v < last) backwards++;
                last = std::max(last, v);
                seen[t].insert(v);
                reads++;
            }
        });
    }
    for (uint32_t v = 1; v <= kVersions; ++v) st->put("hot", versionDoc(v));
    done = true;
    for (auto& r : readers) r.join();
    CHECK(bad.load() == 0 && backwards.load() == 0);
    CHECK(parseVersionDoc(*st->get("hot")) == kVersions);
    size_t distinct = 0;
    for (auto& s : seen) distinct = std::max(distinct, s.size());
    std::cout << kVersions << " versions written (64 B - " << (kQuick ? 4 : 16) << " KiB); " << reads
              << " concurrent gets, 0 mixed or invalid docs, 0 version regressions; most distinct versions seen by "
                 "one reader: " << distinct << "\n";
    CHECK(distinct > 1);
    finishStore(st, true);
}

// ------------------------------------------------------------------------------------------
// 7. scan edge cases
// ------------------------------------------------------------------------------------------
static void case7ScanEdges(const std::string& d) {
    CHECK(mkdir(d.c_str(), 0755) == 0);
    auto p = Pager::create(d + "/index.db", pagerOpts(64));
    BPTree t(*p);
    std::string k256(256, 'q');
    CHECK(t.scan("", k256, 10).empty());
    CHECK(t.scanFrom("", 10).empty());
    CHECK(t.scan("b", "a", 10).empty());
    std::cout << "empty tree: scan and scanFrom return nothing\n";

    // Keys: 256-byte keys that differ only in the last byte, every prefix of them, and 0x00/0xFF tails.
    Oracle o;
    std::mt19937_64 rng(77);
    uint64_t seq = 0;
    auto add = [&](const std::string& k) { Ref r{}; r.seq = ++seq; r.kind = 1; t.put(k, r); o[k] = r; };
    std::string base(255, 'q');
    for (int c : {0x00, 0x01, 0x41, 0x7F, 0x80, 0xFE, 0xFF}) add(base + static_cast<char>(c));
    for (size_t len = 1; len <= 255; len += 7) add(std::string(len, 'q'));
    add(std::string(255, 'q'));  // the exact 255-byte prefix of the 256-byte keys
    add(std::string(256, '\x00'));
    add(std::string(256, '\xff'));
    for (int i = 0; i < 300; ++i) {
        std::string k(1 + rng() % 256, '\0');
        for (auto& c : k) c = static_cast<char>(rng());
        add(k);
    }
    BPTree::Shape sh;
    CHECK(t.checkInvariants(&sh).empty());
    auto expect = [&](const std::string& a, const std::string& b, size_t limit) {
        BPTree::ScanResult got = t.scan(a, b, limit);
        std::vector<std::string> want;
        if (limit > 0 && a < b) {
            for (auto it = o.lower_bound(a); it != o.end() && it->first < b && want.size() < limit; ++it) want.push_back(it->first);
        }
        CHECK(got.size() == want.size());
        for (size_t i = 0; i < got.size(); ++i) CHECK(got[i].first == want[i]);
        return got.size();
    };
    int n = 0;
    std::string lo = base + '\x00', hi = base + '\xff';
    CHECK(expect(lo, hi, 100) == 6); n++;                      // [..00, ..FF): 6 keys
    CHECK(expect(lo, hi + '\x00', 100) == 7); n++;             // end just above the largest 256-byte key
    CHECK(expect(hi, lo, 100) == 0); n++;                      // start > end
    CHECK(expect(lo, lo, 100) == 0); n++;                      // start == end (both 256 bytes)
    CHECK(expect(lo, hi, 0) == 0); n++;                        // limit 0
    CHECK(expect(lo, hi, 1) == 1); n++;                        // limit 1
    CHECK(expect(std::string(255, 'q'), lo, 100) == 1); n++;   // the 255-byte prefix sorts before its extensions
    expect("", std::string(300, '\xff'), SIZE_MAX); n++;       // end bound longer than 256 bytes: whole tree
    expect(std::string(300, 'q'), std::string(301, 'q'), 100); n++;  // bounds longer than any key
    for (int i = 0; i < 200; ++i) {
        std::string a(1 + rng() % 257, '\0'), b(1 + rng() % 257, '\0');
        for (auto& c : a) c = static_cast<char>(rng());
        for (auto& c : b) c = static_cast<char>(rng());
        expect(a, b, rng() % 40);
        n++;
    }
    // scanFrom with limit 0 and from above the largest key.
    CHECK(t.scanFrom("", 0).empty());
    CHECK(t.scanFrom(std::string(257, '\xff'), 10).empty());
    std::cout << o.size() << " keys (height " << sh.height << "); " << n + 2
              << " scans matched std::map, including 256-byte bounds and keys.\n";
    // Store level: scan on an empty store.
    auto st = IndexedStore::open(d + "/store", storeOpts());
    CHECK(st->scan("", std::string(256, '\xff'), 100).empty());
    CHECK(st->scan("z", "a", 100).empty());
    finishStore(st, true);
    std::cout << "IndexedStore::scan on an empty store returns nothing.\n";
}

// ------------------------------------------------------------------------------------------
// 8. Height 4 with no eviction
// ------------------------------------------------------------------------------------------
static void case8Height4(const std::string& d) {
    CHECK(mkdir(d.c_str(), 0755) == 0);
    const int kKeys = 200000;
    const size_t kFrames = 8192;  // 32 MiB: the whole tree stays cached
    uint64_t bytes0 = bytesWritten();
    auto key = [](int i) { char b[40]; std::snprintf(b, sizeof(b), "seq-%028d", i); return std::string(b); };  // 32 bytes
    auto t0 = std::chrono::steady_clock::now();
    {
        auto p = Pager::create(d + "/index.db", pagerOpts(kFrames));
        BPTree t(*p);
        for (int i = 0; i < kKeys; ++i) {
            Ref r{};
            r.seq = static_cast<uint64_t>(i) + 1;
            r.kind = 1;
            r.vptr.offset = static_cast<uint32_t>(i);
            CHECK(t.put(key(i), r) == BPTree::PutStatus::Inserted);
        }
        BPTree::Shape sh;
        std::string err = t.checkInvariants(&sh);
        if (!err.empty()) std::cerr << err << "\n";
        CHECK(err.empty());
        Pager::Stats s = p->stats();
        std::cout << kKeys << " sequential 32-byte keys: height " << sh.height << ", leaves " << sh.leaves
                  << ", internal " << sh.internals << ", pages " << p->pageCount() << ", evictions " << s.evictions
                  << ", page writes so far " << s.page_writes << "\n";
        CHECK(sh.height == 4 && sh.keys == static_cast<uint64_t>(kKeys));
        CHECK(s.evictions == 0);
        if (kQuick) {
            std::cout << "[quick] skipped close + reopen (would write about " << p->pageCount() * 4096 / (1 << 20)
                      << " MiB).\n";
            return;
        }
        bpt::SuperblockManager sbm(d + "/superblock.db", sbOpts());
        sbm.load();
        p->checkpoint(sbm, Pager::CommitInfo{});
    }
    bpt::OpenIndexResult r = bpt::openCommittedIndex(d, pagerOpts(kFrames));
    CHECK(r.initial_status == OpenStatus::Ok);
    Pager* p = r.pager.get();
    BPTree t(*p);
    BPTree::Shape sh;
    CHECK(t.checkInvariants(&sh).empty());
    CHECK(sh.height == 4 && sh.keys == static_cast<uint64_t>(kKeys));
    BPTree::ScanResult all = t.scanFrom("", SIZE_MAX);
    CHECK(all.size() == static_cast<size_t>(kKeys));
    for (int i = 0; i < kKeys; ++i) CHECK(all[i].first == key(i) && all[i].second.seq == uint64_t(i) + 1);
    std::mt19937 rng(8);
    for (int i = 0; i < 2000; ++i) {
        int k = static_cast<int>(rng() % kKeys);
        auto r = t.get(key(k));
        CHECK(r && r->vptr.offset == static_cast<uint32_t>(k));
    }
    p->checkpoint(*r.sbm, Pager::CommitInfo{});
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "after clean close + reopen: height 4, all " << kKeys << " keys in order, 2000 random gets OK; "
              << "bytes written " << bytesWritten() - bytes0 << "; " << ms << " ms\n";
}

// ------------------------------------------------------------------------------------------
// 9. index.db write/fsync failure: abort, then recover with no lost acked writes
// ------------------------------------------------------------------------------------------
struct Ack { uint64_t seq; uint32_t id; uint32_t pad; };

// 150-byte keys: 600 of them need about 40+ leaves, more than the 16-frame cache, so evictions
// write pages during normal operation.
static std::string case9Key(uint32_t id) { std::string k = "f" + std::to_string(id); k.resize(150, '.'); return k; }

static void case9IoFailure(const std::string& base) {
    CHECK(mkdir(base.c_str(), 0755) == 0);
    for (int variant = 0; variant < 2; ++variant) {
        // 0: the 40th page pwrite fails during normal operation (evictions with a 16-frame cache).
        // 1: an fsync fails inside close() (after all writes were acked).
        const std::string d = base + (variant == 0 ? "/pwrite" : "/fsync");
        int pfd[2];
        CHECK(pipe(pfd) == 0);
        std::cout.flush();
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            CHECK(close(pfd[0]) == 0);
            auto writes = std::make_shared<std::atomic<int>>(0);
            auto armed = std::make_shared<std::atomic<bool>>(false);
            IndexedStore::Options o = storeOpts(16);
            o.inject_index_failure = [=](Pager::IoOp op, bpt::page_id_t) {
                if (variant == 0 && op == Pager::IoOp::PageWrite) return ++*writes == 40;
                if (variant == 1 && op == Pager::IoOp::Fsync) return armed->load();
                return false;
            };
            auto st = IndexedStore::open(d, o);
            std::vector<std::thread> th;
            for (uint32_t w = 0; w < 4; ++w) {
                th.emplace_back([&, w]() {
                    for (uint32_t i = 0; i < 150; ++i) {
                        uint32_t id = w * 1000 + i;
                        std::string doc(200, static_cast<char>('a' + id % 26));
                        uint64_t seq = st->put(case9Key(id), doc);
                        Ack a{seq, id, 0};
                        CHECK(write(pfd[1], &a, sizeof(a)) == static_cast<ssize_t>(sizeof(a)));
                    }
                });
            }
            for (auto& t : th) t.join();
            armed->store(true);
            st->close();  // variant 1 aborts here
            _exit(7);     // variant 0 should have aborted earlier
        }
        CHECK(close(pfd[1]) == 0);
        std::vector<Ack> acks;
        Ack a;
        while (true) {
            ssize_t n = read(pfd[0], &a, sizeof(a));
            if (n == static_cast<ssize_t>(sizeof(a))) { acks.push_back(a); continue; }
            if (n == 0) break;
            CHECK(n < 0 && errno == EINTR);
        }
        CHECK(close(pfd[0]) == 0);
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)) {
            std::cerr << "case 9 child: " << (WIFEXITED(status) ? "exit " + std::to_string(WEXITSTATUS(status))
                                                                  : "signal " + std::to_string(WTERMSIG(status))) << "\n";
        }
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        // Bytes the child wrote to the vLog (index page writes were counted live through shared memory).
        uint64_t vb = 0;
        for (uint32_t id : bpt::listVLogSegments(d + "/vlog")) vb += fileSize(segPath(d + "/vlog", id));
        accountBytes(vb, "case 9 child vLog");

        auto st = IndexedStore::open(d, storeOpts());
        CHECK(!st->openInfo().rebuilt && st->openInfo().initial_status == OpenStatus::Ok);
        for (const Ack& x : acks) {
            uint64_t seq = 0;
            std::optional<std::string> doc = st->get(case9Key(x.id), &seq);
            CHECK(doc && seq == x.seq && *doc == std::string(200, static_cast<char>('a' + x.id % 26)));
        }
        checkIndexEqualsVLog(*st, "after injected failure");
        std::cout << (variant == 0 ? "9a pwrite failure (40th page write)" : "9b fsync failure in close()")
                  << ": child aborted (SIGABRT) after " << acks.size() << " acked puts; reopen recovered from the superblock + vLog replay; all "
                  << acks.size() << " acked keys return their acked value.\n";
        finishStore(st, true);
    }
}

int main() {
    std::cout << "====================================================================\n"
              << "  PHASE 2 EDGE-CASE AUDIT" << (kQuick ? " (quick)" : "") << "\n"
              << "====================================================================\n";
    std::string dir = setupRun("edge_test");
    Cleaner cleaner{dir};

    runCase("1. Index ahead of the vLog", [&] { case1IndexAheadOfVLog(dir + "/c1"); });
    runCase("2. index.db paired with a different vLog", [&] { case2WrongVLog(dir + "/c2"); });
    runCase("3. get() with a bad VPtr (missing segment, corrupt entry)", [&] { case3GetErrors(dir + "/c3"); });
    runCase("4. Document size: 0 bytes and the 16 MiB entry limit", [&] { case4DocSize(dir + "/c4"); });
    runCase("5. Two threads put the same key", [&] { case5SameKeyRace(dir + "/c5"); });
    runCase("6. Concurrent get + put on one key", [&] { case6GetPutMix(dir + "/c6"); });
    runCase("7. scan edge cases", [&] { case7ScanEdges(dir + "/c7"); });
    runCase("8. Height 4, 200,000 sequential keys", [&] { case8Height4(dir + "/c8"); });
    runCase("9. index.db pwrite/fsync failure", [&] { case9IoFailure(dir + "/c9"); });

    std::cout << "\n+" << std::string(60, '-') << "+" << std::string(8, '-') << "+\n";
    int failed = 0;
    for (const auto& r : g_results) {
        std::printf("| %-58s | %-6s |\n", r.name.c_str(), r.passed ? "PASS" : "FAIL");
        if (!r.passed) failed++;
    }
    std::cout << "+" << std::string(60, '-') << "+" << std::string(8, '-') << "+\n";
    printBudget();
    if (failed) {
        std::cout << "\n" << failed << " of " << g_results.size() << " edge cases FAILED.\n";
        return 1;
    }
    std::cout << "\nALL " << g_results.size() << " EDGE CASES PASSED\n";
    return 0;
}
