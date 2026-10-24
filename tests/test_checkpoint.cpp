// Checkpoint and recovery tests: what a checkpoint records, its triggers, recovery from the
// replay point, fallback rebuilds, page reuse safety, a corrupt superblock slot, and kill -9 at
// random points.
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
using Oracle = std::map<std::string, uint64_t>; // key -> seq

// ------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------
static IndexedStore::Options storeOpts(size_t frames = 256) {
    IndexedStore::Options o;
    o.cache_frames = frames;
    o.vlog_opts = vlog::VLogWriter::Options(
        64 * 1024 * 1024, std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index.db / superblock.db write"); };
    o.auto_checkpoint = false; // each test turns on what it needs
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
        if (e.type == static_cast<uint8_t>(vlog::EntryType::Tombstone))
            o.erase(e.key);
        else
            o[e.key] = e.seq;
    }
    return o;
}

static void checkIndexEqualsVLog(IndexedStore& st, const char* what) {
    Oracle ref = vlogOracle(st.vlogDir());
    bpt::BPTree::ScanResult got = st.tree().scanFrom("", SIZE_MAX);
    if (got.size() != ref.size())
        std::cerr << what << ": index " << got.size() << " keys, vLog replay " << ref.size()
                  << "\n";
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

static std::string makeDoc(uint64_t i, size_t len) {
    std::string d = "{\"i\":" + std::to_string(i) + ",\"pad\":\"";
    d.append(len, static_cast<char>('a' + i % 26));
    return d + "\"}";
}

static void printReports(const std::vector<CheckpointReport>& reps) {
    detailf("  %-6s %-7s %-9s %-14s %-6s %-10s %-9s %-9s\n", "epoch", "trigger", "seq",
            "replay point", "pages", "bytes", "wait ms", "stall ms");
    for (const CheckpointReport& r : reps) {
        char rp[32];
        std::snprintf(rp, sizeof(rp), "%u@%u", r.replay_segment, r.replay_offset);
        detailf("  %-6llu %-7s %-9llu %-14s %-6llu %-10llu %-9.2f %-9.2f\n",
                (unsigned long long)r.epoch, bpt::toString(r.trigger),
                (unsigned long long)r.checkpoint_seq, rp, (unsigned long long)r.pages_written,
                (unsigned long long)r.bytes_written, r.lock_wait_ms, r.stall_ms);
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
    detail() << "defaults: auto checkpoint on, every " << (defaults.checkpoint_bytes >> 20)
             << " MiB of vLog or " << defaults.checkpoint_interval_ms / 1000 << " s\n";
    auto st = IndexedStore::open(dir, storeOpts());
    uint64_t last = 0;
    for (int i = 0; i < 200; ++i) last = st->put("k" + std::to_string(i % 50), makeDoc(i, 40));
    st->erase("k3");
    last++;
    CheckpointReport r = st->checkpoint();
    // The entry with seq == checkpoint_seq ends exactly at the replay point.
    bool found = false;
    for (const VLogEntryInfo& e : readVLog(st->vlogDir())) {
        if (e.seq == r.checkpoint_seq) {
            found = true;
            CHECK(e.vptr.segment_id == r.replay_segment &&
                  e.vptr.offset + e.vptr.length == r.replay_offset);
        }
    }
    CHECK(found && r.checkpoint_seq == last && st->lastAppliedSeq() == last);
    bpt::SuperblockManager m(dir + "/superblock.db");
    bpt::SuperblockManager::LoadResult sl = m.load();
    CHECK(sl.status == bpt::SuperblockManager::LoadResult::Status::Ok);
    CHECK(sl.current.sb.epoch == r.epoch && sl.current.sb.checkpoint_seq == r.checkpoint_seq &&
          sl.current.sb.replay_segment_id == r.replay_segment &&
          sl.current.sb.replay_offset == r.replay_offset &&
          sl.current.sb.index_page_count == r.page_count &&
          sl.current.sb.index_root_page == st->pager().rootPageId());
    CheckpointReport r2 =
        st->checkpoint(); // nothing new: a manual checkpoint still commits, with no pages
    CHECK(r2.epoch == r.epoch + 1 && r2.pages_written == 0 &&
          r2.checkpoint_seq == r.checkpoint_seq);
    printReports({r, r2});
    finishStore(st, true);
    pass("CP1 checkpoint records the last applied seq and the vLog position after it");
}

// ------------------------------------------------------------------------------------------
// CP2 / CP3: triggers
// ------------------------------------------------------------------------------------------
static void testCP2BytesTrigger(const std::string& dir) {
    banner("CP2 Bytes trigger: a checkpoint after every N bytes of vLog (N = 64 KiB here; default "
           "4 MiB)");
    IndexedStore::Options o = storeOpts();
    o.auto_checkpoint = true;
    o.checkpoint_bytes = 64 * 1024;
    o.checkpoint_interval_ms = 3600 * 1000;
    auto st = IndexedStore::open(dir, o);
    const int n = 400; // about 600 bytes per entry: about 240 KB of vLog
    for (int i = 0; i < n; ++i) st->put("key" + std::to_string(i % 120), makeDoc(i, 560));
    // The checkpointer runs in the background: wait for it to catch up (bounded).
    for (int w = 0; w < 200 && st->checkpointReports().size() < 3; ++w)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<CheckpointReport> reps = st->checkpointReports();
    uint64_t vbytes = st->vlogBytesWritten();
    detail() << "vLog bytes written: " << vbytes << "; checkpoints: " << reps.size() << "\n";
    printReports(reps);
    for (const auto& r : reps) CHECK(r.trigger == CheckpointTrigger::Bytes);
    CHECK(reps.size() >= 3);
    finishStore(st, true);
    pass("CP2 bytes trigger", std::to_string(reps.size()) + " checkpoints for " +
                                  std::to_string(vbytes) + " vLog bytes");
}

static void testCP3TimerTrigger(const std::string& dir) {
    banner("CP3 Timer trigger: a checkpoint every T ms while there are new entries (T = 150 ms "
           "here; default 30 s)");
    IndexedStore::Options o = storeOpts();
    o.auto_checkpoint = true;
    o.checkpoint_bytes = 1ULL << 40;
    o.checkpoint_interval_ms = 150;
    auto st = IndexedStore::open(dir, o);
    // A fixed number of puts, 20 ms apart (at least 700 ms in total). The number of timer
    // checkpoints still depends on elapsed time; each writes one leaf and a superblock slot.
    const int kPuts = 35;
    int i = 0;
    for (; i < kPuts; ++i) {
        st->put("t" + std::to_string(i % 20), makeDoc(i, 30));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250)); // let the last interval pass
    size_t busy = st->checkpointReports().size();
    std::this_thread::sleep_for(
        std::chrono::milliseconds(600)); // idle: no new entries, no new epochs
    std::vector<CheckpointReport> reps = st->checkpointReports();
    detail() << i << " puts, 20 ms apart; checkpoints while writing: " << busy
             << "; after 600 ms idle: " << reps.size() << "\n";
    printReports(reps);
    for (const auto& r : reps) CHECK(r.trigger == CheckpointTrigger::Timer);
    CHECK(busy >= 3 && reps.size() == busy);
    finishStore(st, true);
    pass("CP3 timer trigger: checkpoints while entries arrive, none while idle");
}

// ------------------------------------------------------------------------------------------
// CP4: checkpoints concurrent with writers and readers, then a crash
// ------------------------------------------------------------------------------------------
static void testCP4Concurrent(const std::string& dir) {
    banner("CP4 Checkpoints while 4 writers and 2 readers run; crash; recover from the last "
           "superblock");
    auto st = IndexedStore::open(dir, storeOpts(128));
    const uint32_t kWriters = 4, kOps = kQuick ? 150 : 500;
    std::atomic<bool> stop{false}, failed{false};
    std::atomic<uint64_t> puts_done{0};
    std::vector<std::map<uint32_t, std::pair<uint64_t, std::string>>> acked(
        kWriters); // key id -> (seq, doc)
    std::vector<std::thread> th;
    for (uint32_t w = 0; w < kWriters; ++w) {
        th.emplace_back([&, w] {
            std::mt19937 rng(40 + w);
            for (uint32_t i = 0; i < kOps; ++i) {
                uint32_t id = w * 1000 + rng() % 80; // each writer owns its keys
                std::string d = makeDoc(w * 100000 + i, 50 + rng() % 200);
                uint64_t s = st->put("c" + std::to_string(id), d);
                acked[w][id] = {s, d};
                puts_done++;
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
        if (kQuick) {
            // A checkpoint after every 40 acked puts: the count does not depend on disk speed.
            const uint64_t kStep = 40, kTotalPuts = uint64_t(kWriters) * kOps;
            for (uint64_t next = kStep; next < kTotalPuts; next += kStep) {
                while (puts_done.load() < next)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                reps.push_back(st->checkpoint());
            }
            return;
        }
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
    detail() << kWriters * kOps << " puts, " << reads << " concurrent gets, " << reps.size()
             << " checkpoints; max stall " << max_stall << " ms. Last 5 checkpoints:\n";
    printReports(
        std::vector<CheckpointReport>(reps.end() - std::min<size_t>(5, reps.size()), reps.end()));
    const uint64_t last_cp_seq = reps.back().checkpoint_seq;
    finishStore(st, false); // crash: no close

    auto st2 = IndexedStore::open(dir, storeOpts(128));
    const bpt::OpenIndexResult& info = st2->openInfo();
    detail() << "recovery: first check " << bpt::toString(info.initial_status)
             << ", checkpoint seq " << info.checkpoint_seq << ", entries applied by replay "
             << info.vlog_entries_applied << " (vLog has " << total << ")\n";
    CHECK(info.initial_status == OpenStatus::Ok && !info.rebuilt);
    CHECK(info.checkpoint_seq == last_cp_seq && info.vlog_entries_applied == total - last_cp_seq);
    checkIndexEqualsVLog(*st2, "CP4 after crash");
    for (uint32_t w = 0; w < kWriters; ++w) {
        for (const auto& [id, sd] : acked[w]) {
            uint64_t s = 0;
            CHECK(st2->get("c" + std::to_string(id), &s) == std::optional<std::string>(sd.second) &&
                  s == sd.first);
        }
    }
    finishStore(st2, true);
    pass("CP4 checkpoints during 4 writers + 2 readers; crash recovers from the last superblock",
         std::to_string(reps.size()) + " checkpoints, max stall " +
             std::to_string(static_cast<int>(max_stall + 0.5)) + " ms");
}

// ------------------------------------------------------------------------------------------
// Recovery from the replay point
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
    banner("CP5 Fast recovery: replay and writer tail scan start at the replay point, not at "
           "segment 1");
    const int kTotal = kQuick ? 800 : 6000, kTail = kQuick ? 150 : 200;
    std::map<std::string, std::pair<uint64_t, std::string>> acked;
    uint64_t last_cp_seq = 0;
    {
        // 16 frames and 120-byte keys: the tree does not fit in the cache, so evictions write pages
        // past the committed page count before the crash.
        auto st = IndexedStore::open(dir, smallSegOpts(16));
        for (int i = 0; i < kTotal; ++i) {
            std::string k = "f" + std::to_string(i % (kQuick ? 500 : 700)),
                        d = makeDoc(i, 100 + i % 50);
            k.resize(120, '_');
            acked[k] = {st->put(k, d), d};
            if (i == kTotal - kTail - 1) last_cp_seq = st->checkpoint().checkpoint_seq;
        }
        detail() << "wrote " << kTotal << " entries in "
                 << bpt::listVLogSegments(st->vlogDir()).size() << " segments; checkpoint at seq "
                 << last_cp_seq << "; then " << kTail << " more entries; crash. index.db "
                 << dirFileBytes(dir + "/index.db") / 4096 << " pages\n";
        finishStore(st, false);
    }
    bpt::SuperblockManager m(dir + "/superblock.db");
    bpt::SuperblockManager::LoadResult sl = m.load();
    CHECK(sl.status == bpt::SuperblockManager::LoadResult::Status::Ok &&
          sl.current.sb.checkpoint_seq == last_cp_seq);
    const uint64_t committed_pages = sl.current.sb.index_page_count;
    auto t0 = std::chrono::steady_clock::now();
    auto st = IndexedStore::open(dir, smallSegOpts(256));
    auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const bpt::OpenIndexResult& info = st->openInfo();
    const uint64_t total_entries = readVLog(st->vlogDir()).size();
    detail() << "recovery from superblock epoch " << sl.current.sb.epoch
             << " (replay point segment " << sl.current.sb.replay_segment_id << ", offset "
             << sl.current.sb.replay_offset << "):\n"
             << "  vLog entries in total:            " << total_entries << "\n"
             << "  writer tail scan read:            " << st->writerRecoveryEntriesScanned() << "\n"
             << "  index replay read:                " << info.vlog_entries_scanned << " (applied "
             << info.vlog_entries_applied << ")\n"
             << "  index.db pages truncated at open: " << st->pager().truncatedPagesAtOpen()
             << " (committed page count " << committed_pages << ")\n"
             << "  open time:                        " << ms << " ms\n";
    CHECK(info.initial_status == OpenStatus::Ok && !info.rebuilt);
    CHECK(sl.current.sb.replay_segment_id > 1);
    CHECK(info.vlog_entries_scanned == static_cast<uint64_t>(kTail));
    CHECK(st->writerRecoveryEntriesScanned() == static_cast<uint64_t>(kTail));
    CHECK(info.vlog_entries_scanned * 5 < total_entries);
    CHECK(st->pager().truncatedPagesAtOpen() > 0 &&
          dirFileBytes(dir + "/index.db") == committed_pages * 4096);
    checkIndexEqualsVLog(*st, "CP5");
    for (const auto& [k, sd] : acked) {
        uint64_t s = 0;
        CHECK(st->get(k, &s) == std::optional<std::string>(sd.second) && s == sd.first);
    }
    finishStore(st, false);
    // For comparison: the same vLog through a full rebuild.
    CHECK(unlink((dir + "/superblock.db").c_str()) == 0);
    st = IndexedStore::open(dir, smallSegOpts(256));
    detail() << "  for comparison, a full rebuild (superblock.db deleted) read "
             << st->openInfo().vlog_entries_scanned << " entries (writer tail scan "
             << st->writerRecoveryEntriesScanned() << ")\n";
    CHECK(st->openInfo().rebuilt && st->openInfo().vlog_entries_scanned == total_entries);
    finishStore(st, true);
    pass("CP5 recovery reads only the vLog tail after the replay point",
         std::to_string(kTail) + " of " + std::to_string(total_entries) + " entries");
}

// Writes an index.db in the format v2 layout: a meta page with clean_shutdown = 1.
static void writeV2IndexDb(const std::string& path) {
    std::vector<uint8_t> page(bpt::kPageSize, 0);
    bpt::initPage(page.data(), 0, bpt::PageType::Meta, 0);
    struct V2Meta { // MetaBody of index.db format v2 (72 bytes)
        uint64_t magic;
        uint32_t format_version, page_size, root_page_id, page_count, tree_height, clean_shutdown;
        uint64_t key_count, max_applied_seq, open_count, vlog_created_ns;
        uint32_t vlog_first_segment, reserved1;
    } __attribute__((packed)) m{};
    static_assert(sizeof(V2Meta) == 72);
    m.magic = 0x3141544D58444E49ULL; // "INDXMTA1"
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
    accountBytes(2 * bpt::kPageSize, "test write (format v2 index.db)");
    CHECK(vlog::full_fsync(fd) == 0);
    CHECK(close(fd) == 0);
}

static void testCP6OldLayout(const std::string& dir) {
    banner("CP6 index.db without superblock.db (format v2 layout) -> full rebuild from the vLog");
    std::map<std::string, std::string> want;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 300; ++i) {
            std::string k = "o" + std::to_string(i % 90);
            want[k] = makeDoc(i, 30);
            st->put(k, want[k]);
        }
        finishStore(st, true);
    }
    auto verify = [&](OpenStatus expect, const char* what) {
        auto st = IndexedStore::open(dir, storeOpts());
        const bpt::OpenIndexResult& info = st->openInfo();
        detail() << "  " << what << ": first check " << bpt::toString(info.initial_status)
                 << ", rebuilt " << (info.rebuilt ? "yes" : "no") << ", entries read "
                 << info.vlog_entries_scanned << "\n";
        CHECK(info.initial_status == expect && info.rebuilt);
        CHECK(st->tree().size() == want.size());
        for (const auto& [k, v] : want) CHECK(st->get(k) == std::optional<std::string>(v));
        checkIndexEqualsVLog(*st, what);
        finishStore(st, true);
        CHECK(dirFileBytes(dir + "/superblock.db") == 8192); // the rebuild committed a superblock
    };
    // (a1) current index.db, superblock.db removed.
    CHECK(unlink((dir + "/superblock.db").c_str()) == 0);
    verify(OpenStatus::NoSuperblock, "current-format index.db, no superblock.db");
    // (a2) a real format v2 index.db (v2 meta page, clean_shutdown = 1) and no superblock.db.
    CHECK(unlink((dir + "/superblock.db").c_str()) == 0);
    writeV2IndexDb(dir + "/index.db");
    verify(OpenStatus::BadHeader, "format v2 index.db, no superblock.db");
    pass("CP6 index.db without superblock.db (incl. format v2) -> full rebuild");
}

static void testCP7TreePageCrc(const std::string& dir) {
    banner("CP7 A page of the committed tree fails its CRC -> full rebuild from the vLog");
    std::map<std::string, std::string> want;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 1500; ++i) {
            std::string k = "p" + std::to_string(i % 600);
            k.resize(60, '-');
            want[k] = makeDoc(i, 20);
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
    for (bpt::page_id_t id = 1; id < w.reachable.size(); ++id)
        if (w.reachable[id] && id != sl.current.sb.index_root_page) victim = id;
    CHECK(victim != 0);
    xorByte(dir + "/index.db", uint64_t(victim) * 4096 + 2500, 0x20);
    auto st = IndexedStore::open(dir, storeOpts());
    const bpt::OpenIndexResult& info = st->openInfo();
    detail() << "flipped one bit in page " << victim << " (tree height " << w.height << ", "
             << w.pages_read << " tree pages): first check " << bpt::toString(info.initial_status)
             << " (" << info.why << "), rebuilt " << (info.rebuilt ? "yes" : "no") << "\n";
    CHECK(info.initial_status == OpenStatus::TreeCorrupt && info.rebuilt);
    for (const auto& [k, v] : want) CHECK(st->get(k) == std::optional<std::string>(v));
    checkIndexEqualsVLog(*st, "CP7");
    finishStore(st, true);
    pass("CP7 committed tree page with a bad CRC -> full rebuild");
}

// ------------------------------------------------------------------------------------------
// Crash tests
// ------------------------------------------------------------------------------------------
#ifndef TSAN_BUILD
static uint64_t dirBytes(const std::string& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         !ec && it != std::filesystem::end(it); it.increment(ec)) {
        if (it->is_regular_file()) total += it->file_size();
    }
    CHECK(!ec);
    return total;
}

// CP8: entries appended to the vLog while a checkpoint holds the locks are not in that checkpoint;
// after a crash right after the checkpoint, recovery replays them.
static void testCP8UnappliedReplayed(const std::string& dir) {
    banner("CP8 Entries in the vLog but not applied at checkpoint time are replayed after a crash");
    const int kBase = 100, kLate = 8;
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    std::cout.flush();
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        childExpectsAbort();
        CHECK(close(pfd[0]) == 0);
        IndexedStore::Options o = storeOpts();
        std::atomic<bool> armed{false};
        IndexedStore* sp = nullptr;
        uint64_t seq0 = 0;
        std::vector<std::thread> late;
        // The hook runs while the checkpoint holds the sequencer and tree locks: these puts append
        // to the vLog (write + fsync by the group commit) and then wait to be applied.
        o.on_checkpoint_after_index_fsync = [&](CheckpointTrigger) {
            if (!armed.exchange(false)) return;
            for (int t = 0; t < kLate; ++t)
                late.emplace_back(
                    [sp, t] { sp->put("late" + std::to_string(t), makeDoc(1000 + t, 40)); });
            for (int w = 0; w < 5000 && sp->vlogLastSeq() < seq0 + kLate; ++w)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100)); // let the group commit write them
        };
        auto st = IndexedStore::open(dir, o);
        sp = st.get();
        for (int i = 0; i < kBase; ++i) st->put("base" + std::to_string(i), makeDoc(i, 40));
        seq0 = st->vlogLastSeq();
        armed = true;
        CheckpointReport r = st->checkpoint();
        uint64_t rec[2] = {r.checkpoint_seq, st->vlogBytesWritten()};
        CHECK(write(pfd[1], rec, sizeof(rec)) == static_cast<ssize_t>(sizeof(rec)));
        raise(SIGKILL); // crash right after the checkpoint
    }
    CHECK(close(pfd[1]) == 0);
    uint64_t rec[2] = {0, 0};
    CHECK(read(pfd[0], rec, sizeof(rec)) == static_cast<ssize_t>(sizeof(rec)));
    CHECK(close(pfd[0]) == 0);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    accountBytes(dirBytes(dir + "/vlog"), "CP8 child vLog (index writes were counted live)");
    auto st = IndexedStore::open(dir, storeOpts());
    const bpt::OpenIndexResult& info = st->openInfo();
    std::vector<VLogEntryInfo> all = readVLog(st->vlogDir());
    uint64_t newer = 0;
    for (const auto& e : all) newer += e.seq > rec[0] ? 1 : 0;
    detail() << "checkpoint_seq " << rec[0] << "; vLog entries after it: " << newer
             << "; recovery applied " << info.vlog_entries_applied << "\n";
    CHECK(info.initial_status == OpenStatus::Ok && info.checkpoint_seq == rec[0]);
    CHECK(newer == static_cast<uint64_t>(kLate) && info.vlog_entries_applied == newer);
    for (int t = 0; t < kLate; ++t)
        CHECK(st->get("late" + std::to_string(t)) == std::optional<std::string>(makeDoc(1000 + t, 40)));
    checkIndexEqualsVLog(*st, "CP8");
    finishStore(st, true);
    pass("CP8 entries appended during a checkpoint are replayed after a crash",
         std::to_string(kLate) + " entries");
}

