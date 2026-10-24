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
            std::this_thread::sleep_for(std::chrono::milliseconds(kQuick ? 30 : 15));
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

// ------------------------------------------------------------------------------------------
// Step 4: fast recovery
// ------------------------------------------------------------------------------------------
static uint64_t dirFileBytes(const std::string& path) {
    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0);
    return static_cast<uint64_t>(st.st_size);
}

static void xorByte(const std::string& path, uint64_t off, uint8_t mask) {
    int fd = open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    uint8_t b = 0;
    CHECK(pread(fd, &b, 1, static_cast<off_t>(off)) == 1);
    b ^= mask;
    CHECK(pwrite(fd, &b, 1, static_cast<off_t>(off)) == 1);
    accountBytes(1, "test pwrite (bit flip)");
    CHECK(vlog::full_fsync(fd) == 0);
    CHECK(close(fd) == 0);
}

// Store with small segments (64 KiB), so the vLog has many segments.
static IndexedStore::Options smallSegOpts(size_t frames) {
    IndexedStore::Options o = storeOpts(frames);
    o.vlog_opts.segment_size_limit = 64 * 1024;
    return o;
}

static void testCP5FastRecovery(const std::string& dir) {
    banner("CP5 Fast recovery: replay and writer tail scan start at the replay point, not at segment 1");
    const int kTotal = kQuick ? 1200 : 6000, kTail = kQuick ? 150 : 200;
    std::map<std::string, std::pair<uint64_t, std::string>> acked;
    uint64_t last_cp_seq = 0;
    {
        // 16 frames and 120-byte keys: the tree does not fit in the cache, so evictions write pages
        // past the committed page count before the crash.
        auto st = IndexedStore::open(dir, smallSegOpts(16));
        for (int i = 0; i < kTotal; ++i) {
            std::string k = "f" + std::to_string(i % (kQuick ? 500 : 700)), d = doc(i, 100 + i % 50);
            k.resize(120, '_');
            acked[k] = {st->put(k, d), d};
            if (i == kTotal - kTail - 1) last_cp_seq = st->checkpoint().checkpoint_seq;
        }
        std::cout << "wrote " << kTotal << " entries in " << bpt::listVLogSegments(st->vlogDir()).size()
                  << " segments; checkpoint at seq " << last_cp_seq << "; then " << kTail
                  << " more entries; crash. index.db " << dirFileBytes(dir + "/index.db") / 4096 << " pages\n";
        finishStore(st, false);
    }
    bpt::SuperblockManager m(dir + "/superblock.db");
    bpt::SuperblockManager::LoadResult sl = m.load();
    CHECK(sl.status == bpt::SuperblockManager::LoadResult::Status::Ok && sl.current.sb.checkpoint_seq == last_cp_seq);
    const uint64_t committed_pages = sl.current.sb.index_page_count;
    auto t0 = std::chrono::steady_clock::now();
    auto st = IndexedStore::open(dir, smallSegOpts(256));
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const bpt::OpenIndexResult& info = st->openInfo();
    const uint64_t total_entries = readVLog(st->vlogDir()).size();
    std::cout << "recovery from superblock epoch " << sl.current.sb.epoch << " (replay point segment "
              << sl.current.sb.replay_segment_id << ", offset " << sl.current.sb.replay_offset << "):\n"
              << "  vLog entries in total:            " << total_entries << "\n"
              << "  writer tail scan read:            " << st->writerRecoveryEntriesScanned() << "\n"
              << "  index replay read:                " << info.vlog_entries_scanned << " (applied "
              << info.vlog_entries_applied << ")\n"
              << "  index.db pages truncated at open: " << st->pager().truncatedPagesAtOpen() << " (committed page count "
              << committed_pages << ")\n"
              << "  open time:                        " << ms << " ms\n";
    CHECK(info.initial_status == OpenStatus::Ok && !info.rebuilt);
    CHECK(sl.current.sb.replay_segment_id > 1);
    CHECK(info.vlog_entries_scanned == static_cast<uint64_t>(kTail));
    CHECK(st->writerRecoveryEntriesScanned() == static_cast<uint64_t>(kTail));
    CHECK(info.vlog_entries_scanned * 5 < total_entries);
    CHECK(st->pager().truncatedPagesAtOpen() > 0 && dirFileBytes(dir + "/index.db") == committed_pages * 4096);
    checkIndexEqualsVLog(*st, "CP5");
    for (const auto& [k, sd] : acked) {
        uint64_t s = 0;
        CHECK(st->get(k, &s) == std::optional<std::string>(sd.second) && s == sd.first);
    }
    finishStore(st, false);
    // For comparison: the same vLog through a full rebuild.
    CHECK(unlink((dir + "/superblock.db").c_str()) == 0);
    st = IndexedStore::open(dir, smallSegOpts(256));
    std::cout << "  for comparison, a full rebuild (superblock.db deleted) read " << st->openInfo().vlog_entries_scanned
              << " entries (writer tail scan " << st->writerRecoveryEntriesScanned() << ")\n";
    CHECK(st->openInfo().rebuilt && st->openInfo().vlog_entries_scanned == total_entries);
    finishStore(st, true);
    std::cout << "PASS: recovery read " << kTail << " of " << total_entries << " vLog entries (writer and index), "
                 "truncated uncommitted index pages, and every acked key returns its acked value.\n";
}

