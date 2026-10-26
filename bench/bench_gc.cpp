// vLog GC benchmark (make bench). 10,000 keys x 1 KB documents, 1 MiB segments, 10 rounds of
// 3,000 overwrites, 3,000 one-field updates, 500 erases and 500 re-puts (fixed seed).
//   Run 1, GC off: the rounds, then GC passes (plus the checkpoints they need) until nothing is
//          left to collect. vLog bytes and space amplification before and after, GC bytes
//          written, segments unlinked, verification-scan bytes, and the counter rebuild cost of
//          the next open.
//   Run 2, GC on: the same rounds with the background GC thread.
// Both runs time every foreground put / update / get (one thread; gets: 1,000 random keys after
// each round). Default checkpoint triggers (4 MiB / 30 s).
#include "../tests/test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexedStore.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using bpt::IndexedStore;
using doc::Value;

namespace {

constexpr int kKeys = 10000;
constexpr int kRounds = 10;
constexpr int kOverwrites = 3000, kUpdates = 3000, kErases = 500;
constexpr int kGetsPerRound = 1000;
constexpr uint32_t kSegment = 1024 * 1024;
constexpr uint64_t kBenchBudget = 384ULL * 1024 * 1024;

std::string shell(const char* cmd) {
    std::string out;
    if (FILE* f = popen(cmd, "r")) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) out += buf;
        pclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

void printHardware() {
    struct utsname u;
    uname(&u);
#if defined(__APPLE__)
    const std::string cpu = shell("sysctl -n machdep.cpu.brand_string");
    const std::string mem = shell("sysctl -n hw.memsize");
    const std::string os = "macOS " + shell("sw_vers -productVersion");
#else
    const std::string cpu = shell("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2");
    const std::string mem = shell("awk '/MemTotal/ {print $2 * 1024}' /proc/meminfo");
    const std::string os = std::string(u.sysname) + " " + u.release;
#endif
    std::printf("Hardware: %s, %.0f GB RAM, %s (%s), compiler %s\n", cpu.c_str(),
                mem.empty() ? 0.0 : std::stod(mem) / 1e9, os.c_str(), u.machine, __VERSION__);
}

// A 1 KB document: {"id":..,"n":0,"name":"...","pad":"..."}.
std::string makeDoc(int id, int version) {
    Value v = Value::object();
    v.set("id", Value::integer(id));
    v.set("n", Value::integer(0));
    v.set("name", Value::string("user" + std::to_string(id) + "v" + std::to_string(version)));
    v.set("pad", Value::string(std::string(940, static_cast<char>('a' + (id + version) % 26))));
    return doc::serialize(v); // about 1 KB
}

std::string key(int i) {
    char b[16];
    std::snprintf(b, sizeof(b), "user%05d", i);
    return b;
}

uint64_t vlogDiskBytes(const std::string& vdir) {
    uint64_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(vdir))
        if (e.is_regular_file()) n += e.file_size();
    return n;
}

struct Lat {
    std::vector<double> us;
    void add(std::chrono::steady_clock::time_point t0) {
        us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                         .count());
    }
    double pct(double p) {
        if (us.empty()) return 0;
        std::sort(us.begin(), us.end());
        return us[std::min(us.size() - 1, static_cast<size_t>(p * us.size()))];
    }
};

struct RunResult {
    Lat put, upd, get;
    uint64_t vlog_before = 0, vlog_after = 0;
    double amp_before = 0, amp_after = 0;
    uint64_t gc_bytes = 0, unlinked = 0, passes = 0, verify_bytes = 0, mismatches = 0;
    uint64_t store_vlog_bytes = 0;
    double collect_s = 0, rounds_s = 0;
    bpt::GcOpenStats reopen;
    uint64_t live_keys = 0;
};

uint64_t g_index_bytes = 0;