#endif

// Pages named by either superblock slot, with the CRC of each page's bytes on disk.
static std::map<bpt::page_id_t, uint32_t> protectedPages(const std::string& dir,
                                                         uint64_t* cur_epoch) {
    std::map<bpt::page_id_t, uint32_t> out;
    bpt::SuperblockManager m(dir + "/superblock.db");
    bpt::SuperblockManager::LoadResult sl = m.load();
    CHECK(sl.status == bpt::SuperblockManager::LoadResult::Status::Ok);
    *cur_epoch = sl.current.sb.epoch;
    int fd = open((dir + "/index.db").c_str(), O_RDONLY);
    CHECK(fd >= 0);
    std::vector<const bpt::SuperblockRecord*> recs{&sl.current};
    if (sl.has_older) recs.push_back(&sl.older);
    std::vector<uint8_t> buf(bpt::kPageSize);
    for (const bpt::SuperblockRecord* r : recs) {
        bpt::TreeWalk w = bpt::walkTree(fd, static_cast<bpt::page_id_t>(r->sb.index_root_page),
                                        static_cast<uint32_t>(r->sb.index_page_count));
        if (!w.ok) std::cerr << "tree of epoch " << r->sb.epoch << ": " << w.why << "\n";
        CHECK(w.ok);
        for (bpt::page_id_t id = 1; id < w.reachable.size(); ++id) {
            if (!w.reachable[id]) continue;
            CHECK(pread(fd, buf.data(), bpt::kPageSize, static_cast<off_t>(id) * bpt::kPageSize) ==
                  bpt::kPageSize);
            out[id] = vlog::crc32c(buf.data(), buf.size());
        }
    }
    CHECK(close(fd) == 0);
    return out;
}

