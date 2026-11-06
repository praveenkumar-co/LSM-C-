// Reality-check benchmark (Phase 7). Not part of any test mode. Run it through
// scripts/reality/run_safe.sh, which enforces the machine-safety rules (AC power, free disk,
// swap, thermal, run time, total write budget) from outside the process.
//
// Modes:
//   load   --docs N --threads T [--doc-size B]   put N new documents (keys k000000000...) with
//          T threads, then close() (final checkpoint). Reports throughput, put latency, bytes:
//          logical (key + document), vLog entry bytes, index.db bytes (pages + superblock), and
//          what the OS counted as written to disk for this process (proc_pid_rusage
//          ri_diskio_byteswritten). Write amplification = OS bytes written / logical bytes.
//          Optional: --updates U --update-threads T2 runs U single-field updates
//          (profile.score + 1) at random keys after the load (before close) and reports the OS
//          bytes written per update.
//
//   ops    --docs N [--workloads get,scan,update] [--threads-list 1,4] [--runs 3]
//          [--get-ops 20000] [--scan-ops 2000] [--update-ops 5000]   (ops are per thread)
//          Opens an existing store (--dir must exist; made by load --keep; N = its doc count),
//          timing the open, then for each workload, thread count and run: throughput, p50/p99,
//          RSS (peak during the run), OS bytes read and written. Workloads as in bench-scan:
//          get = get() of a random key; scan = scanDocuments() of 100 keys from a random
//          start key; update = profile.score + 1 at a random key. Deletes the store at the end
//          unless --keep.
//
// Common flags:
//   --dir D            store directory (must not exist; removed at the end unless --keep)
//   --max-write B      hard cap on OS-counted bytes written by this process (or env
//                      REALITY_MAX_WRITE, which run_safe.sh sets to the budget left)
//   --keep             keep the store directory
//   --seg-mb M         vLog segment size (default 64)
//   --cache-frames F   index page frames (default 1024 = 4 MiB, the engine default)
//
// Self-checks (a monitor thread, every 250 ms): OS bytes written <= --max-write, free disk on
// the store's volume >= 50 GB (--min-free-gb), RSS <= 4 GiB (--max-rss-mb). On a breach it
// prints [abort] and exits with code 3 at once. Every 2 s it prints a [mon] line with the OS
// counters, so a run killed from outside still leaves its byte count in the log.
#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexedStore.hpp"
#include "reality_common.hpp"

#include <libproc.h>
#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace reality;

// ---- Args ----
struct Args {
    std::string mode, dir;
    uint64_t docs = 0, max_write = 0;
    int threads = 1;
    uint64_t updates = 0;
    int update_threads = 1;
    std::string workloads = "get,scan,update";
    std::vector<int> threads_list{1, 4};
    int runs = 3;
    uint64_t get_ops = 20000, scan_ops = 2000, update_ops = 5000;
    int verify_load_threads = 0;  // ops: verify a load that stopped part-way (its thread count)
    uint64_t verify_load_min = 0; // ops: documents that must be present
    size_t doc_size = 1024;
    uint32_t seg_mb = 64;
    size_t cache_frames = 1024;
    bool keep = false;
    uint64_t min_free = 50ULL * 1000 * 1000 * 1000;
    uint64_t max_rss = 4096ULL * 1024 * 1024;
};

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr, "bench_reality: %s\nusage: bench_reality load --dir D --docs N "
                         "--threads T [--doc-size B] [--max-write B] [--keep]\n",
                 why);
    std::exit(2);
}

