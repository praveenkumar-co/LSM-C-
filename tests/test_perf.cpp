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

int main() {
    std::string dir = setupRun("perf_test");
    Cleaner cleaner{dir};
    testS1Rollover(dir + "/s1");
    testR1FdCache(dir + "/r1");
    printBudget();
    return 0;
}
