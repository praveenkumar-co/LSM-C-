// Storage engine comparison (make bench-compare). Not part of any test mode or CI.
//
// Engines, behind one BenchStore interface (put, update_field, get, erase, scan_100,
// checkpoint), all with every acked write durable (F_FULLFSYNC on macOS):
//   - IndexedStore (this engine): default options, index cache 1,024 frames (4 MiB), GC on.
//   - SQLite 3 (system libsqlite3): WAL, synchronous=FULL, fullfsync=ON,
//     checkpoint_fullfsync=ON, cache_size=-4096 (4 MiB); one connection per thread.
//   - MongoLite (legacy/, unmodified): 1,024-frame buffer pool (4 MiB). Its WAL does not sync,
//     so this adapter F_FULLFSYNCs (fdatasync on Linux) the WAL file after every operation and
//     the data file at checkpoint, and serializes every call with one mutex (the legacy code has
//     data races).
// Workloads (fresh directory per engine and run, fixed seed): A bulk load, B small updates (the
// headline), C reads after reopen, D disk footprint, E kill -9 during B. Disk bytes are measured
// by the OS for this process (macOS proc_pid_rusage ri_diskio_byteswritten, Linux
// /proc/self/io write_bytes) and cross-checked with file-size growth. Median of 3 runs.
#include "../tests/test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexedStore.hpp"

#include <MongoLite.hpp> // legacy/, included as a system header (-isystem legacy)
#include <sqlite3.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using Clock = std::chrono::steady_clock;

#ifndef BENCH_BUDGET_BYTES
#error "build with make bench-compare (it sets BENCH_BUDGET_BYTES)"
#endif

