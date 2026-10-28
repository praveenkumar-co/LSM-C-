// High-scale stress benchmark (make bench-stress). Not part of any test mode.
//
//   Main runs (GC off, then GC on; a fresh store each, same seed):
//     - load 100,000 keys x 1 KB documents with 16 threads, B+Tree cache of 1,024 frames (4 MiB)
//     - 100,000 mixed operations on 16 threads: 35% put, 45% update ($inc profile.score and
//       $set profile.tag), 10% erase, 10% get. Each thread owns the keys k with k % 16 == t, so
//       it knows every key's exact document and checks every get() byte for byte.
//     - every operation timed (steady_clock, µs); space amplification at baseline (after the
//       load), at peak churn (after the mixed phase) and after compaction (GC off: GC passes to
//       idle; GC on: the background thread until it stays idle).
//   Chaos: 50 x fork + kill -9 during a 16-thread load on a separate store (4,096 keys,
//     background GC on). After each crash: every acked operation is present (an in-flight one
//     may or may not be), and the index equals a full cold rebuild from the vLog.
#include "../tests/test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexedStore.hpp"

#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kKeys = 100000;
constexpr int kThreads = 16;
constexpr int kMixedOps = 100000;
constexpr size_t kCacheFrames = 1024; // 4 MiB of 4 KiB pages
constexpr uint32_t kSegment = 4 * 1024 * 1024;
// Write cap: the Makefile passes -DBENCH_BUDGET_BYTES (2 GiB); test_common.hpp enforces it.
#ifndef BENCH_BUDGET_BYTES
#error "build with make bench-stress (it sets BENCH_BUDGET_BYTES)"
#endif
constexpr uint64_t kBenchBudget = kWriteBudgetBytes;
constexpr int kCrashes = 50;
constexpr int kChaosKeys = 4096;
constexpr int kChaosOpsPerThread = 600; // cap per child thread (the kill comes earlier)

// ------------------------------------------------------------------------------------------
// Documents: {"id":k,"v":version,"profile":{"score":s,"tag":"t<tag>"},"pad":"..."}, about 1 KB.
// An update applies $inc profile.score 1 and $set profile.tag "t<new tag>", so the document
// after any sequence of operations is determined by (version, score, tag).
// ------------------------------------------------------------------------------------------
std::string makeDoc(int k, uint64_t version, int64_t score, uint64_t tag) {
    Value v = Value::object();
    v.set("id", Value::integer(k));
    v.set("v", Value::integer(static_cast<int64_t>(version)));
    Value p = Value::object();
    p.set("score", Value::integer(score));
    p.set("tag", Value::string("t" + std::to_string(tag)));
    v.set("profile", std::move(p));
    v.set("pad", Value::string(std::string(940, static_cast<char>('a' + k % 26))));
    return doc::serialize(v);
}

std::vector<doc::UpdateOp> updateOps(uint64_t tag) {
    return {doc::inc("profile.score", 1), doc::set("profile.tag", Value::string("t" + std::to_string(tag)))};
}

std::string key(int k) {
    char b[16];
    std::snprintf(b, sizeof(b), "k%06d", k);
    return b;
}

struct KeyState {
    bool present = false;
    uint64_t version = 0;
    int64_t score = 0;
    uint64_t tag = 0;
    std::string doc(int k) const { return makeDoc(k, version, score, tag); }
};

// ------------------------------------------------------------------------------------------
// Output helpers
// ------------------------------------------------------------------------------------------
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

struct Lat {
    std::vector<double> us;
    void merge(const Lat& o) { us.insert(us.end(), o.us.begin(), o.us.end()); }
    void sortNow() { std::sort(us.begin(), us.end()); }
    double pct(double p) const {
        if (us.empty()) return 0;
        return us[std::min(us.size() - 1, static_cast<size_t>(p * us.size()))];
    }
    double max() const { return us.empty() ? 0 : us.back(); }
};

double since(Clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

uint64_t vlogDiskBytes(const std::string& vdir) {
    uint64_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(vdir))
        if (e.is_regular_file()) n += e.file_size();
    return n;
}

std::atomic<uint64_t> g_index_bytes{0};

// Tuning knobs for the experiments in the README (defaults = the engine's defaults, except
// checkpoint_bytes, which this benchmark has always set to 16 MiB). The values used are printed.
double envNum(const char* name, double def) {
    const char* v = std::getenv(name);
    return v ? std::strtod(v, nullptr) : def;
}
uint32_t knobWindowUs() {
    return static_cast<uint32_t>(
        envNum("STRESS_COMMIT_WINDOW_US", vlog::VLogWriter::Options().group_commit_window_us));
}
uint64_t knobCheckpointBytes() {
    return static_cast<uint64_t>(envNum("STRESS_CHECKPOINT_MB", 16) * 1024 * 1024);
}
double knobGcThreshold() { return envNum("STRESS_GC_THRESHOLD", IndexedStore::Options().gc_threshold); }
bool knobSkipChaos() { return envNum("STRESS_SKIP_CHAOS", 0) != 0; }
bool knobOnlyChaos() { return envNum("STRESS_ONLY_CHAOS", 0) != 0; }

