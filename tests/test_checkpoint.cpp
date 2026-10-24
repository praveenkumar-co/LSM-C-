// Checkpoint tests (Step 3: IndexedStore::checkpoint and its triggers).
#include "test_common.hpp"

#include "index/IndexRecovery.hpp"
#include "index/IndexedStore.hpp"
#include "index/SuperblockManager.hpp"
#include "vlog/VLogReader.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using bpt::CheckpointReport;
using bpt::CheckpointTrigger;
using bpt::IndexedStore;
using bpt::OpenStatus;
using Oracle = std::map<std::string, uint64_t>;  // key -> seq

// ------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------
static IndexedStore::Options storeOpts(size_t frames = 256) {
    IndexedStore::Options o;
    o.cache_frames = frames;
    o.vlog_opts = vlog::VLogWriter::Options(64 * 1024 * 1024, std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index.db / superblock.db write"); };
    o.auto_checkpoint = false;  // each test turns on what it needs
    return o;
}

static void finishStore(std::unique_ptr<IndexedStore>& st, bool clean) {
    uint64_t vb = st->vlogBytesWritten();
    if (clean) st->close();
    st.reset();
    accountBytes(vb, "IndexedStore vLog writes");
}

struct VLogEntryInfo {
    uint64_t seq;
    std::string key;
    uint8_t type;
    vlog::VPtr vptr;
};

static std::vector<VLogEntryInfo> readVLog(const std::string& vdir) {
    std::vector<VLogEntryInfo> out;
    std::vector<uint32_t> segs = bpt::listVLogSegments(vdir);
    vlog::VLogReader reader(vdir);
    for (size_t i = 0; i < segs.size(); ++i) {
        vlog::ScanResult sr = reader.scanSegment(segs[i], i + 1 == segs.size());
        for (const auto& e : sr.entries) {
            const uint64_t seq = e.header.seq;
            out.push_back(VLogEntryInfo{seq, e.key, e.header.type, e.vptr});
        }
    }
    return out;
}

// should_apply() replay of the whole vLog.
static Oracle vlogOracle(const std::string& vdir) {
    Oracle o;
    for (const VLogEntryInfo& e : readVLog(vdir)) {
        auto it = o.find(e.key);
        bool has = it != o.end();
        if (!vlog::should_apply(has, has ? it->second : 0, e.seq, 0)) continue;
        if (e.type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) o.erase(e.key);
        else o[e.key] = e.seq;
    }
    return o;
}

static void checkIndexEqualsVLog(IndexedStore& st, const char* what) {
    Oracle ref = vlogOracle(st.vlogDir());
    bpt::BPTree::ScanResult got = st.tree().scanFrom("", SIZE_MAX);
    if (got.size() != ref.size()) std::cerr << what << ": index " << got.size() << " keys, vLog replay " << ref.size() << "\n";
    CHECK(got.size() == ref.size());
    size_t i = 0;
    for (const auto& [k, s] : ref) {
        CHECK(got[i].first == k && got[i].second.seq == s);
        ++i;
    }
    std::string err = st.tree().checkInvariants();
    if (!err.empty()) std::cerr << what << ": " << err << "\n";
    CHECK(err.empty());
}

static std::string doc(uint64_t i, size_t len) {
    std::string d = "{\"i\":" + std::to_string(i) + ",\"pad\":\"";
    d.append(len, static_cast<char>('a' + i % 26));
    return d + "\"}";
}

static void printReports(const std::vector<CheckpointReport>& reps) {
    std::printf("  %-6s %-7s %-9s %-14s %-6s %-10s %-9s %-9s\n", "epoch", "trigger", "seq", "replay point", "pages",
                "bytes", "wait ms", "stall ms");
    for (const CheckpointReport& r : reps) {
        char rp[32];
        std::snprintf(rp, sizeof(rp), "%u@%u", r.replay_segment, r.replay_offset);
        std::printf("  %-6llu %-7s %-9llu %-14s %-6llu %-10llu %-9.2f %-9.2f\n", (unsigned long long)r.epoch,
                    bpt::toString(r.trigger), (unsigned long long)r.checkpoint_seq, rp,
                    (unsigned long long)r.pages_written, (unsigned long long)r.bytes_written, r.lock_wait_ms, r.stall_ms);
    }
}

// ------------------------------------------------------------------------------------------
// CP1: what a checkpoint records
// ------------------------------------------------------------------------------------------
static void testCP1Fields(const std::string& dir) {
    banner("CP1 Checkpoint records the last applied seq and the vLog position right after it");
    IndexedStore::Options defaults;
    CHECK(defaults.auto_checkpoint && defaults.checkpoint_bytes == 4ULL * 1024 * 1024 &&
          defaults.checkpoint_interval_ms == 30000);
    std::cout << "defaults: auto checkpoint on, every " << (defaults.checkpoint_bytes >> 20) << " MiB of vLog or "
              << defaults.checkpoint_interval_ms / 1000 << " s\n";
    auto st = IndexedStore::open(dir, storeOpts());
    uint64_t last = 0;
    for (int i = 0; i < 200; ++i) last = st->put("k" + std::to_string(i % 50), doc(i, 40));
    st->erase("k3");
    last++;
    CheckpointReport r = st->checkpoint();
    // The entry with seq == checkpoint_seq ends exactly at the replay point.
    bool found = false;
    for (const VLogEntryInfo& e : readVLog(st->vlogDir())) {
        if (e.seq == r.checkpoint_seq) {
            found = true;
            CHECK(e.vptr.segment_id == r.replay_segment && e.vptr.offset + e.vptr.length == r.replay_offset);
        }
    }
    CHECK(found && r.checkpoint_seq == last && st->lastAppliedSeq() == last);
    bpt::SuperblockManager m(dir + "/superblock.db");
    bpt::SuperblockManager::LoadResult sl = m.load();
    CHECK(sl.status == bpt::SuperblockManager::LoadResult::Status::Ok);
    CHECK(sl.current.sb.epoch == r.epoch && sl.current.sb.checkpoint_seq == r.checkpoint_seq &&
          sl.current.sb.replay_segment_id == r.replay_segment && sl.current.sb.replay_offset == r.replay_offset &&
          sl.current.sb.index_page_count == r.page_count && sl.current.sb.index_root_page == st->pager().rootPageId());
    CheckpointReport r2 = st->checkpoint();  // nothing new: a manual checkpoint still commits, with no pages
    CHECK(r2.epoch == r.epoch + 1 && r2.pages_written == 0 && r2.checkpoint_seq == r.checkpoint_seq);
    printReports({r, r2});
    finishStore(st, true);
    std::cout << "PASS: checkpoint_seq == last applied seq (" << r.checkpoint_seq << "); replay point == end of that "
                 "entry in the vLog; superblock on disk matches; an empty checkpoint writes 0 pages.\n";
}

// ------------------------------------------------------------------------------------------
// CP2 / CP3: triggers
// ------------------------------------------------------------------------------------------
static void testCP2BytesTrigger(const std::string& dir) {
    banner("CP2 Bytes trigger: a checkpoint after every N bytes of vLog (N = 64 KiB here; default 4 MiB)");
    IndexedStore::Options o = storeOpts();
    o.auto_checkpoint = true;
    o.checkpoint_bytes = 64 * 1024;
    o.checkpoint_interval_ms = 3600 * 1000;
    auto st = IndexedStore::open(dir, o);
    const int n = 400;  // about 600 bytes per entry: about 240 KB of vLog
    for (int i = 0; i < n; ++i) st->put("key" + std::to_string(i % 120), doc(i, 560));
    // The checkpointer runs in the background: wait for it to catch up (bounded).
    for (int w = 0; w < 200 && st->checkpointReports().size() < 3; ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<CheckpointReport> reps = st->checkpointReports();
    uint64_t vbytes = st->vlogBytesWritten();
    std::cout << "vLog bytes written: " << vbytes << "; checkpoints: " << reps.size() << "\n";
    printReports(reps);
    for (const auto& r : reps) CHECK(r.trigger == CheckpointTrigger::Bytes);
    CHECK(reps.size() >= 3);
    finishStore(st, true);
    std::cout << "PASS: " << reps.size() << " bytes-triggered checkpoints for " << vbytes << " vLog bytes.\n";
}

static void testCP3TimerTrigger(const std::string& dir) {
    banner("CP3 Timer trigger: a checkpoint every T ms while there are new entries (T = 150 ms here; default 30 s)");
    IndexedStore::Options o = storeOpts();
    o.auto_checkpoint = true;
    o.checkpoint_bytes = 1ULL << 40;
    o.checkpoint_interval_ms = 150;
    auto st = IndexedStore::open(dir, o);
    auto t0 = std::chrono::steady_clock::now();
    int i = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(700)) {
        st->put("t" + std::to_string(i % 20), doc(i, 30));
        ++i;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));  // let the last interval pass
    size_t busy = st->checkpointReports().size();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // idle: no new entries, no new epochs
    std::vector<CheckpointReport> reps = st->checkpointReports();
    std::cout << i << " puts over 700 ms; checkpoints while writing: " << busy << "; after 600 ms idle: " << reps.size()
              << "\n";
    printReports(reps);
    for (const auto& r : reps) CHECK(r.trigger == CheckpointTrigger::Timer);
    CHECK(busy >= 3 && reps.size() == busy);
    finishStore(st, true);
    std::cout << "PASS: timer checkpoints while entries arrive; none while idle.\n";
}