namespace {

// ------------------------------------------------------------------------------------------
// Scale (COMPARE_SCALE / COMPARE_RUNS only for pilots; the defaults are the specified sizes)
// ------------------------------------------------------------------------------------------
double envNum(const char* n, double d) {
    const char* v = std::getenv(n);
    return v ? std::strtod(v, nullptr) : d;
}
const double kScale = envNum("COMPARE_SCALE", 1.0);
const int kRuns = static_cast<int>(envNum("COMPARE_RUNS", 3));
// Half of the specified 50,000 documents and 20,000 updates (chosen to fit the write cap: the
// full sizes measure about 12 GB per run); reads keep the specified counts.
const int kDocs = static_cast<int>(25000 * kScale);
const int kUpdates = static_cast<int>(10000 * kScale);
const int kGets = static_cast<int>(10000 * std::min(1.0, kScale));
const int kScans = static_cast<int>(1000 * std::min(1.0, kScale));
constexpr int kThreads = 16;
constexpr size_t kCacheFrames = 1024; // 4 MiB of 4 KiB pages
const std::vector<size_t> kSizes = {1024, 2048, 8192};

// Sleep detection: the difference between a clock that counts system sleep and one that does
// not. A workload during which the machine slept more than 1 s is discarded and run again.
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
int g_sleep_retries = 0;
std::map<std::string, std::vector<double>> g_results; // "workload|engine|metric" -> per run
// Runs f() until it completes without the machine sleeping (at most 3 tries); the results an
// interrupted attempt recorded are dropped.
template <typename F> void awake(const char* what, F f) {
    for (int attempt = 0;; ++attempt) {
        const auto saved = g_results;
        const double s0 = sleptSeconds();
        f();
        if (sleptSeconds() - s0 < 1.0) return;
        g_results = saved;
        g_sleep_retries++;
        std::printf("  machine slept during %s: discarding and running it again\n", what);
        std::fflush(stdout);
        if (attempt == 2) {
            errno = 0;
            bpt::fatal("the machine kept sleeping; keep the lid open and run again");
        }
    }
}

double since(Clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

// ------------------------------------------------------------------------------------------
// OS-measured bytes written by this process, files, hardware
// ------------------------------------------------------------------------------------------
uint64_t osBytesWritten() {
#if defined(__APPLE__)
    rusage_info_v2 ri{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) != 0) {
        errno = 0;
        bpt::fatal("proc_pid_rusage failed");
    }
    return ri.ri_diskio_byteswritten;
#else
    std::ifstream in("/proc/self/io");
    std::string k;
    uint64_t v = 0;
    while (in >> k >> v)
        if (k == "write_bytes:") return v;
    errno = 0;
    bpt::fatal("/proc/self/io has no write_bytes");
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

std::string hardware() {
    struct utsname u;
    uname(&u);
    std::ostringstream s;
#if defined(__APPLE__)
    const std::string cpu = shell("sysctl -n machdep.cpu.brand_string");
    const std::string mem = shell("sysctl -n hw.memsize");
    const std::string disk = shell("system_profiler SPNVMeDataType 2>/dev/null | awk -F': ' '/Model:/ {print $2; exit}'");
    const std::string fs = shell("diskutil info / | awk -F: '/File System Personality/ {gsub(/^ +/,\"\",$2); print $2}'");
    const std::string os = "macOS " + shell("sw_vers -productVersion");
#else
    const std::string cpu = shell("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2");
    const std::string mem = shell("awk '/MemTotal/ {print $2 * 1024}' /proc/meminfo");
    const std::string disk = shell("lsblk -dno MODEL | head -1");
    const std::string fs = shell("df -T /tmp | awk 'NR==2 {print $2}'");
    const std::string os = std::string(u.sysname) + " " + u.release;
#endif
    s << cpu << ", " << (mem.empty() ? 0.0 : std::stod(mem) / 1e9) << " GB RAM, disk " << disk
      << " (" << fs << "), " << os << " (" << u.machine << "), compiler " << __VERSION__
      << ", SQLite " << sqlite3_libversion();
    return s.str();
}

// ------------------------------------------------------------------------------------------
// Documents: {"id":k,"profile":{"score":0,"name":"..."},"pad":"..."} padded to exactly `size`
// ------------------------------------------------------------------------------------------
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
int64_t scoreOf(const std::string& doc) {
    return doc::parse(doc).find("profile")->find("score")->asInt();
}
// The changed field after one update, as JSON text ("score":<value>): the bytes that differ.
constexpr double kChangedFieldBytes = 11; // e.g. "score":123

// ------------------------------------------------------------------------------------------
// BenchStore interface and the three adapters
// ------------------------------------------------------------------------------------------
struct BenchStore {
    virtual ~BenchStore() = default;
    virtual bool put(const std::string& k, const std::string& doc) = 0;
    virtual bool update_field(const std::string& k) = 0; // profile.score + 1
    virtual std::optional<std::string> get(const std::string& k) = 0;
    virtual bool erase(const std::string& k) = 0;
    virtual size_t scan_100(const std::string& start) = 0; // documents returned
    virtual void checkpoint() = 0;                         // flush background work
    virtual uint64_t gcBytes() const { return 0; }
    virtual uint64_t fullSyncs() const = 0; // F_FULLFSYNC (fdatasync on Linux) calls so far
    virtual void settleGc() {}
};

enum Engine { kIndexed = 0, kSqlite = 1, kMongo = 2, kNumEngines = 3 };
const char* kEngineName[kNumEngines] = {"IndexedStore", "SQLite", "MongoLite"};

// ---- IndexedStore ----
class IndexedBench : public BenchStore {
public:
    explicit IndexedBench(const std::string& dir) {
        bpt::IndexedStore::Options o;
        o.cache_frames = kCacheFrames;
        o.vlog_opts = vlog::VLogWriter::Options(64 * 1024 * 1024, 0);
        o.gc_enabled = true;
        st_ = bpt::IndexedStore::open(dir, o);
    }
    ~IndexedBench() override {
        if (st_) st_->close();
    }
    bool put(const std::string& k, const std::string& d) override {
        st_->put(k, d);
        return true;
    }
    bool update_field(const std::string& k) override {
        try {
            st_->update(k, {doc::inc("profile.score", 1)});
            return true;
        } catch (const doc::NotFoundError&) {
            return false;
        }
    }
    std::optional<std::string> get(const std::string& k) override { return st_->get(k); }
    bool erase(const std::string& k) override { return st_->erase(k).existed; }
    size_t scan_100(const std::string& start) override {
        bpt::ReadGuard g = st_->pin();
        size_t n = 0;
        for (const auto& [k, ref] : st_->tree().scanFrom(start, 100))
            if (st_->get(k)) n++;
        return n;
    }
    void checkpoint() override { st_->checkpoint(); }
    uint64_t gcBytes() const override { return st_->gcStats().relocated_bytes; }
    uint64_t fullSyncs() const override { return st_->vlogFsyncs() + st_->pager().stats().fsyncs; }
    // GC passes (with the checkpoints they need) until nothing is left to collect.
    void settleGc() override {
        st_->stopGc();
        for (int i = 0; i < 100000; ++i) {
            bpt::GcReport r = st_->runGcPass();
            if (r.action == bpt::GcReport::Action::None && !st_->gcPending()) break;
            st_->checkpoint();
        }
    }

private:
    std::unique_ptr<bpt::IndexedStore> st_;
};

// ---- SQLite ----
// The system libsqlite3 on macOS turns PRAGMA fullfsync=ON into F_BARRIERFSYNC (a write barrier,
// about 0.35 ms here) instead of F_FULLFSYNC (3.8 ms), so its acked writes would not be flushed
// like the other two engines. This VFS shim wraps the default VFS: every call is delegated, and
// xSync additionally issues F_FULLFSYNC (fdatasync on Linux) on the shim's own descriptor of the
// same file. The pragmas stay as specified.
struct ShimFile {
    sqlite3_file base;
    sqlite3_file* real; // the default VFS's file, stored right after this struct
    int fd;             // our own read-only descriptor of the same file (-1 for temp files)
};
sqlite3_vfs* g_real_vfs = nullptr;
std::atomic<uint64_t> g_shim_fullfsyncs{0}; // F_FULLFSYNC calls the shim issued
sqlite3_vfs g_shim_vfs;
sqlite3_io_methods g_shim_io;

#define REAL(f) (reinterpret_cast<ShimFile*>(f)->real)
int shimClose(sqlite3_file* f) {
    ShimFile* s = reinterpret_cast<ShimFile*>(f);
    int rc = s->real->pMethods->xClose(s->real);
    if (s->fd >= 0 && ::close(s->fd) != 0) rc = SQLITE_IOERR_CLOSE;
    s->fd = -1;
    return rc;
}
int shimRead(sqlite3_file* f, void* b, int n, sqlite3_int64 o) { return REAL(f)->pMethods->xRead(REAL(f), b, n, o); }
int shimWrite(sqlite3_file* f, const void* b, int n, sqlite3_int64 o) { return REAL(f)->pMethods->xWrite(REAL(f), b, n, o); }
int shimTruncate(sqlite3_file* f, sqlite3_int64 n) { return REAL(f)->pMethods->xTruncate(REAL(f), n); }
int shimSync(sqlite3_file* f, int flags) {
    ShimFile* s = reinterpret_cast<ShimFile*>(f);
    int rc = s->real->pMethods->xSync(s->real, flags);
    if (rc != SQLITE_OK || s->fd < 0) return rc;
#if defined(__APPLE__)
    if (::fcntl(s->fd, F_FULLFSYNC) != 0) return SQLITE_IOERR_FSYNC;
#else
    if (::fdatasync(s->fd) != 0) return SQLITE_IOERR_FSYNC;
#endif
    g_shim_fullfsyncs++;
    return SQLITE_OK;
}
int shimFileSize(sqlite3_file* f, sqlite3_int64* n) { return REAL(f)->pMethods->xFileSize(REAL(f), n); }
int shimLock(sqlite3_file* f, int l) { return REAL(f)->pMethods->xLock(REAL(f), l); }
int shimUnlock(sqlite3_file* f, int l) { return REAL(f)->pMethods->xUnlock(REAL(f), l); }
int shimCheckReserved(sqlite3_file* f, int* r) { return REAL(f)->pMethods->xCheckReservedLock(REAL(f), r); }
int shimFileControl(sqlite3_file* f, int op, void* a) { return REAL(f)->pMethods->xFileControl(REAL(f), op, a); }
int shimSectorSize(sqlite3_file* f) { return REAL(f)->pMethods->xSectorSize(REAL(f)); }
int shimDevChar(sqlite3_file* f) { return REAL(f)->pMethods->xDeviceCharacteristics(REAL(f)); }
int shimShmMap(sqlite3_file* f, int pg, int sz, int ext, void volatile** pp) { return REAL(f)->pMethods->xShmMap(REAL(f), pg, sz, ext, pp); }
int shimShmLock(sqlite3_file* f, int o, int n, int fl) { return REAL(f)->pMethods->xShmLock(REAL(f), o, n, fl); }
void shimShmBarrier(sqlite3_file* f) { REAL(f)->pMethods->xShmBarrier(REAL(f)); }
int shimShmUnmap(sqlite3_file* f, int del) { return REAL(f)->pMethods->xShmUnmap(REAL(f), del); }
int shimFetch(sqlite3_file* f, sqlite3_int64 o, int n, void** pp) { return REAL(f)->pMethods->xFetch(REAL(f), o, n, pp); }
int shimUnfetch(sqlite3_file* f, sqlite3_int64 o, void* p) { return REAL(f)->pMethods->xUnfetch(REAL(f), o, p); }
#undef REAL

int shimOpen(sqlite3_vfs*, sqlite3_filename name, sqlite3_file* f, int flags, int* out_flags) {
    ShimFile* s = reinterpret_cast<ShimFile*>(f);
    s->real = reinterpret_cast<sqlite3_file*>(s + 1);
    s->fd = -1;
    int rc = g_real_vfs->xOpen(g_real_vfs, name, s->real, flags, out_flags);
    if (rc != SQLITE_OK) {
        s->base.pMethods = nullptr;
        return rc;
    }
    if (name) {
        s->fd = ::open(name, O_RDONLY);
        if (s->fd < 0) {
            s->real->pMethods->xClose(s->real);
            s->base.pMethods = nullptr;
            return SQLITE_CANTOPEN;
        }
    }
    g_shim_io.iVersion = s->real->pMethods->iVersion;
    s->base.pMethods = &g_shim_io;
    return SQLITE_OK;
}

void registerShimVfs() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_real_vfs = sqlite3_vfs_find(nullptr);
        g_shim_vfs = *g_real_vfs;
        g_shim_vfs.pNext = nullptr;
        g_shim_vfs.zName = "fullfsync_shim";
        g_shim_vfs.szOsFile = static_cast<int>(sizeof(ShimFile)) + g_real_vfs->szOsFile;
        g_shim_vfs.xOpen = shimOpen;
        g_shim_io = sqlite3_io_methods{3,           shimClose,     shimRead,       shimWrite,
                                       shimTruncate, shimSync,     shimFileSize,   shimLock,
                                       shimUnlock,  shimCheckReserved, shimFileControl,
                                       shimSectorSize, shimDevChar, shimShmMap,   shimShmLock,
                                       shimShmBarrier, shimShmUnmap, shimFetch,   shimUnfetch};
        if (sqlite3_vfs_register(&g_shim_vfs, 0) != SQLITE_OK) {
            errno = 0;
            bpt::fatal("sqlite3_vfs_register failed");
        }
    });
}