static void testCP9PageReuse(const std::string& dir) {
    banner("CP9 Checkpoints with page reuse: no page of the trees named by the 2 superblock slots "
           "is overwritten");
    std::map<bpt::page_id_t, uint32_t> prot;
    uint64_t prot_epoch = 0;
    size_t checks = 0, pages_checked = 0;
    IndexedStore::Options o =
        storeOpts(24); // small cache: evictions write pages between checkpoints
    o.on_checkpoint_after_index_fsync = [&](CheckpointTrigger) {
        // All pages of the new epoch are written and fsynced; the new superblock is not written
        // yet.
        int fd = open((dir + "/index.db").c_str(), O_RDONLY);
        CHECK(fd >= 0);
        std::vector<uint8_t> buf(bpt::kPageSize);
        for (const auto& [id, crc] : prot) {
            CHECK(pread(fd, buf.data(), bpt::kPageSize, static_cast<off_t>(id) * bpt::kPageSize) ==
                  bpt::kPageSize);
            if (vlog::crc32c(buf.data(), buf.size()) != crc) {
                std::cerr << "page " << id << " of a tree named by superblock epoch " << prot_epoch
                          << " (or the one before) was overwritten\n";
                CHECK(false);
            }
        }
        CHECK(close(fd) == 0);
        checks++;
        pages_checked += prot.size();
    };
    auto st = IndexedStore::open(dir, o);
    std::mt19937 rng(9);
    prot = protectedPages(dir, &prot_epoch);
    detailf("  %-6s %-16s %-7s %-6s %-7s\n", "epoch", "protected pages", "pages", "reuses",
            "pages written");
    const int kRounds = 6;
    for (int round = 0; round < kRounds; ++round) {
        for (int i = 0; i < (kQuick ? 150 : 400); ++i) {
            std::string k = "r" + std::to_string(rng() % 400);
            k.resize(80, '=');
            if (rng() % 100 < 70)
                st->put(k, makeDoc(i, 20));
            else
                st->erase(k);
        }
        CheckpointReport r = st->checkpoint();
        prot = protectedPages(dir, &prot_epoch);
        CHECK(prot_epoch == r.epoch);
        detailf("  %-6llu %-16zu %-7u %-6llu %-7llu\n", (unsigned long long)r.epoch, prot.size(),
                r.page_count, (unsigned long long)st->pager().stats().reuses,
                (unsigned long long)r.pages_written);
    }
    const uint64_t reuses = st->pager().stats().reuses;
    checkIndexEqualsVLog(*st, "CP9");
    finishStore(st, true);
    CHECK(checks >= static_cast<size_t>(kRounds) && reuses > 0);
    pass("CP9 page reuse never overwrites pages of the trees named by both slots",
         std::to_string(checks) + " checkpoints, " + std::to_string(reuses) + " pages reused");
}