// ------------------------------------------------------------------------------------------
// CP4: checkpoints concurrent with writers and readers, then a crash
// ------------------------------------------------------------------------------------------
static void testCP4Concurrent(const std::string& dir) {
    banner("CP4 Checkpoints while 4 writers and 2 readers run; crash; recover from the last superblock");
    auto st = IndexedStore::open(dir, storeOpts(128));
    const uint32_t kWriters = 4, kOps = kQuick ? 150 : 500;
    std::atomic<bool> stop{false}, failed{false};
    std::vector<std::map<uint32_t, std::pair<uint64_t, std::string>>> acked(kWriters);  // key id -> (seq, doc)
    std::vector<std::thread> th;
    for (uint32_t w = 0; w < kWriters; ++w) {
        th.emplace_back([&, w] {
            std::mt19937 rng(40 + w);
            for (uint32_t i = 0; i < kOps; ++i) {
                uint32_t id = w * 1000 + rng() % 80;  // each writer owns its keys
                std::string d = doc(w * 100000 + i, 50 + rng() % 200);
                uint64_t s = st->put("c" + std::to_string(id), d);
                acked[w][id] = {s, d};
            }
        });
    }
    std::atomic<uint64_t> reads{0};
    for (int r = 0; r < 2; ++r) {
        th.emplace_back([&, r] {
            std::mt19937 rng(90 + r);
            while (!stop.load()) {
                uint32_t id = (rng() % kWriters) * 1000 + rng() % 80;
                try {
                    std::optional<std::string> d = st->get("c" + std::to_string(id));
                    if (d && d->find("\"i\":") != 1) failed = true;
                } catch (const std::exception&) {
                    failed = true;
                }
                reads++;
            }
        });
    }
    std::vector<CheckpointReport> reps;
    std::thread cp([&] {
        while (!stop.load()) {
            reps.push_back(st->checkpoint());
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    });
    for (uint32_t w = 0; w < kWriters; ++w) th[w].join();
    stop = true;
    for (size_t k = kWriters; k < th.size(); ++k) th[k].join();
    cp.join();
    CHECK(!failed.load());
    const uint64_t total = st->lastAppliedSeq();
    double max_stall = 0;
    for (const auto& r : reps) max_stall = std::max(max_stall, r.stall_ms);
    std::cout << kWriters * kOps << " puts, " << reads << " concurrent gets, " << reps.size()
              << " checkpoints; max stall " << max_stall << " ms. Last 5 checkpoints:\n";
    printReports(std::vector<CheckpointReport>(reps.end() - std::min<size_t>(5, reps.size()), reps.end()));
    const uint64_t last_cp_seq = reps.back().checkpoint_seq;
    finishStore(st, false);  // crash: no close

    auto st2 = IndexedStore::open(dir, storeOpts(128));
    const bpt::OpenIndexResult& info = st2->openInfo();
    std::cout << "recovery: first check " << bpt::toString(info.initial_status) << ", checkpoint seq "
              << info.checkpoint_seq << ", entries applied by replay " << info.vlog_entries_applied << " (vLog has "
              << total << ")\n";
    CHECK(info.initial_status == OpenStatus::Ok && !info.rebuilt);
    CHECK(info.checkpoint_seq == last_cp_seq && info.vlog_entries_applied == total - last_cp_seq);
    checkIndexEqualsVLog(*st2, "CP4 after crash");
    for (uint32_t w = 0; w < kWriters; ++w) {
        for (const auto& [id, sd] : acked[w]) {
            uint64_t s = 0;
            CHECK(st2->get("c" + std::to_string(id), &s) == std::optional<std::string>(sd.second) && s == sd.first);
        }
    }
    finishStore(st2, true);
    std::cout << "PASS: readers never saw an error during checkpoints; after the crash the index was opened at the "
                 "last superblock and only entries after its checkpoint seq were applied; every acked key returns "
                 "its acked value.\n";
}

int main() {
    std::cout << "====================================================================\n"
              << "  CHECKPOINT TESTS" << (kQuick ? " (quick)" : "") << "\n"
              << "====================================================================\n";
    std::string dir = setupRun("checkpoint_test");
    Cleaner cleaner{dir};
    testCP1Fields(dir + "/cp1");
    testCP2BytesTrigger(dir + "/cp2");
    testCP3TimerTrigger(dir + "/cp3");
    testCP4Concurrent(dir + "/cp4");
    printBudget();
    std::cout << "\nALL CHECKPOINT TESTS PASSED\n";
    return 0;
}