std::atomic<uint64_t> g_sqlite_busy{0}; // statements retried after SQLITE_BUSY

void sqlOk(int rc, sqlite3* db, const char* what) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        errno = 0;
        bpt::fatal(std::string("sqlite: ") + what + ": " + (db ? sqlite3_errmsg(db) : "?"));
    }
}

// sqlite3_step that retries after SQLITE_BUSY: with 16 writers one connection can wait longer
// than busy_timeout (10 s) because SQLite's busy handler is not fair. The operation is acked
// when it commits; its latency includes the waiting. Retries are counted and reported.
int stepRetry(sqlite3* db, sqlite3_stmt* st, const char* what) {
    for (;;) {
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            sqlite3_reset(st); // bindings are kept
            g_sqlite_busy++;
            continue;
        }
        sqlOk(rc, db, what);
        return rc;
    }
}

class SqliteBench : public BenchStore {
    struct Conn {
        sqlite3* db = nullptr;
        sqlite3_stmt *put = nullptr, *upd = nullptr, *get = nullptr, *del = nullptr,
                     *scan = nullptr;
    };

public:
    explicit SqliteBench(const std::string& dir) : path_(dir + "/docs.db") {
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) bpt::fatal("mkdir " + dir);
        (void)conn(); // creates the schema
    }
    ~SqliteBench() override {
        for (auto& [id, c] : conns_) {
            for (sqlite3_stmt* s : {c.put, c.upd, c.get, c.del, c.scan}) sqlite3_finalize(s);
            sqlOk(sqlite3_close(c.db), nullptr, "close");
        }
    }
    bool put(const std::string& k, const std::string& d) override {
        Conn& c = conn();
        bind2(c.put, k, d);
        stepRetry(c.db, c.put, "insert");
        sqlite3_reset(c.put);
        return true;
    }
    bool update_field(const std::string& k) override {
        Conn& c = conn();
        sqlite3_bind_text(c.upd, 1, k.data(), static_cast<int>(k.size()), SQLITE_TRANSIENT);
        stepRetry(c.db, c.upd, "update");
        const bool changed = sqlite3_changes(c.db) == 1;
        sqlite3_reset(c.upd);
        return changed;
    }
    std::optional<std::string> get(const std::string& k) override {
        Conn& c = conn();
        sqlite3_bind_text(c.get, 1, k.data(), static_cast<int>(k.size()), SQLITE_TRANSIENT);
        std::optional<std::string> out;
        int rc = stepRetry(c.db, c.get, "select");
        if (rc == SQLITE_ROW)
            out.emplace(reinterpret_cast<const char*>(sqlite3_column_text(c.get, 0)),
                        static_cast<size_t>(sqlite3_column_bytes(c.get, 0)));
        sqlite3_reset(c.get);
        return out;
    }
    bool erase(const std::string& k) override {
        Conn& c = conn();
        sqlite3_bind_text(c.del, 1, k.data(), static_cast<int>(k.size()), SQLITE_TRANSIENT);
        stepRetry(c.db, c.del, "delete");
        const bool changed = sqlite3_changes(c.db) == 1;
        sqlite3_reset(c.del);
        return changed;
    }
    size_t scan_100(const std::string& start) override {
        Conn& c = conn();
        sqlite3_bind_text(c.scan, 1, start.data(), static_cast<int>(start.size()), SQLITE_TRANSIENT);
        size_t n = 0;
        int rc;
        while ((rc = stepRetry(c.db, c.scan, "scan")) == SQLITE_ROW) {
            volatile size_t len = static_cast<size_t>(sqlite3_column_bytes(c.scan, 1));
            (void)len;
            n++;
        }
        sqlite3_reset(c.scan);
        return n;
    }
    void checkpoint() override { exec(conn().db, "PRAGMA wal_checkpoint(TRUNCATE)"); }
    uint64_t fullSyncs() const override { return g_shim_fullfsyncs.load(); }

