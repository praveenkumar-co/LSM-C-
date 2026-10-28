// Tests for the performance work (P6.*): segment rollover under concurrency, and the caches,
// group-commit and checkpoint changes that follow it.
#include "test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexRecovery.hpp"
#include "index/IndexedStore.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using bpt::IndexedStore;
using doc::Value;

static uint64_t dirBytes(const std::string& d) {
    uint64_t n = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(d))
        if (e.is_regular_file()) n += e.file_size();
    return n;
}

// ------------------------------------------------------------------------------------------
// P6.S1 Segment rollover with 16 threads: one new segment per rollover
// ------------------------------------------------------------------------------------------
static void testS1Rollover(const std::string& dir) {
    banner("P6.S1 16 threads rolling over together open one segment, not one each");
    // Full mode: 20 MB with 256 KiB segments. Quick mode: 2 MB with 64 KiB segments.
    const uint32_t seg = kQuick ? 64 * 1024 : 256 * 1024;
    const uint64_t target = kQuick ? 2ULL * 1000 * 1000 : 20ULL * 1000 * 1000;
    const int kThreads = 16;
    const std::string payload(1000, 'x');
    const uint32_t entry = vlog::entry_size(8, static_cast<uint32_t>(payload.size()));
    const uint64_t per_thread = target / entry / kThreads;
    const uint64_t n = per_thread * kThreads;
    {
        vlog::VLogWriter w(dir, vlog::VLogWriter::Options(seg, 64ULL * 1024 * 1024));
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; ++t)
            th.emplace_back([&, t] {
                char key[9];
                for (uint64_t i = 0; i < per_thread; ++i) {
                    std::snprintf(key, sizeof(key), "t%02dk%04d", t, static_cast<int>(i % 10000));
                    w.append(vlog::EntryType::Full, key, payload.data(),
                             static_cast<uint32_t>(payload.size()));
                }
            });
        for (auto& x : th) x.join();
    }
    const uint64_t per_segment = (seg - vlog::kBlockSize) / entry;
    const uint64_t expected = (n + per_segment - 1) / per_segment;
    std::vector<uint32_t> segs = bpt::listVLogSegments(dir);
    vlog::VLogReader reader(dir);
    uint64_t entries = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        vlog::ScanResult sr = reader.scanSegment(segs[i], i + 1 == segs.size());
        CHECK(!sr.truncated);
        for (const auto& e : sr.entries)
            CHECK(std::string(e.payload.begin(), e.payload.end()) == payload);
        entries += sr.entries.size();
    }
    if (segs.size() > expected + 2)
        std::cerr << "P6.S1: " << segs.size() << " segments, expected at most " << expected + 2
                  << "\n";
    CHECK(entries == n);
    CHECK(segs.size() <= expected + 2);
    accountBytes(dirBytes(dir), "P6.S1 vLog");
    pass("P6.S1 16 threads, " + std::to_string(n) + " entries: " + std::to_string(segs.size()) +
         " segments (expected " + std::to_string(expected) + ", limit +2), every entry intact");
}