static void testCP10CorruptNewest(const std::string& dir) {
    banner(
        "CP10 Newest superblock slot corrupt: recovery uses the older slot and is still correct");
    std::map<std::string, std::pair<uint64_t, std::string>> acked;
    std::mt19937 rng(10);
    std::vector<uint64_t> cp_seqs;
    {
        auto st = IndexedStore::open(dir, storeOpts(24));
        for (int round = 0; round < 5; ++round) {
            for (int i = 0; i < (kQuick ? 120 : 300); ++i) {
                std::string k = "n" + std::to_string(rng() % 300);
                k.resize(80, '+');
                if (rng() % 100 < 75) {
                    std::string d = makeDoc(round * 1000 + i, 30);
                    acked[k] = {st->put(k, d), d};
                } else {
                    acked.erase(k);
                    st->erase(k);
                }
            }
            cp_seqs.push_back(st->checkpoint().checkpoint_seq);
        }
        for (int i = 0; i < 100; ++i) {
            std::string k = "n" + std::to_string(rng() % 300);
            k.resize(80, '+');
            std::string d = makeDoc(9000 + i, 30);
            acked[k] = {st->put(k, d), d};
        }
        finishStore(st, false); // crash
    }
    const std::string sbp = dir + "/superblock.db";
    bpt::SuperblockManager m(sbp);
    bpt::SuperblockManager::LoadResult sl = m.load();
    CHECK(sl.status == bpt::SuperblockManager::LoadResult::Status::Ok && sl.has_older);
    detail() << "slots before: current slot " << sl.current_slot << " epoch " << sl.current.sb.epoch
             << " (seq " << sl.current.sb.checkpoint_seq << "), older epoch " << sl.older.sb.epoch
             << " (seq " << sl.older.sb.checkpoint_seq << ")\n";
    CHECK(sl.current.sb.checkpoint_seq == cp_seqs.back() &&
          sl.older.sb.checkpoint_seq == cp_seqs[cp_seqs.size() - 2]);
    xorByte(sbp, static_cast<uint64_t>(sl.current_slot) * 4096 + 24, 0x01); // inside checkpoint_seq
    auto st = IndexedStore::open(dir, storeOpts(64));
    const bpt::OpenIndexResult& info = st->openInfo();
    const uint64_t total = readVLog(st->vlogDir()).size();
    detail() << "after corrupting the newest slot: first check "
             << bpt::toString(info.initial_status) << ", opened at checkpoint seq "
             << info.checkpoint_seq << ", replay applied " << info.vlog_entries_applied << " of "
             << total << " entries\n";
    CHECK(info.initial_status == OpenStatus::Ok && !info.rebuilt);
    CHECK(info.checkpoint_seq == sl.older.sb.checkpoint_seq);
    CHECK(info.vlog_entries_scanned == total - sl.older.sb.checkpoint_seq);
    checkIndexEqualsVLog(*st, "CP10");
    for (const auto& [k, sd] : acked) {
        uint64_t s = 0;
        CHECK(st->get(k, &s) == std::optional<std::string>(sd.second) && s == sd.first);
    }
    finishStore(st, true);
    pass("CP10 newest superblock slot corrupt -> recovery from the older slot");
}