IndexedStore::Options storeOpts(bool gc) {
    IndexedStore::Options o;
    o.cache_frames = kCacheFrames;
    o.vlog_opts = vlog::VLogWriter::Options(kSegment, kBenchBudget);
    o.vlog_opts.group_commit_window_us = knobWindowUs();
    o.on_index_write = [](uint64_t n) {
        g_index_bytes += n;
        accountBytes(n, "index.db / superblock.db writes");
    };
    o.checkpoint_bytes = knobCheckpointBytes();
    o.gc_threshold = knobGcThreshold();
    o.gc_enabled = gc;
    o.gc_interval_ms = 10;
    o.gc_rate_mb_s = 32;
    return o;
}

// ------------------------------------------------------------------------------------------
// Main runs
// ------------------------------------------------------------------------------------------
enum Cls { kPut, kUpdDelta, kUpdFull, kUpdMissing, kErase, kGet0, kGetChain, kGetMissing, kNumCls };
const char* kClsName[kNumCls] = {"put",          "update (Delta)", "update (consolidated)",
                                 "update (absent key)", "erase", "get (depth 0)",
                                 "get (chain, depth 1-4)", "get (absent key)"};

struct Space {
    uint64_t disk = 0, live = 0;
    double amp = 0;
    uint32_t segments = 0;
};
Space space(IndexedStore& st) {
    bpt::GcStats g = st.gcStats();
    return Space{vlogDiskBytes(st.vlogDir()), g.live_bytes, g.space_amplification,
                 static_cast<uint32_t>(g.segments.size())};
}

struct RunResult {
    bool gc = false;
    double load_s = 0, mixed_s = 0, compact_s = 0;
    Lat cls[kNumCls];
    Lat put_all, upd_all, get_all;
    uint64_t gets_checked = 0;
    Space base, peak, post;
    uint64_t gc_bytes = 0, unlinked = 0, mismatches = 0, verify_scans = 0;
    std::string counters_check;
    uint64_t mixed_page_reads = 0, mixed_evictions = 0, load_evictions = 0;
    uint32_t index_pages = 0, height = 0;
    uint64_t index_bytes_written = 0, vlog_bytes_written = 0;
    uint64_t index_load = 0, index_mixed = 0, vlog_load = 0, vlog_mixed = 0;
    uint64_t keys_end = 0;
    double avg_batch = 0;      // entries per group-commit write + F_FULLFSYNC (whole run)
    uint64_t cp_count = 0;     // checkpoints written (not skipped)
    double cp_stall_p50 = 0, cp_stall_max = 0, cp_stall_sum = 0; // ms the locks were held
};