private:
    static void exec(sqlite3* db, const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string m = err ? err : "?";
            sqlite3_free(err);
            errno = 0;
            bpt::fatal(std::string("sqlite exec ") + sql + ": " + m);
        }
    }
    static void bind2(sqlite3_stmt* s, const std::string& a, const std::string& b) {
        sqlite3_bind_text(s, 1, a.data(), static_cast<int>(a.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, b.data(), static_cast<int>(b.size()), SQLITE_TRANSIENT);
    }
    // One connection per thread (created on first use).
    Conn& conn() {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = conns_.find(std::this_thread::get_id());
        if (it != conns_.end()) return it->second;
        Conn c;
        registerShimVfs();
        sqlOk(sqlite3_open_v2(path_.c_str(), &c.db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                              "fullfsync_shim"),
              c.db, "open");
        sqlite3_busy_timeout(c.db, 10000);
        for (const char* p : {"PRAGMA journal_mode = WAL", "PRAGMA synchronous = FULL",
                              "PRAGMA fullfsync = ON", "PRAGMA checkpoint_fullfsync = ON",
                              "PRAGMA cache_size = -4096"})
            exec(c.db, p);
        exec(c.db, "CREATE TABLE IF NOT EXISTS docs(key TEXT PRIMARY KEY, doc TEXT)");
        auto prep = [&](const char* sql, sqlite3_stmt** s) {
            sqlOk(sqlite3_prepare_v2(c.db, sql, -1, s, nullptr), c.db, sql);
        };
        prep("INSERT OR REPLACE INTO docs(key, doc) VALUES(?, ?)", &c.put);
        prep("UPDATE docs SET doc = json_set(doc, '$.profile.score', "
             "json_extract(doc, '$.profile.score') + 1) WHERE key = ?",
             &c.upd);
        prep("SELECT doc FROM docs WHERE key = ?", &c.get);
        prep("DELETE FROM docs WHERE key = ?", &c.del);
        prep("SELECT key, doc FROM docs WHERE key >= ? ORDER BY key LIMIT 100", &c.scan);
        return conns_.emplace(std::this_thread::get_id(), c).first->second;
    }

    std::string path_;
    std::mutex mu_;
    std::map<std::thread::id, Conn> conns_;
};

// ---- MongoLite (legacy/, unmodified) ----
// Mutes the legacy engine's console messages around construction, checkpoint and destruction.
struct Quiet {
    std::ostringstream sink; // declared first: constructed before old redirects to it
    std::streambuf* old;
    Quiet() : old(std::cout.rdbuf(sink.rdbuf())) {}
    ~Quiet() { std::cout.rdbuf(old); }
};

std::atomic<uint64_t> g_mongo_syncs{0};
void syncFd(int fd, const char* what) {
#if defined(__APPLE__)
    if (::fcntl(fd, F_FULLFSYNC) != 0) bpt::fatal(std::string("F_FULLFSYNC failed: ") + what);
#else
    if (::fdatasync(fd) != 0) bpt::fatal(std::string("fdatasync failed: ") + what);
#endif
    g_mongo_syncs++;
}