#ifndef TSAN_BUILD
// ------------------------------------------------------------------------------------------
// CP11: kill -9 at random points, 50 iterations
// ------------------------------------------------------------------------------------------
struct KAck {
    uint64_t seq;
    uint32_t op; // 1 put, 2 erase, 9 child paused at the crash point
    uint32_t key_id;
    uint32_t writer;
    uint32_t idx;
};
static_assert(sizeof(KAck) == 24);

static std::string kKey(uint32_t id) {
    std::string k = "acct:" + std::to_string(id);
    k.resize(60, '.');
    return k;
}
static std::string kDoc(uint32_t w, uint32_t i, uint32_t id) {
    return "{\"w\":" + std::to_string(w) + ",\"i\":" + std::to_string(i) +
           ",\"id\":" + std::to_string(id) + ",\"p\":\"" +
           std::string(30 + (i * 7 + w) % 90, static_cast<char>('a' + (i + id) % 26)) + "\"}";
}
static void sendK(int fd, const KAck& a) {
    CHECK(write(fd, &a, sizeof(a)) == static_cast<ssize_t>(sizeof(a)));
}
static bool readK(int fd, KAck* a) {
    while (true) {
        ssize_t n = read(fd, a, sizeof(*a));
        if (n == static_cast<ssize_t>(sizeof(*a))) return true;
        if (n == 0) return false;
        CHECK(n < 0 && errno == EINTR);
    }
}
[[noreturn]] static void pauseForever() {
    while (true) pause();
}