Args parse(int argc, char** argv) {
    Args a;
    if (argc < 2) usage("no mode");
    a.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string f = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) usage(("missing value for " + f).c_str());
            return argv[++i];
        };
        if (f == "--dir") a.dir = val();
        else if (f == "--docs") a.docs = std::stoull(val());
        else if (f == "--threads") a.threads = std::stoi(val());
        else if (f == "--updates") a.updates = std::stoull(val());
        else if (f == "--update-threads") a.update_threads = std::stoi(val());
        else if (f == "--doc-size") a.doc_size = std::stoull(val());
        else if (f == "--max-write") a.max_write = std::stoull(val());
        else if (f == "--seg-mb") a.seg_mb = uint32_t(std::stoul(val()));
        else if (f == "--cache-frames") a.cache_frames = std::stoull(val());
        else if (f == "--min-free-gb") a.min_free = std::stoull(val()) * 1000ULL * 1000 * 1000;
        else if (f == "--max-rss-mb") a.max_rss = std::stoull(val()) * 1024ULL * 1024;
        else if (f == "--keep") a.keep = true;
        else if (f == "--workloads") a.workloads = val();
        else if (f == "--runs") a.runs = std::stoi(val());
        else if (f == "--get-ops") a.get_ops = std::stoull(val());
        else if (f == "--scan-ops") a.scan_ops = std::stoull(val());
        else if (f == "--update-ops") a.update_ops = std::stoull(val());
        else if (f == "--verify-load-threads") a.verify_load_threads = std::stoi(val());
        else if (f == "--verify-load-min") a.verify_load_min = std::stoull(val());
        else if (f == "--threads-list") {
            a.threads_list.clear();
            std::string v = val();
            for (size_t p = 0; p <= v.size();) {
                size_t q = v.find(',', p);
                if (q == std::string::npos) q = v.size();
                a.threads_list.push_back(std::stoi(v.substr(p, q - p)));
                p = q + 1;
            }
        }
        else usage(("unknown flag " + f).c_str());
    }
    if (a.max_write == 0) {
        if (const char* e = std::getenv("REALITY_MAX_WRITE")) a.max_write = std::stoull(e);
    }
    if (a.max_write == 0) usage("--max-write (or REALITY_MAX_WRITE) is required");
    if (a.dir.empty()) usage("--dir is required");
    if (a.threads < 1 || a.threads > 64) usage("--threads must be 1..64");
    if (a.update_threads < 1 || a.update_threads > 64) usage("--update-threads must be 1..64");
    return a;
}

// ---- Monitor thread: hard limits + periodic [mon] lines ----
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_ops{0};
std::atomic<uint64_t> g_rss_window{0}; // max RSS seen by the monitor since the last reset
OsStats g_os0;
// Engine counters for the [eng] line (load mode): the open store and its index-write counter.
std::mutex g_store_mu;
bpt::IndexedStore* g_store = nullptr;
std::atomic<uint64_t> g_index_bytes{0};

