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

int main() {
    std::string dir = setupRun("perf_test");
    Cleaner cleaner{dir};
    testS1Rollover(dir + "/s1");
    printBudget();
    return 0;
}
