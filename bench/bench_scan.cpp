// Range scan benchmark (make bench-scan). Not part of any test mode or CI.
//
// Dataset (same as bench-compare's 1 KB dataset): 25,000 documents of 1 KB loaded with 16
// threads, then 10,000 single-field updates (profile.score + 1) at random keys with 16 threads,
// every acked write F_FULLFSYNCed. IndexedStore options as in bench-compare: 1,024 index frames
// (4 MiB), 64 MiB segments, GC on. The store is closed and opened again before the scans.
//
// Workloads (bench_scan [workload ...], default all), each with 1 thread and with 4 threads, 3
// runs on the same dataset; every run and the median are printed:
//   get           point get() of a random key (kGetsPerThread per thread)
//   scan-perkey   100 keys from a random start key: pin() + scan() + get() per key (the path
//                 callers had before scanDocuments; bench-compare's adapter still uses it)
//   scan-batched  100 keys from a random start key: scanDocuments()
//
// Read amplification = bytes read / bytes of documents returned, two ways:
//   - bytes the read path asked the files for (vLog preads + index.db page preads): counted only
//     in the -DSCAN_PROFILE build (bench_scan_profile), which runs the same reads;
//   - bytes the OS read from the disk for this process (macOS proc_pid_rusage
//     ri_diskio_bytesread, Linux /proc/self/io read_bytes): 0 means every read was served from
//     the OS page cache (warm). The bench cannot empty the page cache (that needs purge/sudo).
//
// Safety: the run's temp dir is removed on exit; free disk >= 10 GB is checked at start; every
// byte this process writes is counted by the OS and checked against BENCH_BUDGET_BYTES (256 MiB),
// during the load and after every phase; the vLog writer also gets that cap.
#include "../tests/test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexedStore.hpp"

#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using Clock = std::chrono::steady_clock;

#ifndef BENCH_BUDGET_BYTES
#error "build with make bench-scan (it sets BENCH_BUDGET_BYTES)"
#endif

namespace {

constexpr int kDocs = 25000;
constexpr int kUpdates = 10000;
constexpr int kWriteThreads = 16;
constexpr int kScanLen = 100;
constexpr int kScansPerThread = 2000;
constexpr int kRuns = 3;
constexpr size_t kCacheFrames = 1024; // 4 MiB of 4 KiB pages
constexpr uint32_t kSeed = 20261029;
const int kThreadCounts[] = {1, 4};

#if defined(SCAN_PROFILE)
constexpr bool kProfile = true;
#else
constexpr bool kProfile = false;
#endif

double since(Clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

// Sleep detection (as in bench-compare): a run during which the machine slept is run again.
double sleptSeconds() {
#if defined(__APPLE__)
    const double with = double(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW));
    const double without = double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#else
    timespec a, b;
    clock_gettime(CLOCK_BOOTTIME, &a);
    clock_gettime(CLOCK_MONOTONIC, &b);
    const double with = a.tv_sec * 1e9 + a.tv_nsec, without = b.tv_sec * 1e9 + b.tv_nsec;
#endif
    return (with - without) / 1e9;
}

// ---- OS counters for this process ----
struct OsIo {
    uint64_t read = 0, written = 0;
};
OsIo osIo() {
#if defined(__APPLE__)
    rusage_info_v2 ri{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) != 0) {
        errno = 0;
        bpt::fatal("proc_pid_rusage failed");
    }
    return OsIo{ri.ri_diskio_bytesread, ri.ri_diskio_byteswritten};
#else
    std::ifstream in("/proc/self/io");
    std::string k;
    uint64_t v = 0;
    OsIo r;
    bool got_r = false, got_w = false;
    while (in >> k >> v) {
        if (k == "read_bytes:") r.read = v, got_r = true;
        if (k == "write_bytes:") r.written = v, got_w = true;
    }
    if (!got_r || !got_w) {
        errno = 0;
        bpt::fatal("/proc/self/io has no read_bytes/write_bytes");
    }
    return r;
#endif
}

uint64_t dirBytes(const std::string& d) {
    uint64_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(d, ec))
        if (e.is_regular_file()) n += e.file_size();
    return n;
}