// ------------------------------------------------------------------------------------------
// P6.R1 Segment descriptor cache: bounded, closed before GC unlinks, deleted segment = missing
// ------------------------------------------------------------------------------------------
static IndexedStore::Options smallSegOpts(size_t fd_cache) {
    IndexedStore::Options o;
    o.cache_frames = 64;
    o.vlog_opts = vlog::VLogWriter::Options(8 * 1024, std::min<uint64_t>(64ULL << 20, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index writes"); };
    o.auto_checkpoint = false;
    o.gc_rate_mb_s = 0;
    o.fd_cache_size = fd_cache;
    return o;
}
static std::string bigDoc(int i) { return std::string("{\"i\":") + std::to_string(i) + ",\"p\":\"" + std::string(2900, 'a' + i % 26) + "\"}"; }

static void testR1FdCache(const std::string& dir) {
    banner("P6.R1 segment descriptor cache");
    auto st = IndexedStore::open(dir, smallSegOpts(4));
    const int kKeys = 12; // one ~3 KB document per 8 KiB segment: 12 segments, cache of 4
    for (int i = 0; i < kKeys; ++i) st->put("k" + std::to_string(i), bigDoc(i));
    // 8 readers over 12 segments with 4 descriptors: evictions while leases are held.
    std::vector<std::thread> th;
    std::atomic<int> bad{0};
    for (int t = 0; t < 8; ++t)
        th.emplace_back([&, t] {
            for (int r = 0; r < (kQuick ? 30 : 200); ++r) {
                const int i = (t * 7 + r) % kKeys;
                if (st->get("k" + std::to_string(i)) != bigDoc(i)) bad++;
            }
        });
    for (auto& x : th) x.join();
    CHECK(bad == 0);
    CHECK(st->fdCacheForTest().size() <= 4);
    CHECK(st->fdCacheForTest().hits() > 0 && st->fdCacheForTest().misses() >= kKeys);
    // A segment deleted from outside while its descriptor is cached reads as missing.
    const vlog::VPtr p0 = st->tree().get("k11")->vptr;
    CHECK(st->get("k11") == bigDoc(11) && st->fdCacheForTest().contains(p0.segment_id));
    const std::string seg_path = st->vlogDir() + "/vlog_" + std::string(8 - std::to_string(p0.segment_id).size(), '0') + std::to_string(p0.segment_id) + ".log";
    accountBytes(std::filesystem::file_size(seg_path), "segment removed by the test");
    CHECK(::unlink(seg_path.c_str()) == 0);
    bool threw = false;
    try {
        (void)st->get("k11");
    } catch (const bpt::VLogReadError&) {
        threw = true;
    }
    CHECK(threw && !st->fdCacheForTest().contains(p0.segment_id));
    st->put("k11", bigDoc(11)); // live again in a new segment
    // GC unlinks segments whose descriptors are cached: they are closed first.
    std::vector<uint32_t> old;
    for (int i = 0; i < 6; ++i) {
        old.push_back(st->tree().get("k" + std::to_string(i))->vptr.segment_id);
        CHECK(st->get("k" + std::to_string(i)) == bigDoc(i)); // cache the descriptor
    }
    for (int i = 0; i < 6; ++i) st->put("k" + std::to_string(i), bigDoc(i + 20));
    st->put("z", bigDoc(40));
    for (int r = 0; r < 200; ++r) {
        bpt::GcReport g = st->runGcPass();
        if (g.action == bpt::GcReport::Action::None && !st->gcPending()) break;
        st->checkpoint();
    }
    for (uint32_t id : old) {
        CHECK(!std::filesystem::exists(st->vlogDir() + "/vlog_" + std::string(8 - std::to_string(id).size(), '0') + std::to_string(id) + ".log"));
        CHECK(!st->fdCacheForTest().contains(id));
    }
    for (int i = 0; i < kKeys; ++i)
        CHECK(st->get("k" + std::to_string(i)) == bigDoc(i < 6 ? i + 20 : i));
    accountBytes(st->vlogBytesWritten(), "vLog");
    st->close();
    pass("P6.R1 descriptor cache: 8 readers over 12 segments with 4 slots read correctly; a "
         "deleted cached segment reads as missing; GC closes descriptors before unlinking");
}

// ------------------------------------------------------------------------------------------
// P6.W1 Adaptive group-commit window: a single writer never waits; a full batch stops the wait
// ------------------------------------------------------------------------------------------
static void testW1CommitWindow(const std::string& base) {
    banner("P6.W1 adaptive group-commit window");
    CHECK(::mkdir(base.c_str(), 0755) == 0);
    const std::string payload(500, 'w');
    // A 200 ms window: if a single writer waited, 20 appends would take 4 s.
    {
        vlog::VLogWriter::Options o(1 << 20, 64ULL << 20);
        o.group_commit_window_us = 200000;
        vlog::VLogWriter w(base + "/single", o);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) w.append(vlog::EntryType::Full, "k", payload.data(), payload.size());
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(s < 2.0);
        CHECK(w.getAverageBatchSize() <= 1.0); // segment-creation fsyncs count too
    }
    // 16 writers, max batch 16, a 200 ms window: batches fill and stop waiting at 16.
    uint64_t n = 0;
    double avg = 0, secs = 0;
    {
        vlog::VLogWriter::Options o(1 << 20, 64ULL << 20);
        o.group_commit_window_us = 200000;
        o.group_commit_max_batch = 16;
        vlog::VLogWriter w(base + "/many", o);
        const int per = kQuick ? 20 : 100;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> th;
        for (int t = 0; t < 16; ++t)
            th.emplace_back([&] {
                for (int i = 0; i < per; ++i)
                    w.append(vlog::EntryType::Full, "key", payload.data(), payload.size());
            });
        for (auto& x : th) x.join();
        secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        avg = w.getAverageBatchSize();
        n = 16ULL * per;
        CHECK(avg >= 8.0);
        // Waiting out the window on every batch would take n / 16 * 0.2 s.
        CHECK(secs < double(n) / 16 * 0.2 / 2);
    }
    std::vector<uint32_t> segs = bpt::listVLogSegments(base + "/many");
    vlog::VLogReader reader(base + "/many");
    uint64_t entries = 0;
    for (size_t i = 0; i < segs.size(); ++i) entries += reader.scanSegment(segs[i], i + 1 == segs.size()).entries.size();
    CHECK(entries == n);
    accountBytes(dirBytes(base), "P6.W1 vLog");
    pass("P6.W1 window 200 ms: 1 writer never waits (20 appends, batch <= 1.0); 16 writers fill "
         "batches of 16 and stop waiting (avg batch " + std::to_string(avg).substr(0, 4) + ", " +
         std::to_string(secs).substr(0, 4) + " s); all " + std::to_string(n) + " entries intact");
}

// ------------------------------------------------------------------------------------------
// P6.C Checkpoints written outside the locks
// ------------------------------------------------------------------------------------------
static IndexedStore::Options cpOpts() {
    IndexedStore::Options o;
    o.cache_frames = 64; // small cache: evictions while the checkpoint is in flight
    o.vlog_opts = vlog::VLogWriter::Options(1 << 20, std::min<uint64_t>(256ULL << 20, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index writes"); };
    o.auto_checkpoint = false;
    o.gc_rate_mb_s = 0;
    return o;
}
static std::string smallDoc(int k, int v) {
    return "{\"k\":" + std::to_string(k) + ",\"v\":" + std::to_string(v) + ",\"n\":0,\"p\":\"" + std::string(60, 'a' + k % 26) + "\"}";
}
static std::string ckey(int k) {
    char b[16];
    std::snprintf(b, sizeof(b), "k%05d", k);
    return b;
}
using TreeState = std::map<std::string, uint64_t>; // key -> seq
static TreeState treeState(bpt::BPTree& t) {
    TreeState m;
    for (const auto& [k, ref] : t.scanFrom("", SIZE_MAX)) m[k] = ref.seq;
    return m;
}
static void copyIndexFiles(const std::string& from, const std::string& to) {
    std::filesystem::create_directories(to);
    for (const char* f : {"/index.db", "/superblock.db"}) {
        std::filesystem::copy_file(from + f, to + f);
        accountBytes(std::filesystem::file_size(to + f), "index file copy");
    }
}
// The committed tree in dir (newest valid slot), read from disk only.
static TreeState committedState(const std::string& dir) {
    bpt::Pager::Options po;
    po.num_frames = 64;
    bpt::OpenIndexResult r = bpt::openCommittedIndex(dir, po);
    CHECK(r.initial_status == bpt::OpenStatus::Ok && r.pager);
    bpt::BPTree t(*r.pager);
    return treeState(t);
}

static void testC1SnapshotDuringUpdates(const std::string& dir) {
    banner("P6.C1 updates to the same leaf during a slow checkpoint");
    TreeState captured;
    std::atomic<bool> during{false};
    int updates_in_flight = 0;
    IndexedStore* sp = nullptr;
    IndexedStore::Options o = cpOpts();
    o.on_checkpoint_after_capture = [&](bpt::CheckpointTrigger) {
        if (!during) return;
        // Locks released, pages not written yet: hammer the same leaf (keys 0-9).
        for (int r = 0; r < 20; ++r)
            for (int k = 0; k < 10; ++k) {
                sp->put(ckey(k), smallDoc(k, 100 + r));
                updates_in_flight++;
            }
    };
    auto st = IndexedStore::open(dir, o);
    sp = st.get();
    for (int k = 0; k < 400; ++k) st->put(ckey(k), smallDoc(k, 0));
    st->checkpoint(); // epoch E
    for (int k = 0; k < 400; k += 3) st->put(ckey(k), smallDoc(k, 1));
    captured = treeState(st->tree()); // nothing writes until the capture: what it will see
    during = true;
    st->checkpoint(); // epoch E + 1, written while 200 puts change keys 0-9
    during = false;
    CHECK(updates_in_flight == 200 && !captured.empty());
    const TreeState live = treeState(st->tree());
    CHECK(live != captured); // the live tree moved on
    copyIndexFiles(dir, dir + "_e1");
    CHECK(committedState(dir + "_e1") == captured); // disk == the captured snapshot
    // More changes and epoch E + 2: the older slot (E + 1) must still read, so no page freed
    // while E + 1 was in flight was reused.
    for (int k = 0; k < 400; k += 2) st->put(ckey(k), smallDoc(k, 2));
    st->checkpoint();
    copyIndexFiles(dir, dir + "_e2");
    {
        bpt::SuperblockManager m(dir + "_e2/superblock.db");
        bpt::SuperblockManager::LoadResult lr = m.load();
        CHECK(lr.has_older);
        const std::string path = dir + "_e2/superblock.db";
        int fd = ::open(path.c_str(), O_RDWR);
        CHECK(fd >= 0);
        uint8_t b;
        CHECK(::pread(fd, &b, 1, lr.current_slot * bpt::kSuperSlotSize + 20) == 1);
        b ^= 0x40;
        CHECK(::pwrite(fd, &b, 1, lr.current_slot * bpt::kSuperSlotSize + 20) == 1);
        CHECK(::close(fd) == 0);
    }
    CHECK(committedState(dir + "_e2") == captured);
    for (int k = 0; k < 400; ++k) CHECK(st->get(ckey(k)).has_value());
    accountBytes(st->vlogBytesWritten(), "vLog");
    st->close();
    pass("P6.C1 200 puts to one leaf during a checkpoint: the on-disk tree equals the captured "
         "snapshot; after the next checkpoint the older slot still reads (no early page reuse)");
}

// 16 threads, each owning keys k % 16 == t, run put / update / erase / get against a model.
struct Model {
    bool present = false;
    int v = 0;
    int n = 0;
    std::string doc(int k) const {
        std::string d = smallDoc(k, v);
        if (n) d.replace(d.find("\"n\":0"), 5, "\"n\":" + std::to_string(n));
        return d;
    }
};

static void testC2CheckpointsUnderLoad(const std::string& dir) {
    banner("P6.C2 checkpoints during a 16-thread load");
    auto st = IndexedStore::open(dir, cpOpts());
    const int kKeys = 512, kOps = kQuick ? 150 : 800;
    std::vector<Model> model(kKeys);
    std::atomic<bool> done{false};
    std::vector<std::thread> th;
    for (int t = 0; t < 16; ++t)
        th.emplace_back([&, t] {
            std::mt19937 rng(t);
            for (int i = 0; i < kOps; ++i) {
                const int k = t + 16 * static_cast<int>(rng() % (kKeys / 16));
                Model& m = model[k];
                const int op = static_cast<int>(rng() % 10);
                if (op < 4) {
                    st->put(ckey(k), smallDoc(k, i));
                    m = Model{true, i, 0};
                } else if (op < 7) {
                    try {
                        st->update(ckey(k), {doc::inc("n", 1)});
                        CHECK(m.present);
                        m.n++;
                    } catch (const doc::NotFoundError&) {
                        CHECK(!m.present);
                    }
                } else if (op < 8) {
                    st->erase(ckey(k));
                    m.present = false;
                } else {
                    std::optional<std::string> got = st->get(ckey(k));
                    CHECK(m.present ? got.has_value() : !got.has_value());
                }
            }
        });
    int cps = 0;
    std::thread cp([&] {
        while (!done) {
            st->checkpoint();
            cps++;
        }
    });
    for (auto& x : th) x.join();
    done = true;
    cp.join();
    auto verify = [&](IndexedStore& s, const char* what) {
        for (int k = 0; k < kKeys; ++k) {
            std::optional<std::string> got = s.get(ckey(k));
            if (model[k].present ? got != model[k].doc(k) : got.has_value())
                std::cerr << what << ": key " << k << " differs\n";
            CHECK(model[k].present ? got == model[k].doc(k) : !got.has_value());
        }
    };
    verify(*st, "live");
    CHECK(st->verifyCountersForTest().empty());
    accountBytes(st->vlogBytesWritten(), "vLog");
    st.reset(); // no close(): recovery from the last checkpoint + replay
    st = IndexedStore::open(dir, cpOpts());
    CHECK(!st->openInfo().rebuilt);
    verify(*st, "recovered");
    const TreeState rec = treeState(st->tree());
    st->close();
    CHECK(::unlink((dir + "/superblock.db").c_str()) == 0);
    st = IndexedStore::open(dir, cpOpts());
    CHECK(st->openInfo().rebuilt && treeState(st->tree()) == rec);
    verify(*st, "rebuilt");
    st->close();
    pass("P6.C2 " + std::to_string(16 * kOps) + " ops on 16 threads with " + std::to_string(cps) +
         " concurrent checkpoints: live, recovered and rebuilt state equal the model");
}

#ifndef TSAN_BUILD
// kill -9 at the new checkpoint points while 16 threads write; acks through a pipe.
struct Rec {
    uint32_t key;
    uint8_t kind; // 0 intent, 1 ack
    uint8_t op;   // 0 put, 1 update, 2 erase
    uint8_t notfound;
    uint8_t pad;
    uint32_t arg;
    uint32_t pad2;
};
static_assert(sizeof(Rec) == 16);
static bool readRec(int fd, Rec* r) {
    while (true) {
        ssize_t n = read(fd, r, sizeof(*r));
        if (n == static_cast<ssize_t>(sizeof(*r))) return true;
        if (n == 0) return false;
        CHECK(n < 0 && errno == EINTR);
    }
}
static Model applyRec(Model m, const Rec& r) {
    if (r.op == 0) return Model{true, static_cast<int>(r.arg), 0};
    if (r.op == 2) return Model{};
    if (m.present) m.n++;
    return m;
}

static void testC3KillDuringCheckpoint(const std::string& base) {
    banner("P6.C3 kill -9 at the new checkpoint points during a 16-thread load");
    CHECK(::mkdir(base.c_str(), 0755) == 0);
    const char* names[] = {"after capture, before any page write", "after 3 page writes",
                           "after the index fsync, before the superblock"};
    const int kKeys = 512, reps = kQuick ? 1 : 3;
    uint64_t total_acked = 0;
    for (int point = 0; point < 3; ++point) {
        for (int rep = 0; rep < reps; ++rep) {
            const std::string dir = base + "/p" + std::to_string(point) + "_" + std::to_string(rep);
            std::vector<Model> model(kKeys);
            int pfd[2];
            CHECK(pipe(pfd) == 0);
            std::cout.flush();
            pid_t pid = fork();
            CHECK(pid >= 0);
            if (pid == 0) {
                childExpectsAbort();
                CHECK(close(pfd[0]) == 0);
                std::atomic<uint64_t> acks{0};
                std::atomic<bool> writing_pages{false};
                std::atomic<int> pages{0};
                IndexedStore::Options o = cpOpts();
                o.on_index_write = [&](uint64_t n) {
                    accountBytes(n, "index writes");
                    if (writing_pages && ++pages >= 3) raise(SIGKILL);
                };
                o.on_checkpoint_after_capture = [&](bpt::CheckpointTrigger) {
                    if (acks < 300) return;
                    if (point == 0) raise(SIGKILL);
                    if (point == 1) writing_pages = true;
                };
                o.on_checkpoint_after_index_fsync = [&](bpt::CheckpointTrigger) {
                    if (acks >= 300 && point == 2) raise(SIGKILL);
                };
                auto st = IndexedStore::open(dir, o);
                std::vector<std::thread> th;
                for (int t = 0; t < 16; ++t)
                    th.emplace_back([&, t] {
                        std::mt19937 rng(100 * point + t);
                        for (uint32_t i = 1; i < 100000; ++i) {
                            const int k = t + 16 * static_cast<int>(rng() % (kKeys / 16));
                            Rec in{static_cast<uint32_t>(k), 0, 0, 0, 0, i, 0};
                            const int roll = static_cast<int>(rng() % 10);
                            in.op = roll < 5 ? 0 : (roll < 9 ? 1 : 2);
                            CHECK(write(pfd[1], &in, sizeof(in)) == static_cast<ssize_t>(sizeof(in)));
                            Rec ack = in;
                            ack.kind = 1;
                            if (in.op == 0) {
                                st->put(ckey(k), smallDoc(k, static_cast<int>(i)));
                            } else if (in.op == 1) {
                                try {
                                    st->update(ckey(k), {doc::inc("n", 1)});
                                } catch (const doc::NotFoundError&) {
                                    ack.notfound = 1;
                                }
                            } else {
                                st->erase(ckey(k));
                            }
                            CHECK(write(pfd[1], &ack, sizeof(ack)) == static_cast<ssize_t>(sizeof(ack)));
                            acks++;
                        }
                    });
                while (true) st->checkpoint(); // killed inside one of them
            }
            CHECK(close(pfd[1]) == 0);
            std::map<uint32_t, Rec> pending;
            uint64_t acks = 0;
            Rec r;
            while (readRec(pfd[0], &r)) {
                if (r.kind == 0) {
                    pending[r.key] = r;
                    continue;
                }
                auto it = pending.find(r.key);
                CHECK(it != pending.end());
                if (r.op == 1) CHECK(r.notfound == (model[r.key].present ? 0 : 1));
                model[r.key] = applyRec(model[r.key], it->second);
                pending.erase(it);
                acks++;
            }
            CHECK(close(pfd[0]) == 0);
            int status = 0;
            CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
                  WTERMSIG(status) == SIGKILL);
            CHECK(acks >= 300);
            total_acked += acks;
            accountBytes(dirBytes(dir + "/vlog"), "child vLog");
            auto st = IndexedStore::open(dir, cpOpts());
            CHECK(!st->openInfo().rebuilt);
            for (int k = 0; k < kKeys; ++k) {
                std::optional<std::string> got = st->get(ckey(k));
                auto matches = [&](const Model& m) { return m.present ? got == m.doc(k) : !got.has_value(); };
                auto pit = pending.find(static_cast<uint32_t>(k));
                if (pit == pending.end()) {
                    if (!matches(model[k])) std::cerr << names[point] << ": key " << k << " lost an acked op\n";
                    CHECK(matches(model[k]));
                } else {
                    CHECK(matches(model[k]) || matches(applyRec(model[k], pit->second)));
                }
            }
            const TreeState rec = treeState(st->tree());
            CHECK(st->verifyCountersForTest().empty());
            st->close();
            CHECK(::unlink((dir + "/superblock.db").c_str()) == 0);
            st = IndexedStore::open(dir, cpOpts());
            CHECK(st->openInfo().rebuilt && treeState(st->tree()) == rec);
            st->close();
        }
        pass(std::string("P6.C3 kill -9 ") + names[point] + ": every acked op present, index == "
             "full rebuild", std::to_string(reps) + " run(s)");
    }
    detail() << "P6.C3 acked ops checked: " << total_acked << "\n";
}
#endif

static void testC3(const std::string& base) {
#ifdef TSAN_BUILD
    skip("P6.C3 kill -9 at the new checkpoint points", "fork + kill -9 not run under ThreadSanitizer");
    (void)base;
#else
    testC3KillDuringCheckpoint(base);
#endif
}

int main() {
    std::string dir = setupRun("perf_test");
    Cleaner cleaner{dir};
    testS1Rollover(dir + "/s1");
    testR1FdCache(dir + "/r1");
    testW1CommitWindow(dir + "/w1");
    testC1SnapshotDuringUpdates(dir + "/c1");
    testC2CheckpointsUnderLoad(dir + "/c2");
    testC3(dir + "/c3");
    printBudget();
    return 0;
}