IndexedStore::Options benchOpts(bool gc) {
    IndexedStore::Options o;
    o.vlog_opts = vlog::VLogWriter::Options(kSegment, kBenchBudget);
    o.on_index_write = [](uint64_t n) {
        g_index_bytes += n;
        accountBytes(n, "bench index writes");
    };
    o.gc_enabled = gc;
    o.gc_interval_ms = 10;
    return o;
}

RunResult run(const std::string& dir, bool gc_on) {
    RunResult r;
    std::vector<int> version(kKeys, 0);
    std::vector<bool> present(kKeys, true);
    auto st = IndexedStore::open(dir, benchOpts(gc_on));
    // Load with 8 threads (group commit); not timed.
    {
        std::vector<std::thread> th;
        for (int t = 0; t < 8; ++t)
            th.emplace_back([&, t] {
                for (int i = t; i < kKeys; i += 8) st->put(key(i), makeDoc(i, 0));
            });
        for (auto& x : th) x.join();
    }
    std::mt19937 rng(42);
    const auto r0 = std::chrono::steady_clock::now();
    for (int round = 0; round < kRounds; ++round) {
        for (int i = 0; i < kOverwrites; ++i) {
            const int k = static_cast<int>(rng() % kKeys);
            version[k]++;
            const std::string d = makeDoc(k, version[k]);
            auto t0 = std::chrono::steady_clock::now();
            st->put(key(k), d);
            r.put.add(t0);
            present[k] = true;
        }
        for (int i = 0; i < kUpdates; ++i) {
            const int k = static_cast<int>(rng() % kKeys);
            if (!present[k]) continue;
            auto t0 = std::chrono::steady_clock::now();
            st->update(key(k), {doc::inc("n", 1)});
            r.upd.add(t0);
        }
        std::vector<int> erased;
        for (int i = 0; i < kErases; ++i) {
            const int k = static_cast<int>(rng() % kKeys);
            st->erase(key(k));
            present[k] = false;
            erased.push_back(k);
        }
        for (int k : erased) {
            version[k]++;
            auto t0 = std::chrono::steady_clock::now();
            st->put(key(k), makeDoc(k, version[k]));
            r.put.add(t0);
            present[k] = true;
        }
        for (int i = 0; i < kGetsPerRound; ++i) {
            const int k = static_cast<int>(rng() % kKeys);
            auto t0 = std::chrono::steady_clock::now();
            std::optional<std::string> got = st->get(key(k));
            r.get.add(t0);
            CHECK(got.has_value() == present[k]);
        }
    }
    r.rounds_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - r0).count();
    if (gc_on) st->stopGc();
    r.vlog_before = vlogDiskBytes(dir + "/vlog");
    r.amp_before = st->gcStats().space_amplification;
    if (!gc_on) {
        const auto c0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100000; ++i) {
            bpt::GcReport g = st->runGcPass();
            if (g.action == bpt::GcReport::Action::None && !st->gcPending()) break;
            st->checkpoint();
        }
        r.collect_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        r.vlog_after = vlogDiskBytes(dir + "/vlog");
        r.amp_after = st->gcStats().space_amplification;
    }
    const bpt::GcStats g = st->gcStats();
    r.gc_bytes = g.relocated_bytes;
    r.unlinked = g.segments_unlinked;
    r.passes = g.passes;
    r.verify_bytes = g.verify_scan_bytes;
    r.mismatches = g.counter_mismatches;
    r.live_keys = st->tree().size();
    r.store_vlog_bytes = st->vlogBytesWritten();
    CHECK(st->verifyCountersForTest().empty());
    st->close();
    accountBytes(r.store_vlog_bytes, "bench vLog writes");
    CHECK(bytesWritten() <= kBenchBudget);
    if (!gc_on) {
        auto st2 = IndexedStore::open(dir, benchOpts(false));
        r.reopen = st2->gcOpenStats();
        accountBytes(st2->vlogBytesWritten(), "bench reopen");
        st2->close();
    }
    std::filesystem::remove_all(dir);
    return r;
}

} // namespace