RunResult run(const std::string& dir, bool gc_on) {
    RunResult r;
    r.gc = gc_on;
    const uint64_t index0 = g_index_bytes.load();
    auto st = IndexedStore::open(dir, storeOpts(gc_on));
    std::vector<KeyState> model(kKeys);
    // Load: 16 threads, thread t puts the keys k % 16 == t. Timed as a whole.
    auto t0 = Clock::now();
    {
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; ++t)
            th.emplace_back([&, t] {
                for (int k = t; k < kKeys; k += kThreads) {
                    st->put(key(k), makeDoc(k, 0, 0, 0));
                    model[k].present = true;
                }
            });
        for (auto& x : th) x.join();
    }
    r.load_s = since(t0) / 1e6;
    r.index_load = g_index_bytes.load() - index0;
    r.vlog_load = st->vlogBytesWritten();
    r.load_evictions = st->pager().stats().evictions;
    r.base = space(*st);

    // Mixed phase.
    std::printf("phase: mixed start (GC %s)\n", gc_on ? "on" : "off");
    std::fflush(stdout);
    const bpt::Pager::Stats ps0 = st->pager().stats();
    std::vector<std::array<Lat, kNumCls>> lat(kThreads);
    std::atomic<uint64_t> checked{0};
    t0 = Clock::now();
    {
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; ++t)
            th.emplace_back([&, t] {
                std::mt19937_64 rng(1000 + t);
                const int per_thread = kMixedOps / kThreads;
                uint64_t next_id = (static_cast<uint64_t>(t) + 1) << 32;
                for (int i = 0; i < per_thread; ++i) {
                    const int k = t + kThreads * static_cast<int>(rng() % (kKeys / kThreads));
                    KeyState& ks = model[k];
                    const int roll = static_cast<int>(rng() % 100);
                    const std::string kk = key(k);
                    if (roll < 35) {
                        const uint64_t ver = ++next_id;
                        const std::string d = makeDoc(k, ver, 0, 0);
                        auto a = Clock::now();
                        st->put(kk, d);
                        lat[t][kPut].us.push_back(since(a));
                        ks = KeyState{true, ver, 0, 0};
                    } else if (roll < 80) {
                        const uint64_t tag = ++next_id;
                        auto a = Clock::now();
                        try {
                            bpt::UpdateResult u = st->update(kk, updateOps(tag));
                            const double us = since(a);
                            CHECK(ks.present && u.written);
                            lat[t][u.consolidated ? kUpdFull : kUpdDelta].us.push_back(us);
                            ks.score++;
                            ks.tag = tag;
                        } catch (const doc::NotFoundError&) {
                            lat[t][kUpdMissing].us.push_back(since(a));
                            CHECK(!ks.present);
                        }
                    } else if (roll < 90) {
                        auto a = Clock::now();
                        st->erase(kk);
                        lat[t][kErase].us.push_back(since(a));
                        ks.present = false;
                    } else {
                        bpt::ReadStats rs;
                        auto a = Clock::now();
                        std::optional<std::string> got = st->get(kk, nullptr, &rs);
                        const double us = since(a);
                        if (!ks.present) {
                            CHECK(!got);
                            lat[t][kGetMissing].us.push_back(us);
                        } else {
                            CHECK(got && *got == ks.doc(k)); // chain reconstruction checked
                            lat[t][rs.chain_depth == 0 ? kGet0 : kGetChain].us.push_back(us);
                            checked++;
                        }
                    }
                }
            });
        for (auto& x : th) x.join();
    }
    r.mixed_s = since(t0) / 1e6;
    r.index_mixed = g_index_bytes.load() - index0 - r.index_load;
    r.vlog_mixed = st->vlogBytesWritten() - r.vlog_load;
    const bpt::Pager::Stats ps1 = st->pager().stats();
    r.mixed_page_reads = ps1.page_reads - ps0.page_reads;
    r.mixed_evictions = ps1.evictions - ps0.evictions;
    r.gets_checked = checked.load();
    for (int c = 0; c < kNumCls; ++c) {
        for (int t = 0; t < kThreads; ++t) r.cls[c].merge(lat[t][c]);
        r.cls[c].sortNow();
    }
    for (int c : {kPut}) r.put_all.merge(r.cls[c]);
    for (int c : {kUpdDelta, kUpdFull}) r.upd_all.merge(r.cls[c]);
    for (int c : {kGet0, kGetChain}) r.get_all.merge(r.cls[c]);
    r.put_all.sortNow();
    r.upd_all.sortNow();
    r.get_all.sortNow();
    r.peak = space(*st);

    // Compaction.
    t0 = Clock::now();
    if (!gc_on) {
        for (int i = 0; i < 1000000; ++i) {
            bpt::GcReport g = st->runGcPass();
            if (g.action == bpt::GcReport::Action::None && !st->gcPending()) break;
            st->checkpoint();
        }
    } else {
        // The background thread until nothing changes for 3 s and nothing waits (at most 600 s).
        bpt::GcStats last = st->gcStats();
        auto stable_since = Clock::now();
        while (since(t0) < 600e6) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            bpt::GcStats g = st->gcStats();
            const bool moved = g.relocated_bytes != last.relocated_bytes ||
                               g.segments_unlinked != last.segments_unlinked;
            last = g;
            if (moved || st->gcPending()) stable_since = Clock::now();
            if (since(stable_since) >= 3e6) break;
        }
        st->stopGc();
    }
    r.compact_s = since(t0) / 1e6;
    r.post = space(*st);
    const bpt::GcStats g = st->gcStats();
    r.gc_bytes = g.relocated_bytes;
    r.unlinked = g.segments_unlinked;
    r.mismatches = g.counter_mismatches;
    r.verify_scans = g.verify_scans;
    r.counters_check = st->verifyCountersForTest();
    r.index_pages = st->pager().pageCount();
    r.height = st->pager().treeHeight();
    r.keys_end = st->tree().size();
    // Every key once more, after compaction.
    for (int k = 0; k < kKeys; ++k) {
        std::optional<std::string> got = st->get(key(k));
        CHECK(model[k].present ? got == model[k].doc(k) : !got);
    }
    r.vlog_bytes_written = st->vlogBytesWritten();
    r.avg_batch = st->vlogAverageBatch();
    {
        std::vector<double> stalls;
        for (const bpt::CheckpointReport& c : st->checkpointReports()) stalls.push_back(c.stall_ms);
        std::sort(stalls.begin(), stalls.end());
        r.cp_count = stalls.size();
        if (!stalls.empty()) {
            r.cp_stall_p50 = stalls[stalls.size() / 2];
            r.cp_stall_max = stalls.back();
            for (double x : stalls) r.cp_stall_sum += x;
        }
    }
    st->close();
    st.reset();
    r.index_bytes_written = g_index_bytes.load() - index0;
    accountBytes(r.vlog_bytes_written, "vLog writes");
    CHECK(bytesWritten() <= kBenchBudget);
    std::filesystem::remove_all(dir);
    return r;
}