// Writes an index.db in the Phase 2 (format v2) layout: a meta page with clean_shutdown = 1.
static void writePhase2IndexDb(const std::string& path) {
    std::vector<uint8_t> page(bpt::kPageSize, 0);
    bpt::initPage(page.data(), 0, bpt::PageType::Meta, 0);
    struct V2Meta {  // Phase 2 MetaBody, format v2 (72 bytes)
        uint64_t magic; uint32_t format_version, page_size, root_page_id, page_count, tree_height, clean_shutdown;
        uint64_t key_count, max_applied_seq, open_count, vlog_created_ns;
        uint32_t vlog_first_segment, reserved1;
    } __attribute__((packed)) m{};
    static_assert(sizeof(V2Meta) == 72);
    m.magic = 0x3141544D58444E49ULL;  // "INDXMTA1"
    m.format_version = 2;
    m.page_size = bpt::kPageSize;
    m.root_page_id = 1;
    m.page_count = 2;
    m.tree_height = 1;
    m.clean_shutdown = 1;
    m.max_applied_seq = 999999;
    std::memcpy(page.data() + bpt::kHeaderSize, &m, sizeof(m));
    uint32_t crc = bpt::computePageCrc(page.data());
    std::memcpy(page.data(), &crc, 4);
    std::vector<uint8_t> leaf(bpt::kPageSize, 0);
    bpt::initPage(leaf.data(), 1, bpt::PageType::Leaf, 0);
    crc = bpt::computePageCrc(leaf.data());
    std::memcpy(leaf.data(), &crc, 4);
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, page.data(), page.size()) == static_cast<ssize_t>(page.size()));
    CHECK(write(fd, leaf.data(), leaf.size()) == static_cast<ssize_t>(leaf.size()));
    accountBytes(2 * bpt::kPageSize, "test write (Phase 2 index.db)");
    CHECK(vlog::full_fsync(fd) == 0);
    CHECK(close(fd) == 0);
}