std::string shell(const char* cmd) {
    std::string out;
    if (FILE* f = popen(cmd, "r")) {
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) out += buf;
        pclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// Write cap: OS-counted bytes written by this process since start, and the run dir's size.
uint64_t g_os_written0 = 0;
std::string g_dir;
void checkWriteCap(const char* where) {
    const uint64_t os = osIo().written - g_os_written0;
    const uint64_t files = dirBytes(g_dir);
    if (os > BENCH_BUDGET_BYTES || files > BENCH_BUDGET_BYTES) {
        std::fprintf(stderr,
                     "[Budget Fatal] %s: %llu bytes written (OS), %llu bytes of files; cap %llu. "
                     "Aborting.\n",
                     where, (unsigned long long)os, (unsigned long long)files,
                     (unsigned long long)BENCH_BUDGET_BYTES);
        std::abort();
    }
}

// ---- Dataset (bench-compare's makeDoc and key) ----
std::string makeDoc(int k, size_t size) {
    doc::Value v = doc::Value::object();
    v.set("id", doc::Value::integer(k));
    doc::Value p = doc::Value::object();
    p.set("score", doc::Value::integer(0));
    p.set("name", doc::Value::string("user" + std::to_string(k)));
    v.set("profile", std::move(p));
    v.set("pad", doc::Value::string(""));
    std::string s = doc::serialize(v);
    v.set("pad", doc::Value::string(std::string(size - s.size(), static_cast<char>('a' + k % 26))));
    s = doc::serialize(v);
    if (s.size() != size) {
        errno = 0;
        bpt::fatal("makeDoc size mismatch");
    }
    return s;
}
std::string key(int k) {
    char b[16];
    std::snprintf(b, sizeof(b), "k%07d", k);
    return b;
}

bpt::IndexedStore::Options storeOptions() {
    bpt::IndexedStore::Options o;
    o.cache_frames = kCacheFrames;
    o.vlog_opts = vlog::VLogWriter::Options(64 * 1024 * 1024, BENCH_BUDGET_BYTES);
    o.gc_enabled = true;
    return o;
}

// fn(i) for i in [0, n) on `threads` threads; checks the write cap every 1,000 operations.
template <typename F> void parallelWrites(int n, int threads, F fn) {
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t)
        th.emplace_back([&, t] {
            for (int i = t; i < n; i += threads) {
                fn(i);
                if (i % 1000 == 0) checkWriteCap("dataset build");
            }
        });
    for (auto& x : th) x.join();
}

void buildDataset(const std::string& dir) {
    auto st = bpt::IndexedStore::open(dir, storeOptions());
    parallelWrites(kDocs, kWriteThreads, [&](int i) { st->put(key(i), makeDoc(i, 1024)); });
    st->checkpoint();
    checkWriteCap("after load");
    std::vector<int> keys(kUpdates);
    std::mt19937 rng(kSeed);
    for (int& k : keys) k = static_cast<int>(rng() % kDocs);
    parallelWrites(kUpdates, kWriteThreads, [&](int i) {
        bpt::UpdateResult r = st->update(key(keys[i]), {doc::inc("profile.score", 1)});
        if (!r.written) {
            errno = 0;
            bpt::fatal("update wrote nothing");
        }
    });
    st->close();
    checkWriteCap("after updates");
}

// ---- Workloads ----
// get:          one get() of a random key.
// scan-perkey:  pin() + scan() for the refs + get() per key (the path callers have today; the
//               same pattern as bench-compare's IndexedStore adapter).
// scan-batched: scanDocuments() (one tree walk, batched vLog reads).
size_t opGet(bpt::IndexedStore& st, int k, uint64_t* doc_bytes) {
    std::optional<std::string> d = st.get(key(k));
    if (!d) return 0;
    *doc_bytes += d->size();
    return 1;
}
size_t opScanPerKey(bpt::IndexedStore& st, int start, uint64_t* doc_bytes) {
    bpt::ReadGuard pin = st.pin();
    size_t n = 0;
    for (const auto& kv : st.scan(key(start), key(start + kScanLen), kScanLen)) {
        std::optional<std::string> d = st.get(kv.first);
        if (d) {
            n++;
            *doc_bytes += d->size();
        }
    }
    return n;
}
size_t opScanBatched(bpt::IndexedStore& st, int start, uint64_t* doc_bytes) {
    size_t n = 0;
    for (const auto& kv : st.scanDocuments(key(start), key(start + kScanLen), kScanLen)) {
        n++;
        *doc_bytes += kv.second.size();
    }
    return n;
}