int main() {
    std::string dir = setupRun("bench_gc");
    Cleaner cleaner{dir};
    printHardware();
    // Planned vLog bytes per run: load 10,000 x ~1.07 KB; per round 3,500 puts x ~1.07 KB,
    // 3,000 updates (mostly ~120-byte Deltas, every 5th a ~1.07 KB consolidated Full), 500
    // tombstones; plus GC relocation (at most the live data once per collection cycle).
    const double load = kKeys * 1072.0;
    const double round = (kOverwrites + kErases) * 1072.0 + kUpdates * (0.8 * 128 + 0.2 * 1072) +
                         kErases * 48.0;
    std::printf("Planned: about %.0f MB of vLog writes per run (2 runs) plus GC relocation and "
                "index.db checkpoints, budget %.0f MiB; files are deleted after each run.\n\n",
                (load + kRounds * round) / 1e6, kBenchBudget / 1048576.0);
    std::fflush(stdout);

    RunResult off = run(dir + "/off", false);
    RunResult on = run(dir + "/on", true);

    std::printf("Workload: %d keys x 1 KB, %d rounds of %d overwrites + %d updates + %d erases + "
                "%d re-puts, 1 MiB segments\n\n",
                kKeys, kRounds, kOverwrites, kUpdates, kErases, kErases);
    std::printf("GC off, then collected to idle (%llu passes, %.1f s):\n",
                (unsigned long long)off.passes, off.collect_s);
    std::printf("  vLog on disk before GC   %10.1f MB   space amplification %.2fx\n",
                off.vlog_before / 1e6, off.amp_before);
    std::printf("  vLog on disk after GC    %10.1f MB   space amplification %.2fx\n",
                off.vlog_after / 1e6, off.amp_after);
    std::printf("  GC bytes written         %10.1f MB   segments unlinked %llu\n",
                off.gc_bytes / 1e6, (unsigned long long)off.unlinked);
    std::printf("  verification scans read  %10.1f MB   counter mismatches %llu\n",
                off.verify_bytes / 1e6, (unsigned long long)off.mismatches);
    std::printf("  next open: counter rebuild %.1f ms (%llu keys, %llu head reads, %llu chain "
                "reads)\n\n",
                off.reopen.ms, (unsigned long long)off.reopen.keys_walked,
                (unsigned long long)off.reopen.head_reads,
                (unsigned long long)off.reopen.chain_reads);
    std::printf("GC on (background thread, 32 MB/s limit):\n");
    std::printf("  vLog on disk at the end  %10.1f MB   space amplification %.2fx\n",
                on.vlog_before / 1e6, on.amp_before);
    std::printf("  GC bytes written         %10.1f MB   segments unlinked %llu, %llu passes\n\n",
                on.gc_bytes / 1e6, (unsigned long long)on.unlinked,
                (unsigned long long)on.passes);
    std::printf("Foreground latency (one thread, every op timed):\n");
    std::printf("  %-8s %12s %12s %12s %12s\n", "op", "off p50 us", "off p99 us", "on p50 us",
                "on p99 us");
    std::printf("  %-8s %12.1f %12.1f %12.1f %12.1f\n", "put", off.put.pct(0.5), off.put.pct(0.99),
                on.put.pct(0.5), on.put.pct(0.99));
    std::printf("  %-8s %12.1f %12.1f %12.1f %12.1f\n", "update", off.upd.pct(0.5),
                off.upd.pct(0.99), on.upd.pct(0.5), on.upd.pct(0.99));
    std::printf("  %-8s %12.1f %12.1f %12.1f %12.1f\n", "get", off.get.pct(0.5), off.get.pct(0.99),
                on.get.pct(0.5), on.get.pct(0.99));
    std::printf("  rounds took %.1f s (GC off) and %.1f s (GC on)\n", off.rounds_s, on.rounds_s);
    std::printf("\n[Budget] bytes written: %llu of %llu\n", (unsigned long long)bytesWritten(),
                (unsigned long long)kBenchBudget);
    return 0;
}