// ------------------------------------------------------------------------------------------
// Chaos: fork + kill -9 during a 16-thread load
// ------------------------------------------------------------------------------------------
// Records the child sends through the pipe (16 bytes each, below PIPE_BUF, so writes from many
// threads never interleave). An intent is sent before an operation, an ack after it returns.
struct Rec {
    uint32_t key;
    uint8_t kind; // 0 intent, 1 ack
    uint8_t op;   // 0 put, 1 update, 2 erase
    uint8_t notfound;
    uint8_t pad;
    uint64_t arg; // put: version; update: tag; ack: vLog bytes written so far
};
static_assert(sizeof(Rec) == 16);

bool readRec(int fd, Rec* r) {
    while (true) {
        ssize_t n = read(fd, r, sizeof(*r));
        if (n == static_cast<ssize_t>(sizeof(*r))) return true;
        if (n == 0) return false;
        CHECK(n < 0 && errno == EINTR);
    }
}

void writeRec(int fd, const Rec& r) {
    CHECK(write(fd, &r, sizeof(r)) == static_cast<ssize_t>(sizeof(r)));
}

IndexedStore::Options chaosOpts() {
    IndexedStore::Options o = storeOpts(true);
    o.vlog_opts = vlog::VLogWriter::Options(256 * 1024, 64ULL * 1024 * 1024);
    o.checkpoint_bytes = 1024 * 1024;
    o.gc_interval_ms = 5;
    return o;
}

KeyState applyOp(KeyState s, uint8_t op, uint64_t arg) {
    if (op == 0) return KeyState{true, arg, 0, 0};
    if (op == 2) return KeyState{};
    if (!s.present) return s; // update of an absent key: NotFound, nothing written
    s.score++;
    s.tag = arg;
    return s;
}

struct ChaosResult {
    int crashes = 0;
    uint64_t acked = 0, verified = 0, in_flight = 0, in_flight_applied = 0;
    uint64_t rebuild_checks = 0, keys_compared = 0;
    double recover_ms_sum = 0, recover_ms_max = 0, rebuild_ms_sum = 0;
    uint64_t gc_unlinked_seen = 0;
    uint64_t vlog_bytes_end = 0; // chaos store's vLog on disk after the last crash
};

std::map<int, std::optional<std::string>> readAll(IndexedStore& st) {
    std::map<int, std::optional<std::string>> m;
    for (int k = 0; k < kChaosKeys; ++k) m[k] = st.get(key(k));
    return m;
}