class MongoBench : public BenchStore {
public:
    explicit MongoBench(const std::string& dir) {
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) bpt::fatal("mkdir " + dir);
        const std::string path = dir + "/mongo.db";
        {
            Quiet q;
            db_.reset(new MongoLite(path, kCacheFrames));
        }
        // Adapter durability: our own descriptors on the WAL and data files, synced after every
        // operation (WAL) and at checkpoint (data file). legacy/ itself never syncs.
        wal_fd_ = ::open((path + ".wal").c_str(), O_RDONLY);
        data_fd_ = ::open(path.c_str(), O_RDONLY);
        if (wal_fd_ < 0 || data_fd_ < 0) bpt::fatal("open MongoLite files for syncing");
    }
    ~MongoBench() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            Quiet q;
            db_.reset(); // the destructor checkpoints
            syncFd(data_fd_, "MongoLite data at close");
            syncFd(wal_fd_, "MongoLite WAL at close");
        }
        if (::close(wal_fd_) != 0 || ::close(data_fd_) != 0) bpt::fatal("close MongoLite fds");
    }
    // An existing key is deleted first (MongoLite rejects duplicate keys).
    bool put(const std::string& k, const std::string& d) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (exists(k)) db_->deleteOne(k);
        const bool ok = db_->insertOne(k, d);
        syncFd(wal_fd_, "MongoLite WAL");
        return ok;
    }
    // read -> delete -> insert, then one sync of the WAL (both records).
    bool update_field(const std::string& k) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!exists(k)) return false;
        doc::Value v = doc::parse(db_->findOne(k));
        doc::applyOps(v, {doc::inc("profile.score", 1)});
        db_->deleteOne(k);
        const bool ok = db_->insertOne(k, doc::serialize(v));
        syncFd(wal_fd_, "MongoLite WAL");
        return ok;
    }
    std::optional<std::string> get(const std::string& k) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!exists(k)) return std::nullopt;
        return db_->findOne(k);
    }
    bool erase(const std::string& k) override {
        std::lock_guard<std::mutex> lk(mu_);
        const bool ok = db_->deleteOne(k);
        syncFd(wal_fd_, "MongoLite WAL");
        return ok;
    }
    size_t scan_100(const std::string& start) override {
        std::lock_guard<std::mutex> lk(mu_);
        return db_->findRange(start, "", 100).size();
    }
    uint64_t fullSyncs() const override { return g_mongo_syncs.load(); }
    void checkpoint() override {
        std::lock_guard<std::mutex> lk(mu_);
        {
            Quiet q;
            db_->checkpoint();
        }
        syncFd(data_fd_, "MongoLite data at checkpoint");
        syncFd(wal_fd_, "MongoLite WAL at checkpoint");
    }

private:
    bool exists(const std::string& k) {
        return db_->findOne(k) != "null (Document not found)";
    }
    std::mutex mu_;
    std::unique_ptr<MongoLite> db_;
    int wal_fd_ = -1, data_fd_ = -1;
};

std::unique_ptr<BenchStore> openStore(Engine e, const std::string& dir) {
    switch (e) {
    case kIndexed:
        return std::unique_ptr<BenchStore>(new IndexedBench(dir));
    case kSqlite:
        return std::unique_ptr<BenchStore>(new SqliteBench(dir));
    case kMongo:
        return std::unique_ptr<BenchStore>(new MongoBench(dir));
    default:
        break;
    }
    return nullptr;
}
bool supports(Engine e, size_t doc_bytes) { return !(e == kMongo && doc_bytes > 4000); }

// ------------------------------------------------------------------------------------------
// Results: every measurement stored per run; tables print the median of the runs
// ------------------------------------------------------------------------------------------
void record(const std::string& w, Engine e, const std::string& m, double v) {
    g_results[w + "|" + kEngineName[e] + "|" + m].push_back(v);
}
std::optional<double> median(const std::string& w, Engine e, const std::string& m) {
    auto it = g_results.find(w + "|" + kEngineName[e] + "|" + m);
    if (it == g_results.end() || it->second.empty()) return std::nullopt;
    std::vector<double> v = it->second;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Lat {
    std::vector<double> us;
    void add(double x) { us.push_back(x); }
    void merge(const Lat& o) { us.insert(us.end(), o.us.begin(), o.us.end()); }
    double pct(double p) {
        if (us.empty()) return 0;
        std::sort(us.begin(), us.end());
        return us[std::min(us.size() - 1, static_cast<size_t>(p * us.size()))];
    }
};

// Runs fn(thread, i) for n ops split over `threads` threads; returns all latencies and seconds.
template <typename F> std::pair<Lat, double> runOps(int n, int threads, F fn) {
    std::vector<Lat> lat(threads);
    const auto t0 = Clock::now();
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t)
        th.emplace_back([&, t] {
            for (int i = t; i < n; i += threads) {
                const auto a = Clock::now();
                fn(t, i);
                lat[t].add(since(a));
            }
        });
    for (auto& x : th) x.join();
    const double secs = since(t0) / 1e6;
    Lat all;
    for (auto& l : lat) all.merge(l);
    return {all, secs};
}

void recordOps(const std::string& w, Engine e, Lat& l, double secs, int n) {
    record(w, e, "ops_s", n / secs);
    record(w, e, "p50", l.pct(0.5));
    record(w, e, "p99", l.pct(0.99));
    record(w, e, "p999", l.pct(0.999));
    record(w, e, "max", l.pct(1.0));
}

// OS bytes written by fn (after the engine flushed its background work), and file growth.
struct Bytes {
    uint64_t os = 0;
    int64_t files = 0;
    uint64_t syncs = 0; // full syncs during fn and the flush
    uint64_t busy = 0;  // SQLITE_BUSY retries (SQLite only)
};
template <typename F> Bytes measure(BenchStore& st, const std::string& dir, F fn) {
    const uint64_t os0 = osBytesWritten();
    const int64_t f0 = static_cast<int64_t>(dirBytes(dir));
    const uint64_t s0 = st.fullSyncs();
    const uint64_t busy0 = g_sqlite_busy.load();
    fn();
    st.checkpoint();
    Bytes b;
    b.syncs = st.fullSyncs() - s0;
    b.busy = g_sqlite_busy.load() - busy0;
    b.os = osBytesWritten() - os0;
    b.files = static_cast<int64_t>(dirBytes(dir)) - f0;
    accountBytes(b.os, "engine writes (OS counter)");
    return b;
}