enum KillKind {
    kDuringWrites = 0,
    kDuringFlush = 1,
    kBetweenFsyncAndSuperblock = 2,
    kDuringRecovery = 3
};
static const char* kindName(int k) {
    switch (k) {
    case kDuringWrites:
        return "during writes";
    case kDuringFlush:
        return "during checkpoint flush";
    case kBetweenFsyncAndSuperblock:
        return "index fsync -> superblock";
    default:
        return "during recovery";
    }
}

static std::atomic<bool> g_in_cp{false};
static std::atomic<int> g_cp_page_writes{0};
static std::atomic<int> g_cp_count{0}; // automatic checkpoints started in this child

// Quick mode: checkpoints come only from the 16 KiB bytes trigger (no 200 ms timer), and kinds 0
// and 3 are killed at a random ack count instead of after a random delay, so the bytes written
// depend on op counts, not on time or disk speed. 600 writes reach the 3rd automatic checkpoint
// (about 250 writes) with room to spare, which kinds 1 and 2 wait for.
static constexpr uint32_t kChildOpsPerThread = kQuick ? 150 : 3000;

static void killChildWriters(const std::string& sdir, int wfd, int kind) {
    IndexedStore::Options o = storeOpts(48);
    o.auto_checkpoint = true;
    o.checkpoint_bytes = kind == kDuringRecovery ? (1ULL << 40) : 16 * 1024;
    o.checkpoint_interval_ms = (kind == kDuringRecovery || kQuick) ? 3600 * 1000 : 200;
    // Crash in the 3rd automatic checkpoint, so both superblock slots name real trees.
    o.on_checkpoint_start = [](CheckpointTrigger) {
        g_cp_page_writes = 0;
        g_in_cp = ++g_cp_count >= 3;
    };
    if (kind == kDuringFlush) {
        o.on_checkpoint_after_index_fsync = [](CheckpointTrigger) { g_in_cp = false; };
        o.on_index_write = [wfd](uint64_t n) {
            accountBytes(n, "index write (child)");
            if (g_in_cp.load() && ++g_cp_page_writes == 2) {
                sendK(wfd, KAck{0, 9, 0, 0, 0});
                pauseForever();
            }
        };
    } else if (kind == kBetweenFsyncAndSuperblock) {
        o.on_checkpoint_after_index_fsync = [wfd](CheckpointTrigger) {
            if (g_in_cp.load()) {
                sendK(wfd, KAck{0, 9, 0, 0, 0});
                pauseForever();
            }
        };
    }
    auto st = IndexedStore::open(sdir, o);
    std::vector<std::thread> th;
    for (uint32_t w = 0; w < 4; ++w) {
        th.emplace_back([&, w] {
            std::mt19937 rng(7000 + w + 10 * kind);
            for (uint32_t i = 0; i < kChildOpsPerThread; ++i) {
                uint32_t id = rng() % 300;
                KAck a{0, 0, id, w, i};
                if (rng() % 100 < 70) {
                    a.seq = st->put(kKey(id), kDoc(w, i, id));
                    a.op = 1;
                } else {
                    a.seq = st->erase(kKey(id)).seq;
                    a.op = 2;
                }
                sendK(wfd, a);
            }
        });
    }
    for (auto& t : th) t.join();
    if (kQuick) pauseForever(); // cap reached before the kill arrived
    _exit(0);                   // not reached in practice: the parent kills the child first
}