static void testCP6OldLayout(const std::string& dir) {
    banner("CP6 index.db without superblock.db (Phase 2 layout) -> full rebuild from the vLog");
    std::map<std::string, std::string> want;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 300; ++i) { std::string k = "o" + std::to_string(i % 90); want[k] = doc(i, 30); st->put(k, want[k]); }
        finishStore(st, true);
    }
    auto verify = [&](OpenStatus expect, const char* what) {
        auto st = IndexedStore::open(dir, storeOpts());
        const bpt::OpenIndexResult& info = st->openInfo();
        std::cout << "  " << what << ": first check " << bpt::toString(info.initial_status) << ", rebuilt "
                  << (info.rebuilt ? "yes" : "no") << ", entries read " << info.vlog_entries_scanned << "\n";
        CHECK(info.initial_status == expect && info.rebuilt);
        CHECK(st->tree().size() == want.size());
        for (const auto& [k, v] : want) CHECK(st->get(k) == std::optional<std::string>(v));
        checkIndexEqualsVLog(*st, what);
        finishStore(st, true);
        CHECK(dirFileBytes(dir + "/superblock.db") == 8192);  // the rebuild committed a superblock
    };
    // (a1) current index.db, superblock.db removed.
    CHECK(unlink((dir + "/superblock.db").c_str()) == 0);
    verify(OpenStatus::NoSuperblock, "current-format index.db, no superblock.db");
    // (a2) a real Phase 2 index.db (v2 meta page, clean_shutdown = 1) and no superblock.db.
    CHECK(unlink((dir + "/superblock.db").c_str()) == 0);
    writePhase2IndexDb(dir + "/index.db");
    verify(OpenStatus::BadHeader, "Phase 2 (v2) index.db, no superblock.db");
    std::cout << "PASS: both layouts without a superblock were rebuilt from the vLog with all " << want.size()
              << " keys correct, and a superblock was committed.\n";
}

static void testCP7TreePageCrc(const std::string& dir) {
    banner("CP7 A page of the committed tree fails its CRC -> full rebuild from the vLog");
    std::map<std::string, std::string> want;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 1500; ++i) {
            std::string k = "p" + std::to_string(i % 600);
            k.resize(60, '-');
            want[k] = doc(i, 20);
            st->put(k, want[k]);
        }
        finishStore(st, true);
    }
    bpt::SuperblockManager m(dir + "/superblock.db");
    bpt::SuperblockManager::LoadResult sl = m.load();
    CHECK(sl.status == bpt::SuperblockManager::LoadResult::Status::Ok);
    // Pick a leaf of the committed tree: walk it and take the last reachable page id.
    int fd = open((dir + "/index.db").c_str(), O_RDONLY);
    CHECK(fd >= 0);
    bpt::TreeWalk w = bpt::walkTree(fd, static_cast<bpt::page_id_t>(sl.current.sb.index_root_page),
                                    static_cast<uint32_t>(sl.current.sb.index_page_count));
    CHECK(close(fd) == 0);
    CHECK(w.ok && w.height >= 2);
    bpt::page_id_t victim = 0;
    for (bpt::page_id_t id = 1; id < w.reachable.size(); ++id) if (w.reachable[id] && id != sl.current.sb.index_root_page) victim = id;
    CHECK(victim != 0);
    xorByte(dir + "/index.db", uint64_t(victim) * 4096 + 2500, 0x20);
    auto st = IndexedStore::open(dir, storeOpts());
    const bpt::OpenIndexResult& info = st->openInfo();
    std::cout << "flipped one bit in page " << victim << " (tree height " << w.height << ", " << w.pages_read
              << " tree pages): first check " << bpt::toString(info.initial_status) << " (" << info.why << "), rebuilt "
              << (info.rebuilt ? "yes" : "no") << "\n";
    CHECK(info.initial_status == OpenStatus::TreeCorrupt && info.rebuilt);
    for (const auto& [k, v] : want) CHECK(st->get(k) == std::optional<std::string>(v));
    checkIndexEqualsVLog(*st, "CP7");
    finishStore(st, true);
    std::cout << "PASS: the open-time tree walk found the bad page; the rebuilt index has all " << want.size()
              << " keys correct.\n";
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
    testCP5FastRecovery(dir + "/cp5");
    testCP6OldLayout(dir + "/cp6");
    testCP7TreePageCrc(dir + "/cp7");
    printBudget();
    std::cout << "\nALL CHECKPOINT TESTS PASSED\n";
    return 0;
}