// ------------------------------------------------------------------------------------------
// Workloads
// ------------------------------------------------------------------------------------------
void loadDocs(BenchStore& st, size_t size, int threads, Lat* lat = nullptr, double* secs = nullptr) {
    auto [l, s] = runOps(kDocs, threads, [&](int, int i) {
        if (!st.put(key(i), makeDoc(i, size))) {
            errno = 0;
            bpt::fatal("put failed");
        }
    });
    if (lat) *lat = l;
    if (secs) *secs = s;
}

// A: bulk load of 1 KB documents with 1 and 16 threads.
void workloadA(Engine e, const std::string& base) {
    for (int threads : {1, kThreads}) {
        const std::string dir = base + "/A" + std::to_string(threads);
        const std::string w = "A load 1 KB, " + std::to_string(threads) + (threads == 1 ? " thread" : " threads");
        auto st = openStore(e, dir);
        Lat l;
        double secs = 0;
        Bytes b = measure(*st, dir, [&] { loadDocs(*st, 1024, threads, &l, &secs); });
        recordOps(w, e, l, secs, kDocs);
        record(w, e, "bytes_op", double(b.os) / kDocs);
        record(w, e, "file_bytes_op", double(b.files) / kDocs);
        record(w, e, "waf_doc", double(b.os) / (double(kDocs) * 1024));
        record(w, e, "syncs_op", double(b.syncs) / kDocs);
        if (e == kSqlite) record(w, e, "busy_retries", double(b.busy));
        st.reset();
        std::filesystem::remove_all(dir);
    }
}

// B, C, D for one document size.
void workloadBCD(Engine e, const std::string& base, size_t size, uint32_t seed) {
    const std::string dir = base + "/B" + std::to_string(size);
    const std::string sz = std::to_string(size / 1024) + " KB";
    auto st = openStore(e, dir);
    loadDocs(*st, size, kThreads);
    st->checkpoint();
    for (int threads : {1, kThreads}) {
        const std::string w = "B update " + sz + ", " + std::to_string(threads) + (threads == 1 ? " thread" : " threads");
        std::vector<int> keys(kUpdates);
        std::mt19937 rng(seed + threads);
        for (int& k : keys) k = static_cast<int>(rng() % kDocs);
        Lat l;
        double secs = 0;
        const uint64_t gc0 = st->gcBytes();
        Bytes b = measure(*st, dir, [&] {
            auto r = runOps(kUpdates, threads, [&](int, int i) {
                if (!st->update_field(key(keys[i]))) {
                    errno = 0;
                    bpt::fatal("update failed");
                }
            });
            l = r.first;
            secs = r.second;
        });
        recordOps(w, e, l, secs, kUpdates);
        record(w, e, "bytes_op", double(b.os) / kUpdates);
        record(w, e, "file_bytes_op", double(b.files) / kUpdates);
        record(w, e, "waf_field", double(b.os) / (double(kUpdates) * kChangedFieldBytes));
        record(w, e, "waf_doc", double(b.os) / (double(kUpdates) * size));
        if (e == kIndexed) record(w, e, "gc_bytes_op", double(st->gcBytes() - gc0) / kUpdates);
        record(w, e, "syncs_op", double(b.syncs) / kUpdates);
        if (e == kSqlite) record(w, e, "busy_retries", double(b.busy));
    }
    // D: footprint after B.
    const double live = double(kDocs) * size;
    const std::string wd = "D footprint " + sz;
    record(wd, e, "disk_mb", dirBytes(dir) / 1e6);
    record(wd, e, "space_amp", dirBytes(dir) / live);
    if (e == kIndexed) {
        const uint64_t gc0 = st->gcBytes();
        st->settleGc();
        record(wd, e, "disk_mb_after_gc", dirBytes(dir) / 1e6);
        record(wd, e, "space_amp_after_gc", dirBytes(dir) / live);
        record(wd, e, "gc_mb_settle", (st->gcBytes() - gc0) / 1e6);
    }
    st.reset();
    // C: reads after reopening the 1 KB dataset.
    if (size == 1024) {
        st = openStore(e, dir);
        std::mt19937 rng(seed + 99);
        std::vector<int> keys(kGets);
        for (int& k : keys) k = static_cast<int>(rng() % kDocs);
        auto g = runOps(kGets, 1, [&](int, int i) {
            if (!st->get(key(keys[i]))) {
                errno = 0;
                bpt::fatal("get missed");
            }
        });
        recordOps("C point get (1 KB, 1 thread)", e, g.first, g.second, kGets);
        std::vector<int> starts(kScans);
        for (int& k : starts) k = static_cast<int>(rng() % (kDocs - 100));
        auto s = runOps(kScans, 1, [&](int, int i) {
            if (st->scan_100(key(starts[i])) != 100) {
                errno = 0;
                bpt::fatal("scan returned fewer than 100 documents");
            }
        });
        recordOps("C range scan 100 (1 KB, 1 thread)", e, s.first, s.second, kScans);
        st.reset();
    }
    std::filesystem::remove_all(dir);
}

// E: kill -9 during B (1 KB, 16 threads); every acked update must be present after reopening.
struct CrashResult {
    uint64_t acked = 0, lost = 0, keys_checked = 0, in_flight = 0;
    bool reopened = true;
    std::string note;
};
struct Ack {
    uint32_t key;
    uint32_t kind; // 0 intent, 1 ack
};