static void testCP11KillLoop(const std::string& base) {
    banner("CP11 kill -9 at random points (writes, checkpoint flush, between index fsync and "
           "superblock, recovery)");
    CHECK(mkdir(base.c_str(), 0755) == 0);
    // Quick mode writes about 1.05 MiB here, over the 1 MiB guideline: 8 runs cover each of the 4
    // crash points twice, and each run is a fresh store with several checkpoints.
    const int kIters = kQuick ? 8 : 50;
    std::mt19937 rng(1105);
    int kinds[4] = {0, 0, 0, 0};
    uint64_t total_acked = 0, total_keys = 0, sum_scanned = 0, sum_entries = 0;
    detailf("  %-4s %-26s %-6s %-8s %-8s %-10s %-9s\n", "iter", "crash point", "acked", "entries",
            "cp seq", "replayed", "keys");
    for (int iter = 0; iter < kIters; ++iter) {
        const int kind = iter % 4;
        kinds[kind]++;
        const std::string sdir = base + "/it" + std::to_string(iter);
        int pfd[2];
        CHECK(pipe(pfd) == 0);
        std::cout.flush();
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            childExpectsAbort();
            CHECK(close(pfd[0]) == 0);
            killChildWriters(sdir, pfd[1], kind);
        }
        CHECK(close(pfd[1]) == 0);
        std::vector<KAck> acks;
        KAck a;
        if (kind == kDuringFlush || kind == kBetweenFsyncAndSuperblock) {
            bool paused = false;
            while (readK(pfd[0], &a)) {
                if (a.op == 9) {
                    paused = true;
                    break;
                }
                acks.push_back(a);
            }
            CHECK(paused);
        } else {
            const size_t kill_at = kQuick ? 150 + rng() % 300 : 150;
            while (acks.size() < kill_at && readK(pfd[0], &a)) acks.push_back(a);
            CHECK(acks.size() == kill_at);
            if (!kQuick) std::this_thread::sleep_for(std::chrono::milliseconds(5 + rng() % 200));
        }
        CHECK(kill(pid, SIGKILL) == 0);
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGKILL);
        while (readK(pfd[0], &a)) {
            CHECK(a.op != 9);
            acks.push_back(a);
        }
        CHECK(close(pfd[0]) == 0);
        accountBytes(dirBytes(sdir + "/vlog"), "CP11 child vLog");

        if (kind == kDuringRecovery) {
            // A second child recovers (replays the vLog tail) and is killed after 20 applied
            // entries.
            int qfd[2];
            CHECK(pipe(qfd) == 0);
            std::cout.flush();
            pid_t p2 = fork();
            CHECK(p2 >= 0);
            if (p2 == 0) {
                childExpectsAbort();
                CHECK(close(qfd[0]) == 0);
                IndexedStore::Options o = storeOpts(48);
                o.hooks.on_rebuild_entry = [&](uint64_t n) {
                    if (n == 20) {
                        sendK(qfd[1], KAck{0, 9, 0, 0, 0});
                        pauseForever();
                    }
                };
                auto st = IndexedStore::open(sdir, o);
                sendK(qfd[1], KAck{0, 8, 0, 0, 0}); // fewer than 20 entries to replay
                pauseForever();
            }
            CHECK(close(qfd[1]) == 0);
            CHECK(readK(qfd[0], &a) && (a.op == 9 || a.op == 8));
            CHECK(kill(p2, SIGKILL) == 0);
            CHECK(waitpid(p2, &status, 0) == p2);
            CHECK(close(qfd[0]) == 0);
            accountBytes(4096, "CP11 recovery child segment header");
        }

        auto st = IndexedStore::open(sdir, storeOpts(64));
        const bpt::OpenIndexResult& info = st->openInfo();
        CHECK(!info.rebuilt && info.initial_status == OpenStatus::Ok);
        std::vector<VLogEntryInfo> all = readVLog(st->vlogDir());
        std::set<uint64_t> seqs;
        std::map<std::string, std::vector<const VLogEntryInfo*>> per_key;
        for (const auto& e : all) {
            seqs.insert(e.seq);
            per_key[e.key].push_back(&e);
        }
        checkIndexEqualsVLog(*st, "CP11");
        // Every acked op is in the vLog, and the newest acked op per key is reflected.
        std::map<uint32_t, KAck> latest;
        for (const KAck& x : acks) {
            CHECK(seqs.count(x.seq) == 1);
            auto it = latest.find(x.key_id);
            if (it == latest.end() || x.seq > it->second.seq) latest[x.key_id] = x;
        }
        for (const auto& [id, x] : latest) {
            uint64_t got = 0;
            std::optional<std::string> d = st->get(kKey(id), &got);
            if (x.op == 1) {
                if (d) {
                    CHECK(got >= x.seq);
                    if (got == x.seq) CHECK(*d == kDoc(x.writer, x.idx, id)); // the acked value
                } else {
                    bool later_tomb = false; // only a later tombstone (acked or not) may hide it
                    for (const VLogEntryInfo* e : per_key[kKey(id)])
                        if (e->seq > x.seq &&
                            e->type == static_cast<uint8_t>(vlog::EntryType::Tombstone))
                            later_tomb = true;
                    CHECK(later_tomb);
                }
            } else if (d) {
                CHECK(got > x.seq);
            }
        }
        total_acked += acks.size();
        total_keys += latest.size();
        if (kind != kDuringRecovery) { // those iterations run without automatic checkpoints
            sum_scanned += info.vlog_entries_scanned;
            sum_entries += all.size();
        }
        detailf("  %-4d %-26s %-6zu %-8zu %-8llu %-10llu %-9zu\n", iter + 1, kindName(kind),
                acks.size(), all.size(), (unsigned long long)info.checkpoint_seq,
                (unsigned long long)info.vlog_entries_scanned, latest.size());
        finishStore(st, true);
        std::error_code ec;
        std::filesystem::remove_all(sdir, ec);
        CHECK(!ec);
    }
    detail() << "crash points: " << kinds[0] << " during writes, " << kinds[1]
             << " during checkpoint flush, " << kinds[2]
             << " between index fsync and superblock write, " << kinds[3]
             << " during recovery replay\n";
    detail() << "with automatic checkpoints (every 16 KiB of vLog here), recovery read "
             << sum_scanned << " of " << sum_entries << " vLog entries ("
             << (sum_entries ? 100.0 * sum_scanned / sum_entries : 0.0) << "%)\n";
    pass("CP11 kill -9 x" + std::to_string(kIters) +
             " (writes, checkpoint flush, fsync->superblock, recovery)",
         std::to_string(total_acked) + " acked ops, newest acked op reflected for " +
             std::to_string(total_keys) + " keys; recovery read " + std::to_string(sum_scanned) +
             " of " + std::to_string(sum_entries) + " entries");
}
#endif

int main() {
    std::string dir = setupRun("checkpoint_test");
    Cleaner cleaner{dir};
    testCP1Fields(dir + "/cp1");
    testCP2BytesTrigger(dir + "/cp2");
    testCP3TimerTrigger(dir + "/cp3");
    testCP4Concurrent(dir + "/cp4");
    testCP5FastRecovery(dir + "/cp5");
    testCP6OldLayout(dir + "/cp6");
    testCP7TreePageCrc(dir + "/cp7");
#ifndef TSAN_BUILD
    testCP8UnappliedReplayed(dir + "/cp8");
#else
    skip("CP8 entries appended during a checkpoint are replayed after a crash",
         "fork + kill -9 not run under ThreadSanitizer");
#endif
    testCP9PageReuse(dir + "/cp9");
    testCP10CorruptNewest(dir + "/cp10");
#ifndef TSAN_BUILD
    testCP11KillLoop(dir + "/cp11");
#else
    skip("CP11 kill -9 at random points", "fork + kill -9 not run under ThreadSanitizer");
#endif
    printBudget();
    return 0;
}