struct Workload {
    const char* name;
    const char* unit;    // "scans" or "gets"
    int ops_per_thread;
    int key_range;       // random keys in [0, key_range)
    size_t docs_per_op;  // every op must return exactly this many documents
    size_t (*op)(bpt::IndexedStore&, int, uint64_t*);
};
constexpr int kGetsPerThread = 20000;
const Workload kWorkloads[] = {
    {"get", "gets", kGetsPerThread, kDocs, 1, opGet},
    {"scan-perkey", "scans", kScansPerThread, kDocs - kScanLen, kScanLen, opScanPerKey},
    {"scan-batched", "scans", kScansPerThread, kDocs - kScanLen, kScanLen, opScanBatched},
};

// ---- One measurement: `threads` threads, w.ops_per_thread ops each ----
struct ThreadResult {
    std::vector<double> lat_us;
    double secs = 0;
    uint64_t docs = 0, doc_bytes = 0;
#if defined(SCAN_PROFILE)
    vlog::ScanProfile prof;
#endif
};
struct RunResult {
    double agg_ops_s = 0, per_thread_ops_s = 0, per_thread_min = 0, per_thread_max = 0;
    double docs_s = 0, p50 = 0, p99 = 0;
    uint64_t docs = 0, doc_bytes = 0;
    uint64_t os_read = 0;
    uint64_t index_page_reads = 0, fd_misses = 0;
    double sum_lat_us = 0;
#if defined(SCAN_PROFILE)
    vlog::ScanProfile prof;
#endif
};

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(p * double(v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

#if defined(SCAN_PROFILE)
void addProfile(vlog::ScanProfile& q, const vlog::ScanProfile& p) {
    q.tree_ns += p.tree_ns;
    q.tree_calls += p.tree_calls;
    q.lock_tree_ns += p.lock_tree_ns;
    q.lock_pager_ns += p.lock_pager_ns;
    q.lock_registry_ns += p.lock_registry_ns;
    q.lock_fdcache_ns += p.lock_fdcache_ns;
    q.fstat_calls += p.fstat_calls;
    q.fstat_ns += p.fstat_ns;
    q.vlog_preads += p.vlog_preads;
    q.vlog_pread_bytes += p.vlog_pread_bytes;
    q.vlog_pread_ns += p.vlog_pread_ns;
    q.index_preads += p.index_preads;
    q.index_pread_bytes += p.index_pread_bytes;
    q.crc_ns += p.crc_ns;
    q.crc_bytes += p.crc_bytes;
    q.delta_ns += p.delta_ns;
    q.delta_resolves += p.delta_resolves;
}
#endif

RunResult measure(bpt::IndexedStore& st, const Workload& w, int threads, uint32_t seed) {
    std::vector<ThreadResult> tr(threads);
    std::vector<std::vector<int>> starts(threads);
    std::mt19937 rng(seed);
    for (auto& s : starts) {
        s.resize(w.ops_per_thread);
        for (int& k : s) k = static_cast<int>(rng() % w.key_range);
    }
    const OsIo io0 = osIo();
    const uint64_t pages0 = st.pager().stats().page_reads;
    const uint64_t fdm0 = st.fdCacheForTest().misses();
    const auto t0 = Clock::now();
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t)
        th.emplace_back([&, t] {
            ThreadResult& r = tr[t];
            r.lat_us.reserve(w.ops_per_thread);
#if defined(SCAN_PROFILE)
            vlog::scanProfile() = vlog::ScanProfile{};
#endif
            const auto a0 = Clock::now();
            for (int i = 0; i < w.ops_per_thread; ++i) {
                const auto a = Clock::now();
                const size_t n = w.op(st, starts[t][i], &r.doc_bytes);
                r.lat_us.push_back(since(a));
                if (n != w.docs_per_op) {
                    errno = 0;
                    bpt::fatal(std::string(w.name) + " returned " + std::to_string(n) +
                               " documents, expected " + std::to_string(w.docs_per_op));
                }
                r.docs += n;
            }
            r.secs = since(a0) / 1e6;
#if defined(SCAN_PROFILE)
            r.prof = vlog::scanProfile();
#endif
        });
    for (auto& x : th) x.join();
    const double wall = since(t0) / 1e6;
    RunResult rr;
    std::vector<double> all;
    double pt_sum = 0;
    rr.per_thread_min = 1e300;
    for (const ThreadResult& r : tr) {
        all.insert(all.end(), r.lat_us.begin(), r.lat_us.end());
        const double s = w.ops_per_thread / r.secs;
        pt_sum += s;
        rr.per_thread_min = std::min(rr.per_thread_min, s);
        rr.per_thread_max = std::max(rr.per_thread_max, s);
        rr.docs += r.docs;
        rr.doc_bytes += r.doc_bytes;
        for (double x : r.lat_us) rr.sum_lat_us += x;
#if defined(SCAN_PROFILE)
        addProfile(rr.prof, r.prof);
#endif
    }
    rr.agg_ops_s = double(threads) * w.ops_per_thread / wall;
    rr.per_thread_ops_s = pt_sum / threads;
    rr.docs_s = double(rr.docs) / wall;
    rr.p50 = pct(all, 0.50);
    rr.p99 = pct(all, 0.99);
    rr.os_read = osIo().read - io0.read;
    rr.index_page_reads = st.pager().stats().page_reads - pages0;
    rr.fd_misses = st.fdCacheForTest().misses() - fdm0;
    return rr;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void row(const std::string& name, const std::vector<double>& v, const char* fmt) {
    std::printf("| %s |", name.c_str());
    for (double x : v) {
        std::printf(" ");
        std::printf(fmt, x);
        std::printf(" |");
    }
    std::printf(" ");
    std::printf(fmt, median(v));
    std::printf(" |\n");
}

void report(const Workload& w, int threads, const std::vector<RunResult>& runs) {
    auto col = [&](std::function<double(const RunResult&)> f) {
        std::vector<double> v;
        for (const RunResult& r : runs) v.push_back(f(r));
        return v;
    };
    const std::string u = w.unit;
    std::printf("### %s, %d thread%s\n\n| Metric | run 1 | run 2 | run 3 | median |\n"
                "|---|---:|---:|---:|---:|\n",
                w.name, threads, threads == 1 ? "" : "s");
    row("aggregate " + u + "/s", col([](const RunResult& r) { return r.agg_ops_s; }), "%.0f");
    row("per-thread " + u + "/s (mean)",
        col([](const RunResult& r) { return r.per_thread_ops_s; }), "%.0f");
    if (threads > 1) {
        row("per-thread " + u + "/s (min)",
            col([](const RunResult& r) { return r.per_thread_min; }), "%.0f");
        row("per-thread " + u + "/s (max)",
            col([](const RunResult& r) { return r.per_thread_max; }), "%.0f");
    }
    row("docs/s", col([](const RunResult& r) { return r.docs_s; }), "%.0f");
    row("p50 latency (us)", col([](const RunResult& r) { return r.p50; }), "%.1f");
    row("p99 latency (us)", col([](const RunResult& r) { return r.p99; }), "%.1f");
    row("document bytes returned (MB)", col([](const RunResult& r) { return r.doc_bytes / 1e6; }),
        "%.2f");
    row("OS disk bytes read (MB)", col([](const RunResult& r) { return r.os_read / 1e6; }),
        "%.3f");
    row("read amplification, OS disk bytes",
        col([](const RunResult& r) { return double(r.os_read) / double(r.doc_bytes); }), "%.4f");
    row("index.db page reads (buffer pool misses)",
        col([](const RunResult& r) { return double(r.index_page_reads); }), "%.0f");
    row("segment fd cache misses", col([](const RunResult& r) { return double(r.fd_misses); }),
        "%.0f");
#if defined(SCAN_PROFILE)
    row("read amplification, bytes requested (vLog + index preads)", col([](const RunResult& r) {
            return double(r.prof.vlog_pread_bytes + r.prof.index_pread_bytes) /
                   double(r.doc_bytes);
        }),
        "%.3f");
    const std::string per_op = u.substr(0, u.size() - 1); // "scan" / "get"
    std::printf("\nTime inside %s, per %s (us, summed over threads / %s):\n\n"
                "| Part | run 1 | run 2 | run 3 | median |\n|---|---:|---:|---:|---:|\n",
                u.c_str(), per_op.c_str(), u.c_str());
    const double n = double(threads) * w.ops_per_thread;
    auto us = [&](std::function<double(const RunResult&)> f) {
        return col([&, f](const RunResult& r) { return f(r) / 1e3 / n; });
    };
    row("total (sum of latencies)", col([&](const RunResult& r) { return r.sum_lat_us / n; }),
        "%.2f");
    row("tree walk, excluding its lock waits", us([](const RunResult& r) {
            return double(r.prof.tree_ns - r.prof.lock_tree_ns - r.prof.lock_pager_ns);
        }),
        "%.2f");
    row("lock wait: tree shared lock",
        us([](const RunResult& r) { return double(r.prof.lock_tree_ns); }), "%.2f");
    row("lock wait: pager mutex", us([](const RunResult& r) { return double(r.prof.lock_pager_ns); }),
        "%.2f");
    row("lock wait: reader registry",
        us([](const RunResult& r) { return double(r.prof.lock_registry_ns); }), "%.2f");
    row("lock wait: segment fd cache",
        us([](const RunResult& r) { return double(r.prof.lock_fdcache_ns); }), "%.2f");
    row("fstat (fd cache)", us([](const RunResult& r) { return double(r.prof.fstat_ns); }),
        "%.2f");
    row("vLog pread", us([](const RunResult& r) { return double(r.prof.vlog_pread_ns); }), "%.2f");
    row("CRC32C", us([](const RunResult& r) { return double(r.prof.crc_ns); }), "%.2f");
    row("Delta decode + apply + serialize",
        us([](const RunResult& r) { return double(r.prof.delta_ns); }), "%.2f");
    row("rest (allocation, copies, timers, ...)", col([&](const RunResult& r) {
            const vlog::ScanProfile& p = r.prof;
            const double parts = double(p.tree_ns + p.lock_registry_ns + p.lock_fdcache_ns +
                                        p.fstat_ns + p.vlog_pread_ns + p.crc_ns + p.delta_ns);
            return (r.sum_lat_us * 1e3 - parts) / 1e3 / n;
        }),
        "%.2f");
    std::printf("\nCounts per %s:\n\n| Count | run 1 | run 2 | run 3 | median |\n"
                "|---|---:|---:|---:|---:|\n",
                per_op.c_str());
    auto per = [&](std::function<double(const RunResult&)> f) {
        return col([&, f](const RunResult& r) { return f(r) / n; });
    };
    row("tree calls", per([](const RunResult& r) { return double(r.prof.tree_calls); }), "%.2f");
    row("vLog pread calls", per([](const RunResult& r) { return double(r.prof.vlog_preads); }),
        "%.2f");
    row("vLog bytes read (KB)",
        per([](const RunResult& r) { return r.prof.vlog_pread_bytes / 1e3; }), "%.2f");
    row("index.db preads", per([](const RunResult& r) { return double(r.prof.index_preads); }),
        "%.2f");
    row("fstat calls (fd cache)", per([](const RunResult& r) { return double(r.prof.fstat_calls); }),
        "%.2f");
    row("documents with a Delta chain",
        per([](const RunResult& r) { return double(r.prof.delta_resolves); }), "%.2f");
#endif
    std::printf("\n");
    std::fflush(stdout);
}

} // namespace

// Usage: bench_scan [workload ...]   (default: every workload this build has)
int main(int argc, char** argv) {
    std::vector<const Workload*> chosen;
    for (int a = 1; a < argc; ++a) {
        const Workload* found = nullptr;
        for (const Workload& w : kWorkloads)
            if (std::string(argv[a]) == w.name) found = &w;
        if (!found) {
            std::fprintf(stderr, "unknown workload %s\n", argv[a]);
            return 2;
        }
        chosen.push_back(found);
    }
    if (chosen.empty())
        for (const Workload& w : kWorkloads) chosen.push_back(&w);

    const std::string dir_root = setupRun("bench_scan"); // checks free disk >= 10 GB
    Cleaner cleaner{dir_root};
    g_dir = dir_root;
    g_os_written0 = osIo().written;
    const std::string dir = dir_root + "/store";

#if defined(__APPLE__)
    const std::string cpu = shell("sysctl -n machdep.cpu.brand_string");
    const std::string mem = shell("sysctl -n hw.memsize");
    const std::string os = "macOS " + shell("sw_vers -productVersion");
#else
    const std::string cpu = shell("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2");
    const std::string mem = shell("awk '/MemTotal/ {print $2 * 1024}' /proc/meminfo");
    const std::string os = shell("uname -sr");
#endif
    std::printf("bench_scan%s: %s, %.0f GB RAM, %s, %u hardware threads\n",
                kProfile ? " (-DSCAN_PROFILE: timers inside the read path)" : "", cpu.c_str(),
                std::strtod(mem.c_str(), nullptr) / 1e9, os.c_str(),
                std::thread::hardware_concurrency());
    std::printf("dataset: %d docs of 1 KB (%d write threads), %d updates profile.score+1 at random "
                "keys; F_FULLFSYNC on every acked write\n",
                kDocs, kWriteThreads, kUpdates);
    std::fflush(stdout);

    const auto tb = Clock::now();
    buildDataset(dir);
    const OsIo after_build = osIo();
    std::printf("build: %.1f s, %.1f MB written (OS counter), %.1f MB of files\n",
                since(tb) / 1e6, (after_build.written - g_os_written0) / 1e6, dirBytes(dir) / 1e6);

    auto st = bpt::IndexedStore::open(dir, storeOptions());
    // Chain depth of every key (this also reads every document once, so the page cache is warm).
    uint64_t depth_hist[vlog::kMaxDeltaChain + 1] = {};
    for (int i = 0; i < kDocs; ++i) {
        bpt::ReadStats rs;
        if (!st->get(key(i), nullptr, &rs)) {
            errno = 0;
            bpt::fatal("missing key " + key(i));
        }
        depth_hist[std::min<int>(rs.chain_depth, vlog::kMaxDeltaChain)]++;
    }
    std::printf("chain depth of the %d keys:", kDocs);
    for (int d = 0; d <= vlog::kMaxDeltaChain; ++d)
        std::printf(" depth %d: %llu%s", d, (unsigned long long)depth_hist[d],
                    d < vlog::kMaxDeltaChain ? "," : "\n");
    std::printf("page cache: warm (the dataset was just written and read once; the bench cannot "
                "empty the OS cache without purge/sudo)\n");
    std::printf("gets: %d per thread; scans: %d keys from a random start key, %d per thread\n\n",
                kGetsPerThread, kScanLen, kScansPerThread);
    std::fflush(stdout);

    for (const Workload* w : chosen) {
        for (int threads : kThreadCounts) {
            std::vector<RunResult> runs;
            for (int r = 0; r < kRuns; ++r) {
                for (int attempt = 0;; ++attempt) {
                    const double s0 = sleptSeconds();
                    RunResult rr = measure(*st, *w, threads, kSeed + 1000u * r + threads);
                    if (sleptSeconds() - s0 < 1.0) {
                        runs.push_back(rr);
                        break;
                    }
                    std::printf("  machine slept during run %d: running it again\n", r + 1);
                    if (attempt == 2) {
                        errno = 0;
                        bpt::fatal("the machine kept sleeping; keep the lid open and run again");
                    }
                }
            }
            report(*w, threads, runs);
        }
    }
    st->close();
    checkWriteCap("end");
    const uint64_t w = osIo().written - g_os_written0;
    accountBytes(w, "bench writes (OS counter)");
    printBudget();
    return 0;
}