CrashResult workloadE(Engine e, const std::string& base, uint32_t seed) {
    const std::string dir = base + "/E";
    CrashResult cr;
    {
        auto st = openStore(e, dir);
        loadDocs(*st, 1024, kThreads);
        st->checkpoint();
    }
    accountBytes(dirBytes(dir), "crash dataset");
    const uint64_t kill_at = static_cast<uint64_t>(kUpdates / 4);
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    std::cout.flush();
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        childExpectsAbort();
        CHECK(close(pfd[0]) == 0);
        auto st = openStore(e, dir);
        std::vector<std::thread> th;
        for (int t = 0; t < kThreads; ++t)
            th.emplace_back([&, t] {
                std::mt19937 rng(seed + 1000 + t);
                for (int i = 0; i < kUpdates; ++i) {
                    Ack in{static_cast<uint32_t>(rng() % kDocs), 0};
                    CHECK(write(pfd[1], &in, sizeof(in)) == static_cast<ssize_t>(sizeof(in)));
                    CHECK(st->update_field(key(static_cast<int>(in.key))));
                    Ack ack{in.key, 1};
                    CHECK(write(pfd[1], &ack, sizeof(ack)) == static_cast<ssize_t>(sizeof(ack)));
                }
            });
        for (auto& x : th) x.join();
        while (true) pause();
    }
    CHECK(close(pfd[1]) == 0);
    std::map<uint32_t, int64_t> acked, inflight;
    Ack a;
    uint64_t acks = 0;
    auto readAck = [&](Ack* r) {
        while (true) {
            ssize_t n = read(pfd[0], r, sizeof(*r));
            if (n == static_cast<ssize_t>(sizeof(*r))) return true;
            if (n == 0) return false;
            CHECK(n < 0 && errno == EINTR);
        }
    };
    auto consume = [&](const Ack& r) {
        if (r.kind == 0) {
            inflight[r.key]++;
        } else {
            inflight[r.key]--;
            acked[r.key]++;
            acks++;
        }
    };
    while (acks < kill_at && readAck(&a)) consume(a);
    CHECK(kill(pid, SIGKILL) == 0);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status));
    while (readAck(&a)) consume(a);
    CHECK(close(pfd[0]) == 0);
    cr.acked = acks;
    accountBytes(dirBytes(dir), "crash child writes (file sizes)");
    // Reopen and check every key: score in [acked, acked + in flight] means nothing acked is lost.
    try {
        auto st = openStore(e, dir);
        for (int k = 0; k < kDocs; ++k) {
            const auto ak = acked.count(k) ? acked[k] : 0;
            const auto fl = inflight.count(k) ? inflight[k] : 0;
            cr.in_flight += static_cast<uint64_t>(fl);
            std::optional<std::string> d = st->get(key(k));
            const int64_t s = d ? scoreOf(*d) : -1;
            if (s < ak) cr.lost += static_cast<uint64_t>(ak - std::max<int64_t>(s, 0));
            cr.keys_checked++;
        }
    } catch (const std::exception& ex) {
        cr.reopened = false;
        cr.note = ex.what();
    }
    std::filesystem::remove_all(dir);
    return cr;
}

// ------------------------------------------------------------------------------------------
// Report
// ------------------------------------------------------------------------------------------
std::string fmt(double v, int prec) {
    char b[64];
    if (prec == 0) {
        std::snprintf(b, sizeof(b), "%.0f", v);
        std::string s = b, out;
        int n = 0;
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            if (n && n % 3 == 0 && *it != '-') out.insert(out.begin(), ',');
            out.insert(out.begin(), *it);
            n++;
        }
        return out;
    }
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

// One row: a metric for the three engines, the winner marked (higher or lower is better).
void row(const std::string& w, const std::string& label, const std::string& m, bool higher,
         int prec, double scale = 1.0) {
    std::optional<double> v[kNumEngines];
    int best = -1;
    for (int e = 0; e < kNumEngines; ++e) {
        v[e] = median(w, static_cast<Engine>(e), m);
        if (!v[e]) continue;
        if (best < 0 || (higher ? *v[e] > *v[best] : *v[e] < *v[best])) best = e;
    }
    if (best < 0) return;
    int n = 0;
    for (int e = 0; e < kNumEngines; ++e) n += v[e] ? 1 : 0;
    const bool compare = n > 1; // a row only one engine has (GC bytes, busy retries): no winner
    std::printf("| %s |", label.c_str());
    for (int e = 0; e < kNumEngines; ++e) {
        if (!v[e])
            std::printf(" %s |", compare ? "not supported" : "-");
        else
            std::printf(" %s%s |", fmt(*v[e] * scale, prec).c_str(),
                        compare && e == best ? " **(best)**" : "");
    }
    std::printf(" %s |\n", compare ? kEngineName[best] : "n/a");
}

void opsTable(const std::string& w, bool updates) {
    std::printf("\n#### %s\n\n| Metric | IndexedStore | SQLite | MongoLite | Winner |\n|---|---:|---:|---:|---|\n", w.c_str());
    row(w, "ops/s", "ops_s", true, 0);
    row(w, "p50 latency (µs)", "p50", false, 0);
    row(w, "p99 latency (µs)", "p99", false, 0);
    row(w, "p99.9 latency (µs)", "p999", false, 0);
    row(w, "max latency (µs)", "max", false, 0);
    if (g_results.count(w + "|IndexedStore|bytes_op") || g_results.count(w + "|SQLite|bytes_op")) {
        row(w, "bytes written per op (OS)", "bytes_op", false, 0);
        row(w, "file growth per op", "file_bytes_op", false, 0);
        if (updates) row(w, "WAF vs changed field (11 B)", "waf_field", false, 1);
        row(w, "WAF vs document size", "waf_doc", false, 2);
        row(w, "full syncs per op (F_FULLFSYNC)", "syncs_op", false, 3);
        row(w, "SQLITE_BUSY retries (SQLite only)", "busy_retries", false, 0);
        if (updates) row(w, "GC bytes per op (IndexedStore only)", "gc_bytes_op", false, 0);
    }
}

} // namespace