void monitor(const Args a) {
    const auto t0 = Clock::now();
    auto last_print = t0 - std::chrono::seconds(10);
    const std::string vol = std::filesystem::path(a.dir).parent_path().string();
    while (!g_stop.load()) {
        const OsStats os = osStats();
        const uint64_t w = os.written - g_os0.written;
        const uint64_t rss = rssNow();
        for (uint64_t m = g_rss_window.load(); rss > m && !g_rss_window.compare_exchange_weak(m, rss);) {
        }
        const uint64_t fr = freeBytes(vol);
        const char* why = nullptr;
        if (w > a.max_write) why = "OS bytes written above --max-write";
        else if (fr < a.min_free) why = "free disk below the minimum";
        else if (rss > a.max_rss) why = "RSS above the limit";
        const auto now = Clock::now();
        if (why || now - last_print >= std::chrono::seconds(2)) {
            std::printf("[mon] t=%.1fs ops=%llu os_written=%llu os_read=%llu rss=%llu free=%llu\n",
                        std::chrono::duration<double>(now - t0).count(),
                        (unsigned long long)g_ops.load(), (unsigned long long)w,
                        (unsigned long long)(os.read - g_os0.read), (unsigned long long)rss,
                        (unsigned long long)fr);
            {
                std::lock_guard<std::mutex> lk(g_store_mu);
                if (g_store) {
                    // index_db = every index.db page write + superblock writes (on_index_write);
                    // checkpoint = what checkpoints wrote (pages + one superblock slot each);
                    // eviction = the rest (dirty pages written back when evicted from the cache).
                    uint64_t cp_bytes = 0, cp_pages = 0;
                    const auto reps = g_store->checkpointReports();
                    for (const auto& r : reps) cp_bytes += r.bytes_written, cp_pages += r.pages_written;
                    const uint64_t ib = g_index_bytes.load();
                    std::printf("[eng] t=%.1fs ops=%llu vlog_bytes=%llu index_db_bytes=%llu "
                                "checkpoint_bytes=%llu checkpoint_pages=%llu eviction_bytes=%llu "
                                "checkpoints=%zu os_written=%llu\n",
                                std::chrono::duration<double>(now - t0).count(),
                                (unsigned long long)g_ops.load(),
                                (unsigned long long)g_store->vlogBytesWritten(),
                                (unsigned long long)ib, (unsigned long long)cp_bytes,
                                (unsigned long long)cp_pages,
                                (unsigned long long)(ib > cp_bytes ? ib - cp_bytes : 0), reps.size(),
                                (unsigned long long)w);
                }
            }
            std::fflush(stdout);
            last_print = now;
        }
        if (why) {
            std::printf("[abort] %s (os_written=%llu cap=%llu, free=%llu, rss=%llu)\n", why,
                        (unsigned long long)w, (unsigned long long)a.max_write,
                        (unsigned long long)fr, (unsigned long long)rss);
            std::fflush(stdout);
            std::_Exit(3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ---- Dataset: bench-compare's 1 KB JSON document, key k%09d ----
std::string makeDoc(uint64_t k, size_t size) {
    doc::Value v = doc::Value::object();
    v.set("id", doc::Value::integer(int64_t(k)));
    doc::Value p = doc::Value::object();
    p.set("score", doc::Value::integer(0));
    p.set("name", doc::Value::string("user" + std::to_string(k)));
    v.set("profile", std::move(p));
    v.set("pad", doc::Value::string(""));
    std::string s = doc::serialize(v);
    if (s.size() > size) {
        std::fprintf(stderr, "doc size %zu too small\n", size);
        std::_Exit(2);
    }
    v.set("pad", doc::Value::string(std::string(size - s.size(), char('a' + k % 26))));
    s = doc::serialize(v);
    if (s.size() != size) {
        std::fprintf(stderr, "makeDoc size mismatch\n");
        std::_Exit(2);
    }
    return s;
}

int runLoad(const Args& a) {
    if (a.docs == 0) usage("--docs is required for load");
    std::atomic<uint64_t>& index_bytes = g_index_bytes;
    bpt::IndexedStore::Options o;
    o.cache_frames = a.cache_frames;
    o.vlog_opts = vlog::VLogWriter::Options(a.seg_mb * 1024 * 1024, a.max_write);
    o.on_index_write = [&](uint64_t n) { index_bytes += n; };
    o.gc_enabled = true;

    const std::string sample = makeDoc(0, a.doc_size);
    const uint64_t key_len = key(0).size();
    const uint64_t logical = a.docs * (key_len + a.doc_size);
    const uint64_t entry = vlog::entry_size(uint16_t(key_len), uint32_t(a.doc_size));
    std::printf("[config] mode=load docs=%llu threads=%d doc_size=%zu key_len=%llu seg_mb=%u "
                "cache_frames=%zu max_write=%llu logical_bytes=%llu\n",
                (unsigned long long)a.docs, a.threads, a.doc_size, (unsigned long long)key_len,
                a.seg_mb, a.cache_frames, (unsigned long long)a.max_write,
                (unsigned long long)logical);

    auto st = bpt::IndexedStore::open(a.dir, o);
    {
        std::lock_guard<std::mutex> lk(g_store_mu);
        g_store = st.get();
    }
    std::vector<Hist> hist(a.threads);
    std::vector<std::thread> th;
    const auto t0 = Clock::now();
    for (int t = 0; t < a.threads; ++t)
        th.emplace_back([&, t] {
            for (uint64_t i = uint64_t(t); i < a.docs; i += uint64_t(a.threads)) {
                const std::string d = makeDoc(i, a.doc_size);
                const auto s = Clock::now();
                st->put(key(i), d);
                hist[t].add(std::chrono::duration<double, std::micro>(Clock::now() - s).count());
                g_ops++;
            }
        });
    for (auto& x : th) x.join();
    const double load_s = std::chrono::duration<double>(Clock::now() - t0).count();
    const OsStats os_load = osStats();
    if (a.updates > 0) {
        st->checkpoint();
        const OsStats u0 = osStats();
        std::vector<Hist> uh(a.update_threads);
        std::vector<std::thread> ut;
        const auto ts = Clock::now();
        for (int t = 0; t < a.update_threads; ++t)
            ut.emplace_back([&, t] {
                std::mt19937_64 rng(20261105 + t);
                for (uint64_t i = uint64_t(t); i < a.updates; i += uint64_t(a.update_threads)) {
                    const uint64_t k = rng() % a.docs;
                    const auto s = Clock::now();
                    bpt::UpdateResult r = st->update(key(k), {doc::inc("profile.score", 1)});
                    uh[t].add(std::chrono::duration<double, std::micro>(Clock::now() - s).count());
                    if (!r.written) {
                        std::fprintf(stderr, "update wrote nothing\n");
                        std::_Exit(5);
                    }
                    g_ops++;
                }
            });
        for (auto& x : ut) x.join();
        const double us = std::chrono::duration<double>(Clock::now() - ts).count();
        const OsStats u1 = osStats();
        Hist h;
        for (const Hist& x : uh) h.merge(x);
        std::printf("[update] updates=%llu threads=%d secs=%.2f updates_s=%.0f p50_us=%.0f "
                    "p99_us=%.0f os_written=%llu os_bytes_per_update=%.0f os_read=%llu\n",
                    (unsigned long long)a.updates, a.update_threads, us, double(a.updates) / us,
                    h.pct(0.5), h.pct(0.99), (unsigned long long)(u1.written - u0.written),
                    double(u1.written - u0.written) / double(a.updates),
                    (unsigned long long)(u1.read - u0.read));
    }
    {
        std::lock_guard<std::mutex> lk(g_store_mu);
        g_store = nullptr;
    }
    const auto c0 = Clock::now();
    st->close();
    const double close_s = std::chrono::duration<double>(Clock::now() - c0).count();
    const auto reports = st->checkpointReports();
    st.reset();
    const OsStats os = osStats();
    Hist h;
    for (const Hist& x : hist) h.merge(x);
    const uint64_t store_bytes = dirBytes(a.dir);
    const uint64_t vlog_bytes = a.docs * entry; // + one 4 KiB header per segment (in store_bytes)
    const uint64_t w = os.written - g_os0.written;
    const uint64_t w_load = os_load.written - g_os0.written;
    std::printf("[result] load_s=%.2f puts_s=%.0f p50_us=%.0f p99_us=%.0f p999_us=%.0f "
                "close_s=%.2f checkpoints=%zu\n",
                load_s, double(a.docs) / load_s, h.pct(0.50), h.pct(0.99), h.pct(0.999), close_s,
                reports.size());
    std::printf("[bytes] logical=%llu vlog_entries=%llu index_db_writes=%llu store_on_disk=%llu "
                "os_written_load=%llu os_written_total=%llu os_read=%llu\n",
                (unsigned long long)logical, (unsigned long long)vlog_bytes,
                (unsigned long long)index_bytes.load(), (unsigned long long)store_bytes,
                (unsigned long long)w_load, (unsigned long long)w,
                (unsigned long long)(os.read - g_os0.read));
    std::printf("[wa] os_written/logical=%.3f (vlog_entries+index_db)/logical=%.3f "
                "os_bytes_per_put=%.0f\n",
                double(w) / double(logical),
                double(vlog_bytes + index_bytes.load()) / double(logical),
                double(w) / double(a.docs));
    std::printf("[mem] rss_peak=%llu footprint_peak=%llu rss_end=%llu\n",
                (unsigned long long)rssPeak(), (unsigned long long)os.footprint_peak,
                (unsigned long long)rssNow());
    return 0;
}

constexpr int kScanLen = 100;

double pctOf(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}
double median(std::vector<double> v) { return pctOf(v, 0.5); }

int runOps(const Args& a) {
    if (a.docs <= uint64_t(kScanLen)) usage("--docs (the store's doc count) is required for ops");
    bpt::IndexedStore::Options o;
    o.cache_frames = a.cache_frames;
    o.vlog_opts = vlog::VLogWriter::Options(a.seg_mb * 1024 * 1024, a.max_write);
    o.gc_enabled = true;
    const OsStats p0 = osStats();
    const auto t0 = Clock::now();
    auto st = bpt::IndexedStore::open(a.dir, o);
    const double open_s = std::chrono::duration<double>(Clock::now() - t0).count();
    const OsStats p1 = osStats();
    std::printf("[open] secs=%.2f os_read=%llu os_written=%llu rss=%llu keys_walked=%llu\n", open_s,
                (unsigned long long)(p1.read - p0.read),
                (unsigned long long)(p1.written - p0.written), (unsigned long long)rssNow(),
                (unsigned long long)st->gcOpenStats().keys_walked);
    uint64_t range = a.docs; // workloads use keys [0, range)
    if (a.verify_load_threads > 0) {
        // The load gave key i to thread i % T, in increasing order, and acked each put before the
        // next. After a crash every thread's acked keys must be present, so its keys present must
        // form an unbroken run from its first key: no key may be present after a missing one.
        // Every document present must equal what the load wrote, byte for byte.
        const uint64_t T = uint64_t(a.verify_load_threads);
        std::vector<uint64_t> run_len(T, 0), holes(T, 0), bad(T, 0), present(T, 0);
        std::atomic<uint64_t> next_r{0};
        std::vector<std::thread> vt;
        const auto vs = Clock::now();
        for (int t = 0; t < 8; ++t)
            vt.emplace_back([&] {
                for (uint64_t r; (r = next_r++) < T;) {
                    bool missing_seen = false;
                    for (uint64_t k = r; k < a.docs; k += T) {
                        auto d = st->get(key(k));
                        if (!d) {
                            missing_seen = true;
                            continue;
                        }
                        present[r]++;
                        if (missing_seen) holes[r]++;
                        else run_len[r]++;
                        if (*d != makeDoc(k, a.doc_size)) bad[r]++;
                        g_ops++;
                    }
                }
            });
        for (auto& x : vt) x.join();
        uint64_t tot = 0, tot_holes = 0, tot_bad = 0, mn = UINT64_MAX, mx = 0;
        range = UINT64_MAX;
        for (uint64_t r = 0; r < T; ++r) {
            tot += present[r], tot_holes += holes[r], tot_bad += bad[r];
            mn = std::min(mn, run_len[r]), mx = std::max(mx, run_len[r]);
            range = std::min(range, run_len[r] * T + r); // first missing key of thread r
        }
        std::printf("[verify-load] secs=%.1f present=%llu holes=%llu content_mismatches=%llu "
                    "per_thread_run_min=%llu per_thread_run_max=%llu first_missing_key=%llu "
                    "expected_at_least=%llu result=%s\n",
                    std::chrono::duration<double>(Clock::now() - vs).count(),
                    (unsigned long long)tot, (unsigned long long)tot_holes,
                    (unsigned long long)tot_bad, (unsigned long long)mn, (unsigned long long)mx,
                    (unsigned long long)range, (unsigned long long)a.verify_load_min,
                    (tot_holes == 0 && tot_bad == 0 && tot >= a.verify_load_min) ? "OK" : "FAIL");
        if (tot_holes || tot_bad || tot < a.verify_load_min) return 6;
    } else {
        // Spot check: the first, last and a middle key exist with the expected size.
        for (uint64_t k : {uint64_t(0), a.docs / 2, a.docs - 1}) {
            auto d = st->get(key(k));
            if (!d || d->size() != a.doc_size) {
                std::printf("[fail] key %s missing or wrong size\n", key(k).c_str());
                return 6;
            }
        }
    }
    if (range <= uint64_t(kScanLen)) usage("key range too small");
    std::vector<std::string> wl;
    for (size_t p = 0; p <= a.workloads.size();) {
        size_t q = a.workloads.find(',', p);
        if (q == std::string::npos) q = a.workloads.size();
        wl.push_back(a.workloads.substr(p, q - p));
        p = q + 1;
    }
    for (const std::string& w : wl) {
        if (w != "get" && w != "scan" && w != "update") usage(("unknown workload " + w).c_str());
        const uint64_t per_thread =
            w == "get" ? a.get_ops : w == "scan" ? a.scan_ops : a.update_ops;
        for (int threads : a.threads_list) {
            std::vector<double> agg, p50s, p99s;
            for (int run = 1; run <= a.runs; ++run) {
                std::vector<Hist> h(threads);
                std::vector<uint64_t> docs(threads, 0);
                std::vector<std::thread> th;
                g_rss_window = rssNow();
                const OsStats r0 = osStats();
                const auto ts = Clock::now();
                for (int t = 0; t < threads; ++t)
                    th.emplace_back([&, t] {
                        std::mt19937_64 rng(uint64_t(run) * 1000003 + uint64_t(t) * 7919 +
                                            std::hash<std::string>{}(w));
                        for (uint64_t i = 0; i < per_thread; ++i) {
                            const auto s = Clock::now();
                            if (w == "get") {
                                auto d = st->get(key(rng() % range));
                                if (!d) {
                                    std::printf("[fail] get returned nothing\n");
                                    std::_Exit(6);
                                }
                                docs[t]++;
                            } else if (w == "scan") {
                                const uint64_t k = rng() % (range - kScanLen);
                                auto r = st->scanDocuments(key(k), key(k + kScanLen), kScanLen);
                                if (r.size() != size_t(kScanLen)) {
                                    std::printf("[fail] scan returned %zu docs\n", r.size());
                                    std::_Exit(6);
                                }
                                docs[t] += r.size();
                            } else {
                                auto r = st->update(key(rng() % range),
                                                    {doc::inc("profile.score", 1)});
                                if (!r.written) {
                                    std::printf("[fail] update wrote nothing\n");
                                    std::_Exit(6);
                                }
                                docs[t]++;
                            }
                            h[t].add(
                                std::chrono::duration<double, std::micro>(Clock::now() - s).count());
                            g_ops++;
                        }
                    });
                for (auto& x : th) x.join();
                const double secs = std::chrono::duration<double>(Clock::now() - ts).count();
                const OsStats r1 = osStats();
                Hist m;
                for (const Hist& x : h) m.merge(x);
                const double ops_s = double(per_thread * uint64_t(threads)) / secs;
                agg.push_back(ops_s);
                p50s.push_back(m.pct(0.5));
                p99s.push_back(m.pct(0.99));
                const uint64_t rss_peak = std::max<uint64_t>(g_rss_window.load(), rssNow());
                std::printf("[run] workload=%s threads=%d run=%d ops=%llu secs=%.2f ops_s=%.0f "
                            "per_thread_ops_s=%.0f p50_us=%.0f p99_us=%.0f rss_peak=%llu "
                            "os_read=%llu os_written=%llu os_written_per_op=%.0f\n",
                            w.c_str(), threads, run,
                            (unsigned long long)(per_thread * uint64_t(threads)), secs, ops_s,
                            ops_s / threads, m.pct(0.5), m.pct(0.99),
                            (unsigned long long)rss_peak, (unsigned long long)(r1.read - r0.read),
                            (unsigned long long)(r1.written - r0.written),
                            double(r1.written - r0.written) /
                                double(per_thread * uint64_t(threads)));
            }
            std::printf("[median] workload=%s threads=%d ops_s=%.0f p50_us=%.0f p99_us=%.0f\n",
                        w.c_str(), threads, median(agg), median(p50s), median(p99s));
        }
    }
    const auto c0 = Clock::now();
    st->close();
    std::printf("[close] secs=%.2f\n", std::chrono::duration<double>(Clock::now() - c0).count());
    st.reset();
    std::printf("[mem] rss_peak=%llu footprint_peak=%llu store_on_disk=%llu\n",
                (unsigned long long)rssPeak(), (unsigned long long)osStats().footprint_peak,
                (unsigned long long)dirBytes(a.dir));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Args a = parse(argc, argv);
    if (a.mode == "load" && std::filesystem::exists(a.dir)) usage("--dir already exists");
    if (a.mode == "ops" && !std::filesystem::exists(a.dir)) usage("--dir does not exist");
    const std::string vol = std::filesystem::path(a.dir).parent_path().string();
    const uint64_t free0 = freeBytes(vol);
    std::printf("[disk] free_before=%llu\n", (unsigned long long)free0);
    if (free0 < a.min_free) usage("not enough free disk to start");
    g_os0 = osStats();
    std::thread mon(monitor, a);
    int rc = 2;
    if (a.mode == "load") rc = runLoad(a);
    else if (a.mode == "ops") rc = runOps(a);
    else usage("unknown mode");
    const OsStats os = osStats();
    if (!a.keep) {
        std::error_code ec;
        std::filesystem::remove_all(a.dir, ec);
        if (ec) std::printf("[warn] remove_all failed: %s\n", ec.message().c_str());
    }
    std::printf("[disk] free_after=%llu (dataset %s)\n", (unsigned long long)freeBytes(vol),
                a.keep ? "kept" : "deleted");
    std::printf("[final] os_written=%llu os_read=%llu exit=%d\n",
                (unsigned long long)(os.written - g_os0.written),
                (unsigned long long)(os.read - g_os0.read), rc);
    g_stop = true;
    mon.join();
    return rc;
}