ChaosResult chaos(const std::string& dir) {
    ChaosResult cr;
    std::vector<KeyState> model(kChaosKeys);
    std::mt19937 rng(9);
    for (int c = 0; c < kCrashes; ++c) {
        const size_t kill_at = 400 + rng() % 4000;
        int pfd[2];
        CHECK(pipe(pfd) == 0);
        std::cout.flush();
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            childExpectsAbort();
            CHECK(close(pfd[0]) == 0);
            auto st = IndexedStore::open(dir, chaosOpts());
            std::vector<std::thread> th;
            for (int t = 0; t < kThreads; ++t)
                th.emplace_back([&, t] {
                    std::mt19937_64 r(static_cast<uint64_t>(c) * 1000 + t);
                    uint64_t next = (static_cast<uint64_t>(c + 1) << 40) |
                                    (static_cast<uint64_t>(t) << 32);
                    for (int i = 0; i < kChaosOpsPerThread; ++i) {
                        const int k = t + kThreads * static_cast<int>(r() % (kChaosKeys / kThreads));
                        const int roll = static_cast<int>(r() % 100);
                        if (roll >= 90) {
                            (void)st->get(key(k)); // gets change nothing: no records
                            continue;
                        }
                        Rec in{static_cast<uint32_t>(k), 0, 0, 0, 0, ++next};
                        in.op = roll < 35 ? 0 : (roll < 80 ? 1 : 2);
                        writeRec(pfd[1], in);
                        Rec ack = in;
                        ack.kind = 1;
                        if (in.op == 0) {
                            st->put(key(k), makeDoc(k, in.arg, 0, 0));
                        } else if (in.op == 1) {
                            try {
                                st->update(key(k), updateOps(in.arg));
                            } catch (const doc::NotFoundError&) {
                                ack.notfound = 1;
                            }
                        } else {
                            st->erase(key(k));
                        }
                        ack.arg = st->vlogBytesWritten();
                        writeRec(pfd[1], ack);
                    }
                });
            for (auto& x : th) x.join();
            while (true) pause(); // the kill comes before the op cap in practice
        }
        CHECK(close(pfd[1]) == 0);
        std::map<uint32_t, Rec> pending; // key -> intent without an ack yet
        uint64_t acks = 0, child_vlog_bytes = 0;
        Rec rec;
        auto consume = [&](const Rec& x) {
            if (x.kind == 0) {
                pending[x.key] = x;
                return;
            }
            auto it = pending.find(x.key);
            CHECK(it != pending.end());
            KeyState& ks = model[x.key];
            if (x.op == 1) CHECK(x.notfound == (ks.present ? 0 : 1));
            ks = applyOp(ks, it->second.op, it->second.arg);
            pending.erase(it);
            child_vlog_bytes = std::max<uint64_t>(child_vlog_bytes, x.arg);
            acks++;
        };
        while (acks < kill_at && readRec(pfd[0], &rec)) consume(rec);
        CHECK(acks == kill_at);
        CHECK(kill(pid, SIGKILL) == 0);
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGKILL);
        while (readRec(pfd[0], &rec)) consume(rec);
        CHECK(close(pfd[0]) == 0);
        accountBytes(child_vlog_bytes, "chaos child vLog writes (up to its last ack)");
        cr.acked += acks;

        // Recover: every acked operation present; an in-flight one either applied or not.
        auto t0 = Clock::now();
        auto st = IndexedStore::open(dir, storeOpts(false));
        const double rec_ms = since(t0) / 1e3;
        cr.recover_ms_sum += rec_ms;
        cr.recover_ms_max = std::max(cr.recover_ms_max, rec_ms);
        CHECK(!st->openInfo().rebuilt);
        for (int k = 0; k < kChaosKeys; ++k) {
            std::optional<std::string> got = st->get(key(k));
            const KeyState& before = model[k];
            auto pit = pending.find(static_cast<uint32_t>(k));
            auto matches = [&](const KeyState& s) {
                return s.present ? got == s.doc(k) : !got.has_value();
            };
            if (pit == pending.end()) {
                if (!matches(before))
                    std::cerr << "crash " << c << ": key " << k << " lost an acked operation\n";
                CHECK(matches(before));
            } else {
                const KeyState after = applyOp(before, pit->second.op, pit->second.arg);
                const bool a = matches(after), b = matches(before);
                CHECK(a || b);
                cr.in_flight++;
                if (a && !b) cr.in_flight_applied++;
                if (a && !b) model[k] = after;
            }
        }
        cr.verified += acks;
        // The index equals a full cold rebuild from the vLog.
        const std::map<int, std::optional<std::string>> live = readAll(*st);
        const uint64_t keys_live = st->tree().size();
        st->close();
        st.reset();
        CHECK(::unlink((dir + "/superblock.db").c_str()) == 0);
        t0 = Clock::now();
        st = IndexedStore::open(dir, storeOpts(false));
        cr.rebuild_ms_sum += since(t0) / 1e3;
        CHECK(st->openInfo().rebuilt);
        CHECK(readAll(*st) == live && st->tree().size() == keys_live);
        CHECK(st->verifyCountersForTest().empty());
        cr.rebuild_checks++;
        cr.keys_compared += kChaosKeys;
        accountBytes(st->vlogBytesWritten(), "recovery opens");
        st->close();
        cr.crashes++;
    }
    cr.vlog_bytes_end = vlogDiskBytes(dir + "/vlog");
    std::filesystem::remove_all(dir);
    return cr;
}

// ------------------------------------------------------------------------------------------
// Report
// ------------------------------------------------------------------------------------------
void latRow(const char* name, const Lat& off, const Lat& on) {
    std::printf("| %s | %zu | %.0f | %.0f | %.0f | %.0f | %.0f | %zu | %.0f | %.0f | %.0f | %.0f | %.0f |\n",
                name, off.us.size(), off.pct(0.50), off.pct(0.90), off.pct(0.99), off.pct(0.999),
                off.max(), on.us.size(), on.pct(0.50), on.pct(0.90), on.pct(0.99), on.pct(0.999),
                on.max());
}