int main() {
    std::string dir = setupRun("bench_compare");
    Cleaner cleaner{dir};
    // On a failure, remove the run's directory too (the default handler only prints its path).
    std::signal(SIGABRT, [](int) {
        std::error_code ec;
        if (g_test_dir[0]) std::filesystem::remove_all(g_test_dir, ec);
        std::_Exit(134);
    });
    const std::string hw = hardware();
    std::printf("Hardware: %s\n", hw.c_str());
    std::printf("Scale: %d docs per dataset, %d updates, %d gets, %d scans, %d threads, %d run(s)\n",
                kDocs, kUpdates, kGets, kScans, kThreads, kRuns);
    // Planned bytes, from the pilot: one run of all workloads and engines at 5,000 documents and
    // 2,000 updates per dataset wrote 1.27 GB (OS counter); writes scale with the document and
    // update counts (reads write nothing).
    const double planned = kRuns * 1.27e9 * (double(kDocs) / 5000.0);
    std::printf("Planned: about %.2f GB written in total (cap %.1f GB); free disk checked >= 10 GB.\n",
                planned / 1e9, kWriteBudgetBytes / 1e9);
    std::fflush(stdout);
    if (planned > double(kWriteBudgetBytes)) {
        std::printf("STOP: planned bytes exceed the cap.\n");
        return 2;
    }

    std::vector<CrashResult> crashes[kNumEngines];
    for (int run = 0; run < kRuns; ++run) {
        for (int e = 0; e < kNumEngines; ++e) {
            const Engine en = static_cast<Engine>(e);
            const std::string base = dir + "/r" + std::to_string(run) + "_" + std::to_string(e);
            std::filesystem::create_directories(base);
            std::printf("run %d/%d: %s ...\n", run + 1, kRuns, kEngineName[e]);
            std::fflush(stdout);
            awake("workload A", [&] { workloadA(en, base); });
            for (size_t size : kSizes)
                if (supports(en, size))
                    awake("workloads B-D", [&] { workloadBCD(en, base, size, 7 + run); });
            CrashResult cr;
            awake("workload E", [&] { cr = workloadE(en, base, 11 + run); });
            crashes[e].push_back(cr);
            std::filesystem::remove_all(base);
        }
    }

    std::printf("\n## Storage Engine Comparison\n\nMedian of %d runs. %s\n", kRuns, hw.c_str());
    std::printf("\n### A. Bulk load (%d documents of 1 KB)\n", kDocs);
    opsTable("A load 1 KB, 1 thread", false);
    opsTable("A load 1 KB, 16 threads", false);
    std::printf("\n### B. Small updates (%d updates of profile.score + 1 on %d documents)\n", kUpdates, kDocs);
    for (size_t size : kSizes)
        for (const char* t : {"1 thread", "16 threads"})
            opsTable("B update " + std::to_string(size / 1024) + " KB, " + t, true);
    std::printf("\n### C. Reads after reopening (1 KB dataset)\n");
    opsTable("C point get (1 KB, 1 thread)", false);
    opsTable("C range scan 100 (1 KB, 1 thread)", false);
    std::printf("\n### D. Disk footprint after B (live data = documents x document size)\n\n"
                "| Dataset | Metric | IndexedStore | SQLite | MongoLite | Winner |\n|---|---|---:|---:|---:|---|\n");
    for (size_t size : kSizes) {
        const std::string w = "D footprint " + std::to_string(size / 1024) + " KB";
        auto r = [&](const char* label, const char* m, int prec) {
            std::optional<double> v[kNumEngines];
            int best = -1;
            for (int e = 0; e < kNumEngines; ++e) {
                v[e] = median(w, static_cast<Engine>(e), m);
                if (v[e] && (best < 0 || *v[e] < *v[best])) best = e;
            }
            if (best < 0) return;
            int n = 0;
            for (int e = 0; e < kNumEngines; ++e) n += v[e] ? 1 : 0;
            std::printf("| %zu KB | %s |", size / 1024, label);
            for (int e = 0; e < kNumEngines; ++e)
                std::printf(" %s%s |",
                            v[e] ? fmt(*v[e], prec).c_str()
                                 : (n > 1 ? "not supported" : "-"),
                            n > 1 && v[e] && e == best ? " **(best)**" : "");
            std::printf(" %s |\n", n > 1 ? kEngineName[best] : "n/a");
        };
        r("disk MB", "disk_mb", 1);
        r("space amplification", "space_amp", 2);
        r("disk MB after GC settles (IndexedStore)", "disk_mb_after_gc", 1);
        r("space amplification after GC settles", "space_amp_after_gc", 2);
        r("GC MB written while settling", "gc_mb_settle", 1);
    }
    std::printf("\n### E. kill -9 during 16-thread 1 KB updates (killed after %d acks)\n\n"
                "| Engine | Run | Acked updates | Acked updates lost | In flight at the kill | Reopened |\n|---|---:|---:|---:|---:|---|\n",
                kUpdates / 4);
    for (int e = 0; e < kNumEngines; ++e)
        for (size_t i = 0; i < crashes[e].size(); ++i) {
            const CrashResult& c = crashes[e][i];
            std::printf("| %s | %zu | %s | %s of %s | %s | %s |\n", kEngineName[e], i + 1,
                        fmt(double(c.acked), 0).c_str(), fmt(double(c.lost), 0).c_str(),
                        fmt(double(c.acked), 0).c_str(), fmt(double(c.in_flight), 0).c_str(),
                        c.reopened ? "yes" : ("no: " + c.note).c_str());
        }
    std::printf("\nWorkloads repeated because the machine slept: %d\n", g_sleep_retries);
    std::printf("\n[Budget] bytes written (OS counter, this process): %llu of %llu\n",
                (unsigned long long)bytesWritten(), (unsigned long long)kWriteBudgetBytes);
    return 0;
}