void spaceRow(const char* when, const Space& off, const Space& on) {
    std::printf("| %s | %.1f MB | %.1f MB | %.2fx | %u | %.1f MB | %.1f MB | %.2fx | %u |\n", when,
                off.disk / 1e6, off.live / 1e6, off.amp, off.segments, on.disk / 1e6,
                on.live / 1e6, on.amp, on.segments);
}

} // namespace

int main() {
    std::string dir = setupRun("bench_stress");
    Cleaner cleaner{dir};
    printHardware();
    const double doc_bytes = makeDoc(12345, 0, 0, 0).size();
    const double load = kKeys * (doc_bytes + 40);
    const double mixed = kMixedOps * (0.35 * (doc_bytes + 40) + 0.45 * 300 + 0.10 * 48);
    std::printf("Planned: about %.0f MB of vLog writes per main run (2 runs), plus GC relocation "
                "and index writes (index pages evicted from the 4 MiB cache and checkpoints; about "
                "90 MB in a trial GC-off run), plus about 50 x 10 MB for the crash runs; budget "
                "%.0f MiB; files are deleted after each run.\n",
                (load + mixed) / 1e6, kBenchBudget / 1048576.0);
    std::printf("Settings: group_commit_window_us %u, checkpoint_bytes %.0f MiB, gc_threshold "
                "%.2f%s\n",
                knobWindowUs(), knobCheckpointBytes() / 1048576.0, knobGcThreshold(),
                knobSkipChaos() ? ", chaos skipped" : "");
    std::printf("METRIC cfg window_us %u\nMETRIC cfg checkpoint_mb %.0f\nMETRIC cfg gc_threshold %.2f\n",
                knobWindowUs(), knobCheckpointBytes() / 1048576.0, knobGcThreshold());
    std::fflush(stdout);

    if (knobOnlyChaos()) {
        ChaosResult c = chaos(dir + "/chaos");
        std::printf("METRIC chaos recover_ms %.2f\nMETRIC chaos rebuild_ms %.2f\nMETRIC chaos "
                    "vlog_mb_end %.2f\nMETRIC chaos acked %llu verified %llu\n",
                    c.recover_ms_sum / std::max(1, c.crashes), c.rebuild_ms_sum / std::max(1, c.crashes),
                    c.vlog_bytes_end / 1e6, (unsigned long long)c.acked, (unsigned long long)c.verified);
        std::printf("[Budget] bytes written: %llu of %llu\n", (unsigned long long)bytesWritten(),
                    (unsigned long long)kBenchBudget);
        return 0;
    }
    RunResult off = run(dir + "/off", false);
    std::printf("GC-off run done (%.0f s)\n", off.load_s + off.mixed_s + off.compact_s);
    std::fflush(stdout);
    RunResult on = run(dir + "/on", true);
    std::printf("GC-on run done (%.0f s)\n", on.load_s + on.mixed_s + on.compact_s);
    std::fflush(stdout);
    ChaosResult cr = knobSkipChaos() ? ChaosResult{} : chaos(dir + "/chaos");

    std::printf("\n## High-Scale Stress Benchmark\n\n");
    std::printf("%d keys x %.0f-byte documents, %d threads, B+Tree cache %zu frames (%.0f MiB), "
                "%u MiB segments, %d mixed operations (35%% put, 45%% update, 10%% erase, 10%% "
                "get). GC rate limit 32 MB/s.\n\n",
                kKeys, doc_bytes, kThreads, kCacheFrames, kCacheFrames * 4096 / 1048576.0,
                kSegment / (1024 * 1024), kMixedOps);

    std::printf("### Throughput\n\n");
    std::printf("| Phase | GC off ops/s | GC on ops/s |\n|---|---:|---:|\n");
    std::printf("| Load (100,000 puts, 16 threads) | %.0f | %.0f |\n", kKeys / off.load_s,
                kKeys / on.load_s);
    std::printf("| Mixed (100,000 ops, 16 threads) | %.0f | %.0f |\n", kMixedOps / off.mixed_s,
                kMixedOps / on.mixed_s);
    auto writes = [](const RunResult& r) {
        return double(r.cls[kPut].us.size() + r.cls[kUpdDelta].us.size() +
                      r.cls[kUpdFull].us.size() + r.cls[kErase].us.size()) /
               r.mixed_s;
    };
    std::printf("| Mixed, writes only | %.0f | %.0f |\n\n", writes(off), writes(on));

    std::printf("### Latency (µs, every operation timed)\n\n");
    std::printf("| Operation | n (off) | p50 | p90 | p99 | p99.9 | max | n (on) | p50 | p90 | p99 | "
                "p99.9 | max |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    latRow("put (all)", off.put_all, on.put_all);
    latRow("update (all)", off.upd_all, on.upd_all);
    latRow("get (all present keys)", off.get_all, on.get_all);
    for (int c : {kUpdDelta, kUpdFull, kUpdMissing, kErase, kGet0, kGetChain, kGetMissing})
        latRow(kClsName[c], off.cls[c], on.cls[c]);
    std::printf("\nEvery get() of a present key was compared byte for byte with the document "
                "its thread expected: %llu (GC off) and %llu (GC on), all equal.\n\n",
                (unsigned long long)off.gets_checked, (unsigned long long)on.gets_checked);

    std::printf("### Space amplification\n\n");
    std::printf("| When | GC off: vLog on disk | live | amplification | segments | GC on: vLog "
                "on disk | live | amplification | segments |\n"
                "|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    spaceRow("Baseline (after loading 100,000 keys)", off.base, on.base);
    spaceRow("Peak churn (after 100,000 mixed ops)", off.peak, on.peak);
    spaceRow("After compaction", off.post, on.post);
    std::printf("\n| GC | Compaction time | GC bytes written | Segments unlinked | Verification "
                "scans | Counter mismatches | Counter recount |\n|---|---:|---:|---:|---:|---:|---|\n");
    for (const RunResult* r : {&off, &on})
        std::printf("| %s | %.1f s | %.1f MB | %llu | %llu | %llu | %s |\n",
                    r->gc ? "on (background thread)" : "off (passes to idle after the run)",
                    r->compact_s, r->gc_bytes / 1e6, (unsigned long long)r->unlinked,
                    (unsigned long long)r->verify_scans, (unsigned long long)r->mismatches,
                    r->counters_check.empty() ? "equal" : r->counters_check.c_str());

    std::printf("\n### Buffer pool (1,024 frames = 4 MiB)\n\n");
    std::printf("| Measure | GC off | GC on |\n|---|---:|---:|\n");
    std::printf("| index.db pages at the end (free pages included) | %u (%.1f MiB) | %u (%.1f MiB) |\n", off.index_pages,
                off.index_pages * 4096 / 1048576.0, on.index_pages,
                on.index_pages * 4096 / 1048576.0);
    std::printf("| Tree height | %u | %u |\n", off.height, on.height);
    std::printf("| Page evictions during the load | %llu | %llu |\n",
                (unsigned long long)off.load_evictions, (unsigned long long)on.load_evictions);
    std::printf("| Page reads during the mixed phase | %llu | %llu |\n",
                (unsigned long long)off.mixed_page_reads, (unsigned long long)on.mixed_page_reads);
    std::printf("| Page evictions during the mixed phase | %llu | %llu |\n",
                (unsigned long long)off.mixed_evictions, (unsigned long long)on.mixed_evictions);
    std::printf("| Page reads per mixed operation | %.3f | %.3f |\n",
                double(off.mixed_page_reads) / kMixedOps, double(on.mixed_page_reads) / kMixedOps);
    std::printf("| vLog bytes written: load / mixed / whole run | %.1f / %.1f / %.1f MB | %.1f / "
                "%.1f / %.1f MB |\n",
                off.vlog_load / 1e6, off.vlog_mixed / 1e6, off.vlog_bytes_written / 1e6,
                on.vlog_load / 1e6, on.vlog_mixed / 1e6, on.vlog_bytes_written / 1e6);
    std::printf("| Index bytes written: load / mixed / whole run | %.1f / %.1f / %.1f MB | %.1f / "
                "%.1f / %.1f MB |\n\n",
                off.index_load / 1e6, off.index_mixed / 1e6, off.index_bytes_written / 1e6,
                on.index_load / 1e6, on.index_mixed / 1e6, on.index_bytes_written / 1e6);
    std::printf("Each leaf cell holds the key and a 24-byte reference (seq, kind, VPtr). For "
                "comparison only (computed, not measured): with the %.0f-byte documents stored in "
                "the leaves, 3 would fit in a 4 KiB page, so %d keys would need at least %d leaf "
                "pages (%.0f MiB), %.0fx the buffer pool.\n\n",
                doc_bytes, kKeys, kKeys / 3, kKeys / 3 * 4096 / 1048576.0,
                (kKeys / 3.0) / kCacheFrames);

    std::printf("### Crash resilience (fork + kill -9 during the 16-thread load)\n\n");
    std::printf("| Measure | Value |\n|---|---:|\n");
    std::printf("| Crashes | %d |\n", cr.crashes);
    std::printf("| Acked operations verified after recovery | %llu of %llu |\n",
                (unsigned long long)cr.verified, (unsigned long long)cr.acked);
    std::printf("| In-flight operations at the kill (applied / not applied) | %llu / %llu |\n",
                (unsigned long long)cr.in_flight_applied,
                (unsigned long long)(cr.in_flight - cr.in_flight_applied));
    std::printf("| Index equal to a full cold rebuild | %llu of %d crashes (%llu key "
                "comparisons) |\n",
                (unsigned long long)cr.rebuild_checks, cr.crashes,
                (unsigned long long)cr.keys_compared);
    std::printf("| Recovery open time, mean / max | %.1f / %.1f ms |\n",
                cr.recover_ms_sum / std::max(1, cr.crashes), cr.recover_ms_max);
    std::printf("| Full rebuild time, mean | %.1f ms |\n\n",
                cr.rebuild_ms_sum / std::max(1, cr.crashes));
    std::printf("Chaos store: %d keys, 256 KiB segments, background GC on, checkpoints every "
                "1 MiB of vLog; each child runs 16 threads and is killed after a random number of "
                "acks (400-4,399).\n\n",
                kChaosKeys);
    std::printf("### Group commit and checkpoints\n\n");
    std::printf("| Measure | GC off | GC on |\n|---|---:|---:|\n");
    std::printf("| Entries per write + F_FULLFSYNC (whole run) | %.2f | %.2f |\n", off.avg_batch,
                on.avg_batch);
    std::printf("| Checkpoints written | %llu | %llu |\n", (unsigned long long)off.cp_count,
                (unsigned long long)on.cp_count);
    std::printf("| Checkpoint stall (locks held), median / max / total | %.1f / %.1f / %.0f ms | "
                "%.1f / %.1f / %.0f ms |\n\n",
                off.cp_stall_p50, off.cp_stall_max, off.cp_stall_sum, on.cp_stall_p50,
                on.cp_stall_max, on.cp_stall_sum);
    // Machine-readable summary (medians over runs are taken from these lines).
    for (const RunResult* r : {&off, &on}) {
        const char* g = r->gc ? "on" : "off";
        std::printf("METRIC %s load_ops_s %.1f\n", g, kKeys / r->load_s);
        std::printf("METRIC %s mixed_ops_s %.1f\n", g, kMixedOps / r->mixed_s);
        const std::pair<const char*, const Lat*> lats[] = {
            {"put", &r->put_all}, {"update", &r->upd_all}, {"get", &r->get_all},
            {"get0", &r->cls[kGet0]}, {"getchain", &r->cls[kGetChain]}, {"erase", &r->cls[kErase]}};
        for (const auto& [name, l] : lats)
            std::printf("METRIC %s %s_us p50 %.1f p90 %.1f p99 %.1f p999 %.1f max %.1f\n", g, name,
                        l->pct(0.5), l->pct(0.9), l->pct(0.99), l->pct(0.999), l->max());
        std::printf("METRIC %s index_mb %.2f\n", g, r->index_bytes_written / 1e6);
        std::printf("METRIC %s index_mixed_mb %.2f\n", g, r->index_mixed / 1e6);
        std::printf("METRIC %s amp_peak %.3f\n", g, r->peak.amp);
        std::printf("METRIC %s amp_post %.3f\n", g, r->post.amp);
        std::printf("METRIC %s gc_mb %.2f\n", g, r->gc_bytes / 1e6);
        std::printf("METRIC %s avg_batch %.2f\n", g, r->avg_batch);
        std::printf("METRIC %s cp_count %llu\n", g, (unsigned long long)r->cp_count);
        std::printf("METRIC %s cp_stall_p50_ms %.2f\n", g, r->cp_stall_p50);
        std::printf("METRIC %s cp_stall_max_ms %.2f\n", g, r->cp_stall_max);
        std::printf("METRIC %s page_reads_per_op %.4f\n", g, double(r->mixed_page_reads) / kMixedOps);
    }
    std::printf("METRIC chaos recover_ms %.2f\n", cr.recover_ms_sum / std::max(1, cr.crashes));
    std::printf("METRIC chaos rebuild_ms %.2f\n", cr.rebuild_ms_sum / std::max(1, cr.crashes));
    std::printf("METRIC chaos vlog_mb_end %.2f\n", cr.vlog_bytes_end / 1e6);
    std::printf("METRIC chaos acked %llu verified %llu\n", (unsigned long long)cr.acked,
                (unsigned long long)cr.verified);
    std::printf("[Budget] bytes written: %llu of %llu\n", (unsigned long long)bytesWritten(),
                (unsigned long long)kBenchBudget);
    return 0;
}
