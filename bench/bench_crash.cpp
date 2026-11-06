// Crash, soak, disk-full and large-document checks (Phase 7, R3-R5). Not part of any test mode.
// Run through scripts/reality/run_safe.sh (machine-safety rules) with an explicit --max-write.
//
// Modes:
//   soak   --dir D [--minutes 60] [--kill-every-s 300] [--writers 8] [--readers 4] [--rate 400]
//          [--keys 150000] [--preload 100000] [--seg-mb 16]
//          The parent preloads, then repeats: fork a child that opens the store (GC on, automatic
//          checkpoints) and runs writers (inserts, updates, deletes; each key belongs to one
//          writer) and readers; kill -9 the child after --kill-every-s; reopen and verify.
//   fill   --dir D --scenario vlog|index --filler F [--leave-mb 8] [--free-check-path P]
//          For a store inside a small disk image: preload, fill the volume with file F until
//          --leave-mb are free, fork a child that writes until it dies, try a reopen while the
//          volume is still full (in a child), delete F, reopen and verify.
//   large  --dir D     documents of 64 KB, 1 MB, 4 MB, 16,000,000 B and the maximum size; put,
//          get, update, scan, checkpoint, overwrite + GC, kill -9 during a put; then documents
//          just above the maximum.
//
// Verification after every reopen (verify()):
//   1. every key's document matches the parent's model: the last acked write, or the one write
//      to that key that had started but was not acked when the child died (each writer sends an
//      "intent" record before an operation and an "ack" record with the seq after it, over a
//      pipe; records are 24 bytes, so each pipe write is atomic);
//   2. verifyCountersForTest() (GC counters == a recount from the index);
//   3. index == full log replay: the index (every key's seq) is compared with a winner per key
//      computed from every entry of every vLog segment (read-only scans), by the rule the
//      engine's rebuild uses (highest seq; on an equal seq consolidated > relocated > original
//      Full > Delta). Where both name the same seq but different locations, both must be the
//      same version: either the two entries are equal (a GC copy of a Full), or the winner is a
//      Full whose payload is the document get() returns through the index (a GC consolidation of
//      a Delta chain that a crash left unapplied: the index still names the Delta, which reads
//      as the same document). The replay runs while the store is open, so get() can be used.
//
// Write accounting: the parent's own OS counter plus every child's, read with proc_pid_rusage
// every 250 ms and right before each kill (a child that dies on its own may have written up to
// 250 ms more than counted). The total is checked against --max-write.
#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexedStore.hpp"
#include "reality_common.hpp"

#include <signal.h>
#include <sys/wait.h>

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace {
using namespace reality;

// ---- Args ----
struct Args {
    std::string mode, dir, scenario, filler, free_check_path;
    uint64_t max_write = 0;
    uint64_t min_free = 50ULL * 1000 * 1000 * 1000;
    uint64_t max_rss = 4096ULL * 1024 * 1024;
    double minutes = 60;
    int kill_every_s = 300, writers = 8, readers = 4;
    double rate = 400;
    uint32_t keys = 150000, preload = 100000, seg_mb = 16;
    uint64_t leave_mb = 8;
};

[[noreturn]] void usage(const std::string& why) {
    std::fprintf(stderr, "bench_crash: %s\nusage: bench_crash soak|fill|large --dir D ...\n",
                 why.c_str());
    std::exit(2);
}

Args parse(int argc, char** argv) {
    Args a;
    if (argc < 2) usage("no mode");
    a.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string f = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) usage("missing value for " + f);
            return argv[++i];
        };
        if (f == "--dir") a.dir = val();
        else if (f == "--max-write") a.max_write = std::stoull(val());
        else if (f == "--minutes") a.minutes = std::stod(val());
        else if (f == "--kill-every-s") a.kill_every_s = std::stoi(val());
        else if (f == "--writers") a.writers = std::stoi(val());
        else if (f == "--readers") a.readers = std::stoi(val());
        else if (f == "--rate") a.rate = std::stod(val());
        else if (f == "--keys") a.keys = uint32_t(std::stoul(val()));
        else if (f == "--preload") a.preload = uint32_t(std::stoul(val()));
        else if (f == "--seg-mb") a.seg_mb = uint32_t(std::stoul(val()));
        else if (f == "--scenario") a.scenario = val();
        else if (f == "--filler") a.filler = val();
        else if (f == "--leave-mb") a.leave_mb = std::stoull(val());
        else if (f == "--free-check-path") a.free_check_path = val();
        else if (f == "--min-free-gb") a.min_free = std::stoull(val()) * 1000ULL * 1000 * 1000;
        else if (f == "--max-rss-mb") a.max_rss = std::stoull(val()) * 1024ULL * 1024;
        else usage("unknown flag " + f);
    }
    if (a.max_write == 0)
        if (const char* e = std::getenv("REALITY_MAX_WRITE")) a.max_write = std::stoull(e);
    if (a.max_write == 0) usage("--max-write (or REALITY_MAX_WRITE) is required");
    if (a.dir.empty()) usage("--dir is required");
    if (a.free_check_path.empty())
        a.free_check_path = std::filesystem::path(a.dir).parent_path().string();
    if (a.writers < 1 || a.writers > 64 || a.readers < 0 || a.readers > 64)
        usage("bad --writers/--readers");
    if (a.preload > a.keys) usage("--preload above --keys");
    return a;
}

// ---- Write accounting and the monitor thread ----
std::mutex g_child_mu;         // guards the three fields below; also held around fork()
pid_t g_child = 0;             // running child, 0 if none
uint64_t g_child_last = 0;     // its last polled OS bytes written
uint64_t g_child_rss_last = 0; // its last polled resident size
uint64_t g_children_done = 0;  // bytes written by children that have exited
uint64_t g_child_rss_peak = 0; // peak resident size over all children
OsStats g_os0;
std::atomic<bool> g_stop{false};
Args g_args;

void pollChildLocked() {
    if (g_child <= 0) return;
    OsStats s;
    uint64_t res = 0;
    if (osStatsOf(g_child, &s, &res)) {
        g_child_last = std::max(g_child_last, s.written);
        g_child_rss_last = res;
        g_child_rss_peak = std::max(g_child_rss_peak, res);
    }
}
uint64_t totalWrittenLocked() {
    return (osStats().written - g_os0.written) + g_children_done + g_child_last;
}
uint64_t totalWritten() {
    std::lock_guard<std::mutex> lk(g_child_mu);
    pollChildLocked();
    return totalWrittenLocked();
}
// After the child has been reaped.
void childFinishedLocked() {
    g_children_done += g_child_last;
    g_child_last = 0;
    g_child = 0;
    g_child_rss_last = 0;
}

void monitor() {
    const auto t0 = Clock::now();
    auto last = t0 - std::chrono::seconds(60);
    while (!g_stop.load()) {
        {
            std::lock_guard<std::mutex> lk(g_child_mu);
            pollChildLocked();
            const uint64_t w = totalWrittenLocked();
            const uint64_t fr = freeBytes(g_args.free_check_path);
            const uint64_t own_rss = rssNow();
            const char* why = nullptr;
            if (w > g_args.max_write) why = "OS bytes written above --max-write";
            else if (fr < g_args.min_free) why = "free disk below the minimum";
            else if (own_rss > g_args.max_rss || g_child_rss_last > g_args.max_rss)
                why = "RSS above the limit";
            const auto now = Clock::now();
            if (why || now - last >= std::chrono::seconds(2)) {
                std::printf("[mon] t=%.1fs os_written=%llu free=%llu rss=%llu child_rss=%llu\n",
                            std::chrono::duration<double>(now - t0).count(),
                            (unsigned long long)w, (unsigned long long)fr,
                            (unsigned long long)own_rss, (unsigned long long)g_child_rss_last);
                last = now;
            }
            if (why) {
                if (g_child > 0) kill(g_child, SIGKILL);
                std::printf("[abort] %s\n", why);
                std::fflush(stdout);
                std::_Exit(3);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ---- Model of what the store must hold ----
#pragma pack(push, 1)
struct Rec {
    uint8_t kind; // kIntent, kAck, kFail, kReady, kReads
    uint8_t op;   // kPut, kUpdate, kErase
    uint16_t writer;
    uint32_t key;
    uint64_t tag;
    uint64_t seq;
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 24 && sizeof(Rec) <= PIPE_BUF);
enum : uint8_t { kIntent = 1, kAck = 2, kFail = 3, kReady = 4, kReads = 5 };
enum : uint8_t { kPut = 1, kUpdate = 2, kErase = 3 };

struct KeyState {
    bool exists = false;
    uint64_t tag = 0;
    uint8_t inflight = 0; // op started, not acked
    uint64_t inflight_tag = 0;
};

struct Model {
    std::vector<KeyState> k;
    uint64_t acked[4] = {0, 0, 0, 0}; // by op, this cycle
    uint64_t reads = 0;
    uint64_t fails = 0;
    std::string fail_msg;
};

void writeRec(int fd, const Rec& r) {
    ssize_t n = ::write(fd, &r, sizeof(r));
    if (n != ssize_t(sizeof(r))) std::_Exit(9); // parent gone
}

// Reads records until EOF (every write end closed: the child is dead).
void readRecords(int fd, Model* m) {
    Rec r;
    size_t have = 0;
    uint8_t buf[sizeof(Rec)];
    while (true) {
        ssize_t n = ::read(fd, buf + have, sizeof(buf) - have);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("read pipe");
            std::_Exit(4);
        }
        have += size_t(n);
        if (have < sizeof(buf)) continue;
        have = 0;
        std::memcpy(&r, buf, sizeof(r));
        if (r.kind == kIntent) {
            m->k[r.key].inflight = r.op;
            m->k[r.key].inflight_tag = r.tag;
        } else if (r.kind == kAck) {
            KeyState& s = m->k[r.key];
            if (r.op == kErase) s.exists = false;
            else s.exists = true, s.tag = r.tag;
            s.inflight = 0;
            m->acked[r.op]++;
        } else if (r.kind == kReads) {
            m->reads = r.tag;
        } else if (r.kind == kFail) {
            m->fails++;
            if (m->fail_msg.empty())
                m->fail_msg = "child reported failure code " + std::to_string(r.tag) + " at key " +
                              std::to_string(r.key);
        }
    }
}

// ---- Documents ----
std::string makeDoc(uint64_t id, uint64_t tag, size_t size) {
    doc::Value v = doc::Value::object();
    v.set("id", doc::Value::integer(int64_t(id)));
    v.set("tag", doc::Value::integer(int64_t(tag)));
    v.set("pad", doc::Value::string(""));
    std::string s = doc::serialize(v);
    if (s.size() > size) usage("document size too small");
    v.set("pad", doc::Value::string(std::string(size - s.size(), char('a' + id % 26))));
    s = doc::serialize(v);
    if (s.size() != size) usage("makeDoc size mismatch");
    return s;
}
// id and tag of a stored document; false if it is not one of ours.
bool docFields(const std::string& d, int64_t* id, int64_t* tag, size_t* pad = nullptr) {
    try {
        doc::Value v = doc::parse(d);
        const doc::Value* i = v.find("id");
        const doc::Value* t = v.find("tag");
        const doc::Value* p = v.find("pad");
        if (!i || !t || !p) return false;
        *id = i->asInt();
        *tag = t->asInt();
        if (pad) *pad = p->asString().size();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bpt::IndexedStore::Options storeOptions(uint32_t seg_mb, bool gc, bool auto_cp) {
    bpt::IndexedStore::Options o;
    o.vlog_opts = vlog::VLogWriter::Options(seg_mb * 1024 * 1024, 0);
    o.gc_enabled = gc;
    o.auto_checkpoint = auto_cp;
    return o;
}

// ---- Index == full log replay ----
struct ReplayResult {
    bool ok = true;
    uint64_t entries = 0, keys = 0, copies = 0;
    uint64_t same_doc = 0; // winner is a Full holding the document the index's chain reads as
    std::string why;
};

ReplayResult replayCheck(const std::string& dir,
                         const std::map<std::string, vlog::IndexValueRef>& index,
                         const bpt::IndexedStore& st) {
    ReplayResult rr;
    struct Win {
        uint64_t seq;
        int rank;
        uint8_t type;
        vlog::VPtr vptr;
    };
    std::unordered_map<std::string, Win> win;
    const std::string vdir = dir + "/vlog";
    vlog::VLogReader reader(vdir);
    for (uint32_t seg : bpt::listVLogSegments(vdir)) {
        // is_last_segment = false: never truncates; a torn region would throw (the store was
        // opened and closed cleanly just before, so there is none).
        vlog::ScanResult sr = reader.scanSegment(seg, false, vlog::kBlockSize);
        for (const vlog::RecoveredEntry& e : sr.entries) {
            rr.entries++;
            const uint64_t seq = e.header.seq;
            const int rank = bpt::rebuildWinnerRank(e.header.type, e.header.flags);
            auto it = win.find(e.key);
            if (it == win.end() || seq > it->second.seq ||
                (seq == it->second.seq && rank > it->second.rank))
                win[e.key] = Win{seq, rank, e.header.type, e.vptr};
        }
    }
    auto fail = [&](const std::string& w) {
        if (rr.ok) rr.why = w;
        rr.ok = false;
    };
    for (const auto& [k, w] : win) {
        const bool live = w.type != uint8_t(vlog::EntryType::Tombstone);
        auto it = index.find(k);
        if (!live) {
            if (it != index.end())
                fail("key " + k + ": replay winner is a tombstone (seq " +
                     std::to_string(w.seq) + "), index has seq " + std::to_string(it->second.seq));
            continue;
        }
        rr.keys++;
        if (it == index.end()) {
            fail("key " + k + ": replay has seq " + std::to_string(w.seq) + ", index has no key");
            continue;
        }
        const uint64_t iseq = it->second.seq;
        if (iseq != w.seq) {
            fail("key " + k + ": replay seq " + std::to_string(w.seq) + ", index seq " +
                 std::to_string(iseq));
            continue;
        }
        const vlog::VPtr a = w.vptr, b = it->second.vptr;
        if (std::memcmp(&a, &b, sizeof(a)) != 0) {
            vlog::EntryHeader ha, hb;
            std::string ka, kb;
            std::vector<uint8_t> pa, pb;
            if (!reader.readEntry(a, ha, ka, pa) || !reader.readEntry(b, hb, kb, pb) || ka != kb) {
                fail("key " + k + ": same seq " + std::to_string(iseq) +
                     " at different locations, an entry is unreadable or has another key");
            } else if (ha.type == hb.type && pa == pb) {
                rr.copies++;
            } else if (ha.type != uint8_t(vlog::EntryType::Full)) {
                fail("key " + k + ": same seq " + std::to_string(iseq) +
                     " at different locations; the replay winner is not a Full entry");
            } else {
                // Same version in two encodings? Compare documents, not entries.
                std::optional<std::string> d;
                try {
                    d = st.get(k);
                } catch (const std::exception& e) {
                    fail("key " + k + ": get() threw during replay check: " + e.what());
                    continue;
                }
                const std::string w(pa.begin(), pa.end());
                bool same = d && *d == w;
                if (!same && d) {
                    try {
                        same = doc::parse(*d) == doc::parse(w);
                    } catch (const std::exception&) {
                        same = false;
                    }
                }
                if (same)
                    rr.same_doc++;
                else
                    fail("key " + k + ": same seq " + std::to_string(iseq) +
                         " at different locations with different documents");
            }
        }
    }
    for (const auto& [k, r] : index)
        if (!win.count(k)) fail("key " + k + ": in the index, not in the vLog");
    return rr;
}

std::map<std::string, vlog::IndexValueRef> indexContents(bpt::IndexedStore& st) {
    std::map<std::string, vlog::IndexValueRef> m;
    for (const auto& [k, r] : st.scan(std::string(1, '\x01'), std::string(1, '\x7f'),
                                      size_t(1) << 40))
        m[k] = r;
    return m;
}

// ---- verify(): model check + counters + replay. Resolves in-flight operations. ----
struct VerifyResult {
    bool ok = true;
    double open_s = 0;
    uint64_t keys_checked = 0, inflight = 0, inflight_applied = 0, live = 0;
    ReplayResult replay;
    std::string why;
    size_t segments = 0;
    uint32_t lowest_segment = 0, highest_segment = 0;
    uint64_t store_bytes = 0;
};

VerifyResult verify(const std::string& dir, Model& m, uint32_t seg_mb, size_t doc_size) {
    VerifyResult vr;
    const auto t0 = Clock::now();
    auto st = bpt::IndexedStore::open(dir, storeOptions(seg_mb, false, false));
    vr.open_s = std::chrono::duration<double>(Clock::now() - t0).count();
    auto fail = [&](const std::string& w) {
        if (vr.ok) vr.why = w;
        vr.ok = false;
    };
    for (uint32_t key_id = 0; key_id < m.k.size(); ++key_id) {
        KeyState& s = m.k[key_id];
        vr.keys_checked++;
        std::optional<std::string> d;
        try {
            d = st->get(key(key_id));
        } catch (const std::exception& e) {
            fail("get(" + key(key_id) + ") threw: " + e.what());
            continue;
        }
        int64_t id = -1, tag = -1;
        size_t pad = 0;
        if (d && (!docFields(*d, &id, &tag, &pad) || id != int64_t(key_id))) {
            fail("key " + key(key_id) + ": stored document is not the expected one");
            continue;
        }
        if (d && doc_size && d->size() < doc_size) {
            fail("key " + key(key_id) + ": document shorter than written");
            continue;
        }
        const bool acked_ok = s.exists ? (d && uint64_t(tag) == s.tag) : !d;
        bool inflight_ok = false;
        if (s.inflight) {
            vr.inflight++;
            inflight_ok = s.inflight == kErase ? !d : (d && uint64_t(tag) == s.inflight_tag);
        }
        if (acked_ok) {
            // the in-flight write did not land (or there was none)
        } else if (inflight_ok) {
            vr.inflight_applied++;
            s.exists = s.inflight != kErase;
            s.tag = s.inflight_tag;
        } else {
            fail("key " + key(key_id) + ": expected " +
                 (s.exists ? "tag " + std::to_string(s.tag) : std::string("absent")) +
                 (s.inflight ? " (or in-flight op " + std::to_string(s.inflight) + " tag " +
                                   std::to_string(s.inflight_tag) + ")"
                             : std::string("")) +
                 ", found " + (d ? "tag " + std::to_string(tag) : std::string("absent")));
        }
        s.inflight = 0;
        if (s.exists) vr.live++;
    }
    const std::string cv = st->verifyCountersForTest();
    if (!cv.empty()) fail("GC counters: " + cv);
    const auto idx = indexContents(*st);
    if (idx.size() != vr.live)
        fail("index holds " + std::to_string(idx.size()) + " keys, model " +
             std::to_string(vr.live));
    vr.replay = replayCheck(dir, idx, *st); // store open, idle (no writers, GC off)
    st->close();
    st.reset();
    const std::vector<uint32_t> segs = bpt::listVLogSegments(dir + "/vlog");
    vr.segments = segs.size();
    vr.lowest_segment = segs.empty() ? 0 : segs.front();
    vr.highest_segment = segs.empty() ? 0 : segs.back();
    vr.store_bytes = dirBytes(dir);
    if (!vr.replay.ok) fail("replay: " + vr.replay.why);
    return vr;
}

void printVerify(const char* tag, const VerifyResult& v) {
    std::printf("[%s] verify=%s open_s=%.2f keys_checked=%llu live=%llu inflight=%llu "
                "inflight_applied=%llu replay_entries=%llu replay_keys=%llu gc_copies=%llu "
                "same_doc_other_encoding=%llu segments=%zu "
                "segment_ids=%u-%u store_bytes=%llu%s%s\n",
                tag, v.ok ? "OK" : "FAIL", v.open_s, (unsigned long long)v.keys_checked,
                (unsigned long long)v.live, (unsigned long long)v.inflight,
                (unsigned long long)v.inflight_applied, (unsigned long long)v.replay.entries,
                (unsigned long long)v.replay.keys, (unsigned long long)v.replay.copies,
                (unsigned long long)v.replay.same_doc, v.segments, v.lowest_segment, v.highest_segment,
                (unsigned long long)v.store_bytes, v.ok ? "" : " why=", v.ok ? "" : v.why.c_str());
}

// Parallel preload of keys [from, to) with tag = key id.
void preload(bpt::IndexedStore& st, Model& m, uint32_t from, uint32_t to, size_t doc_size) {
    std::vector<std::thread> th;
    for (int t = 0; t < 64; ++t)
        th.emplace_back([&, t] {
            for (uint32_t i = from + uint32_t(t); i < to; i += 64) st.put(key(i), makeDoc(i, i, doc_size));
        });
    for (auto& x : th) x.join();
    for (uint32_t i = from; i < to; ++i) m.k[i].exists = true, m.k[i].tag = i;
}

// fork() with the monitor quiet (it holds g_child_mu while it polls/prints).
pid_t forkChild(int* pipe_rd, int* pipe_wr) {
    int p[2];
    if (pipe(p) != 0) {
        std::perror("pipe");
        std::_Exit(4);
    }
    std::lock_guard<std::mutex> lk(g_child_mu);
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        std::_Exit(4);
    }
    if (pid == 0) {
        close(p[0]);
        *pipe_wr = p[1];
        *pipe_rd = -1;
        return 0;
    }
    close(p[1]);
    *pipe_rd = p[0];
    *pipe_wr = -1;
    g_child = pid;
    g_child_last = 0;
    return pid;
}

// Kills (if alive) and reaps the child. Returns its wait status.
int killAndReap(pid_t pid, bool kill9) {
    int status = 0;
    {
        std::lock_guard<std::mutex> lk(g_child_mu);
        pollChildLocked(); // last byte count before it dies
        if (kill9) kill(pid, SIGKILL);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    std::lock_guard<std::mutex> lk(g_child_mu);
    childFinishedLocked();
    return status;
}

std::string statusText(int st) {
    if (WIFSIGNALED(st)) return "signal " + std::to_string(WTERMSIG(st));
    if (WIFEXITED(st)) return "exit " + std::to_string(WEXITSTATUS(st));
    return "status " + std::to_string(st);
}

// ---- soak ----
[[noreturn]] void soakChild(const Args& a, int wr, const Model& m, uint32_t cycle) {
    std::unique_ptr<bpt::IndexedStore> st;
    try {
        st = bpt::IndexedStore::open(a.dir, storeOptions(a.seg_mb, true, true));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "child open failed: %s\n", e.what());
        std::_Exit(7);
    }
    writeRec(wr, Rec{kReady, 0, 0, 0, 0, 0});
    std::atomic<uint64_t> reads{0};
    std::vector<std::thread> th;
    for (int w = 0; w < a.writers; ++w)
        th.emplace_back([&, w] {
            std::vector<KeyState> local = m.k; // this writer's keys: key % writers == w
            std::mt19937_64 rng(cycle * 1000 + uint64_t(w));
            const auto period = std::chrono::duration<double>(double(a.writers) / a.rate);
            auto next = Clock::now();
            uint64_t n = 0;
            const uint32_t per = a.keys / uint32_t(a.writers);
            while (true) {
                next += std::chrono::duration_cast<Clock::duration>(period);
                std::this_thread::sleep_until(next);
                const uint32_t k = uint32_t(w) + uint32_t(a.writers) * uint32_t(rng() % per);
                KeyState& s = local[k];
                uint8_t op;
                const uint64_t r = rng() % 100;
                if (!s.exists) op = kPut;
                else op = r < 55 ? kUpdate : r < 75 ? kPut : kErase;
                const uint64_t tag = (uint64_t(cycle) << 44) | (uint64_t(w) << 36) | ++n;
                writeRec(wr, Rec{kIntent, op, uint16_t(w), k, tag, 0});
                uint64_t seq = 0;
                try {
                    if (op == kPut) seq = st->put(key(k), makeDoc(k, tag, 1024));
                    else if (op == kErase) seq = st->erase(key(k)).seq;
                    else
                        seq = st->update(key(k), {doc::set("tag", doc::Value::integer(int64_t(tag)))}).seq;
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "writer %d: %s\n", w, e.what());
                    writeRec(wr, Rec{kFail, op, uint16_t(w), k, 1, 0});
                    std::_Exit(7);
                }
                writeRec(wr, Rec{kAck, op, uint16_t(w), k, tag, seq});
                if (op == kErase) s.exists = false;
                else s.exists = true, s.tag = tag;
            }
        });
    for (int r = 0; r < a.readers; ++r)
        th.emplace_back([&, r] {
            std::mt19937_64 rng(cycle * 7777 + uint64_t(r));
            while (true) {
                const uint32_t k = uint32_t(rng() % a.keys);
                try {
                    if (rng() % 10 < 7) {
                        auto d = st->get(key(k));
                        int64_t id, tag;
                        if (d && (!docFields(*d, &id, &tag) || id != int64_t(k))) {
                            writeRec(wr, Rec{kFail, 0, 0, k, 2, 0});
                            std::_Exit(7);
                        }
                    } else {
                        const uint32_t end = std::min(a.keys, k + 100);
                        for (const auto& [kk, d] : st->scanDocuments(key(k), key(end), 100)) {
                            int64_t id, tag;
                            if (!docFields(d, &id, &tag) || key(uint64_t(id)) != kk) {
                                writeRec(wr, Rec{kFail, 0, 0, k, 3, 0});
                                std::_Exit(7);
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "reader %d: %s\n", r, e.what());
                    writeRec(wr, Rec{kFail, 0, 0, k, 4, 0});
                    std::_Exit(7);
                }
                const uint64_t n = ++reads;
                if (n % 20000 == 0) writeRec(wr, Rec{kReads, 0, 0, 0, n, 0});
            }
        });
    for (auto& t : th) t.join(); // never returns: the parent kills this process
    std::_Exit(0);
}

// Counters + index == full log replay on an existing store (no model: the acked-write model
// lived in the soak parent). Opens the store, so run it on a disposable copy.
int runReplayOnly(const Args& a) {
    if (!std::filesystem::exists(a.dir)) usage("--dir does not exist");
    auto st = bpt::IndexedStore::open(a.dir, storeOptions(16, false, false));
    const std::string cv = st->verifyCountersForTest();
    const auto idx = indexContents(*st);
    ReplayResult rr = replayCheck(a.dir, idx, *st);
    st->close();
    std::printf("[replay-only] counters=%s index_keys=%zu replay=%s replay_entries=%llu "
                "replay_keys=%llu gc_copies=%llu same_doc_other_encoding=%llu%s%s\n",
                cv.empty() ? "OK" : cv.c_str(), idx.size(), rr.ok ? "OK" : "FAIL",
                (unsigned long long)rr.entries, (unsigned long long)rr.keys,
                (unsigned long long)rr.copies, (unsigned long long)rr.same_doc,
                rr.ok ? "" : " why=", rr.ok ? "" : rr.why.c_str());
    return cv.empty() && rr.ok ? 0 : 8;
}

int runSoak(const Args& a) {
    if (std::filesystem::exists(a.dir)) usage("--dir already exists");
    Model m;
    m.k.resize(a.keys);
    {
        auto st = bpt::IndexedStore::open(a.dir, storeOptions(a.seg_mb, false, true));
        const auto t0 = Clock::now();
        preload(*st, m, 0, a.preload, 1024);
        st->close();
        std::printf("[preload] docs=%u secs=%.1f os_written=%llu\n", a.preload,
                    std::chrono::duration<double>(Clock::now() - t0).count(),
                    (unsigned long long)totalWritten());
    }
    const auto start = Clock::now();
    const double total_s = a.minutes * 60;
    uint64_t total_acked = 0, total_reads = 0, total_inflight_applied = 0;
    int failures = 0;
    for (uint32_t cycle = 1;; ++cycle) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        if (elapsed >= total_s - 5) break;
        const double run_s = std::min<double>(a.kill_every_s, total_s - elapsed);
        int rd, wr;
        m.acked[1] = m.acked[2] = m.acked[3] = 0;
        m.reads = 0;
        const pid_t pid = forkChild(&rd, &wr);
        if (pid == 0) soakChild(a, wr, m, cycle);
        std::thread reader(readRecords, rd, &m);
        const auto cs = Clock::now();
        bool died = false;
        int status = 0;
        uint64_t rss_max = 0;
        auto next_rss = cs;
        while (std::chrono::duration<double>(Clock::now() - cs).count() < run_s) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (waitpid(pid, &status, WNOHANG) == pid) {
                died = true;
                break;
            }
            if (Clock::now() >= next_rss) {
                uint64_t rss;
                {
                    std::lock_guard<std::mutex> lk(g_child_mu);
                    pollChildLocked();
                    rss = g_child_rss_last;
                }
                rss_max = std::max(rss_max, rss);
                std::printf("[rss] cycle=%u t=%.0fs child_rss=%llu\n", cycle,
                            std::chrono::duration<double>(Clock::now() - start).count(),
                            (unsigned long long)rss);
                next_rss += std::chrono::seconds(60);
            }
        }
        if (died) {
            std::lock_guard<std::mutex> lk(g_child_mu);
            childFinishedLocked();
        } else {
            status = killAndReap(pid, true);
        }
        reader.join();
        close(rd);
        const uint64_t acked = m.acked[1] + m.acked[2] + m.acked[3];
        total_acked += acked;
        total_reads += m.reads;
        std::printf("[cycle] n=%u run_s=%.0f child=%s acked=%llu puts=%llu updates=%llu "
                    "erases=%llu reads=%llu child_rss_max=%llu os_written_total=%llu\n",
                    cycle, run_s, died ? ("died: " + statusText(status)).c_str() : "killed -9",
                    (unsigned long long)acked, (unsigned long long)m.acked[kPut],
                    (unsigned long long)m.acked[kUpdate], (unsigned long long)m.acked[kErase],
                    (unsigned long long)m.reads, (unsigned long long)rss_max,
                    (unsigned long long)totalWritten());
        if (died || m.fails) {
            std::printf("[fail] child stopped on its own or reported a failure: %s %s\n",
                        statusText(status).c_str(), m.fail_msg.c_str());
            failures++;
        }
        VerifyResult v = verify(a.dir, m, a.seg_mb, 0);
        total_inflight_applied += v.inflight_applied;
        printVerify("verify", v);
        if (!v.ok) failures++;
        if (failures) break; // stop at the first failure: keep the store as evidence
    }
    std::printf("[soak] secs=%.0f acked_total=%llu reads_total=%llu inflight_applied_total=%llu "
                "child_rss_peak=%llu failures=%d\n",
                std::chrono::duration<double>(Clock::now() - start).count(),
                (unsigned long long)total_acked, (unsigned long long)total_reads,
                (unsigned long long)total_inflight_applied, (unsigned long long)g_child_rss_peak,
                failures);
    return failures ? 8 : 0;
}

// ---- fill (disk full) ----
uint64_t writeFiller(const std::string& path, uint64_t bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        std::perror("open filler");
        std::_Exit(4);
    }
    std::vector<char> buf(1 << 20, 'F');
    uint64_t done = 0;
    while (done < bytes) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), bytes - done));
        const ssize_t w = ::write(fd, buf.data(), n);
        if (w <= 0) {
            std::printf("[filler] write stopped at %llu bytes: %s\n", (unsigned long long)done,
                        std::strerror(errno));
            break;
        }
        done += uint64_t(w);
    }
    if (fsync(fd) != 0) std::printf("[filler] fsync: %s\n", std::strerror(errno));
    close(fd);
    return done;
}

void listStore(const std::string& dir) {
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file(ec))
            std::printf("[files] %s %llu\n", e.path().lexically_relative(dir).c_str(),
                        (unsigned long long)e.file_size(ec));
}

int runFill(const Args& a) {
    if (a.scenario != "vlog" && a.scenario != "index" && a.scenario != "index-ckpt")
        usage("--scenario vlog|index|index-ckpt");
    if (a.filler.empty()) usage("--filler is required");
    if (std::filesystem::exists(a.dir)) usage("--dir already exists");
    // index-ckpt: the child updates with automatic checkpoints off (dirty index pages pile up),
    // the parent fills the volume, then the child checkpoints: the first write that can fail is
    // an index.db page write.
    const bool ckpt = a.scenario == "index-ckpt";
    const bool idx = a.scenario == "index" || ckpt;
    const size_t doc_size = idx ? 128 : 1024;
    const uint32_t preload_n = idx ? 300000 : 20000;
    const uint32_t keys = idx ? preload_n : 400000;
    const std::string vol = std::filesystem::path(a.dir).parent_path().string();
    Model m;
    m.k.resize(keys);
    {
        auto st = bpt::IndexedStore::open(a.dir, storeOptions(16, false, true));
        preload(*st, m, 0, preload_n, doc_size);
        st->close();
    }
    std::printf("[fill] scenario=%s preloaded=%u doc_size=%zu volume_free=%llu\n",
                a.scenario.c_str(), preload_n, doc_size, (unsigned long long)freeBytes(vol));
    listStore(a.dir);
    auto fillVolume = [&] {
        const uint64_t fr = freeBytes(vol), leave = a.leave_mb * 1024 * 1024;
        const uint64_t filler = fr > leave ? fr - leave : 0;
        const uint64_t wrote = writeFiller(a.filler, filler);
        std::printf("[fill] filler=%llu volume_free=%llu\n", (unsigned long long)wrote,
                    (unsigned long long)freeBytes(vol));
    };
    if (!ckpt) fillVolume();

    // 1. Write until the child dies.
    int ctl[2] = {-1, -1}; // index-ckpt: parent -> child "checkpoint now"
    if (ckpt && pipe(ctl) != 0) {
        std::perror("pipe");
        std::_Exit(4);
    }
    int rd, wr;
    const pid_t pid = forkChild(&rd, &wr);
    if (pid == 0 && ckpt) {
        close(ctl[1]);
        std::unique_ptr<bpt::IndexedStore> st =
            bpt::IndexedStore::open(a.dir, storeOptions(16, false, false));
        std::vector<std::thread> th;
        for (int w = 0; w < 16; ++w)
            th.emplace_back([&, w] {
                std::mt19937_64 rng(uint64_t(w) + 77);
                for (uint64_t n = 1; n <= 1250; ++n) {
                    const uint64_t tag = (uint64_t(3) << 44) | (uint64_t(w) << 36) | n;
                    const uint32_t k = uint32_t(w) + 16 * uint32_t(rng() % (keys / 16));
                    writeRec(wr, Rec{kIntent, kUpdate, uint16_t(w), k, tag, 0});
                    const uint64_t seq =
                        st->update(key(k), {doc::set("tag", doc::Value::integer(int64_t(tag)))}).seq;
                    writeRec(wr, Rec{kAck, kUpdate, uint16_t(w), k, tag, seq});
                }
            });
        for (auto& t : th) t.join();
        writeRec(wr, Rec{kReady, 0, 0, 0, 0, 0});
        char c;
        if (::read(ctl[0], &c, 1) != 1) std::_Exit(12);
        std::printf("[ckpt-child] checkpoint() on a full volume\n");
        std::fflush(stdout);
        bpt::CheckpointReport r = st->checkpoint();
        std::printf("[ckpt-child] checkpoint returned: pages=%llu (no failure)\n",
                    (unsigned long long)r.pages_written);
        std::fflush(stdout);
        std::_Exit(0);
    }
    if (pid == 0) {
        std::unique_ptr<bpt::IndexedStore> st =
            bpt::IndexedStore::open(a.dir, storeOptions(16, false, true));
        writeRec(wr, Rec{kReady, 0, 0, 0, 0, 0});
        std::vector<std::thread> th;
        std::atomic<uint32_t> next{preload_n};
        for (int w = 0; w < 16; ++w)
            th.emplace_back([&, w] {
                std::mt19937_64 rng(uint64_t(w) + 99);
                for (uint64_t n = 1;; ++n) {
                    const uint64_t tag = (uint64_t(1) << 44) | (uint64_t(w) << 36) | n;
                    if (idx) {
                        // Random single-field updates: tiny vLog entries, many dirty index pages.
                        const uint32_t k = uint32_t(w) + 16 * uint32_t(rng() % (keys / 16));
                        writeRec(wr, Rec{kIntent, kUpdate, uint16_t(w), k, tag, 0});
                        const uint64_t seq =
                            st->update(key(k), {doc::set("tag", doc::Value::integer(int64_t(tag)))}).seq;
                        writeRec(wr, Rec{kAck, kUpdate, uint16_t(w), k, tag, seq});
                    } else {
                        const uint32_t k = next++;
                        if (k >= keys) std::_Exit(10); // never filled the volume
                        writeRec(wr, Rec{kIntent, kPut, uint16_t(w), k, tag, 0});
                        const uint64_t seq = st->put(key(k), makeDoc(k, tag, doc_size));
                        writeRec(wr, Rec{kAck, kPut, uint16_t(w), k, tag, seq});
                    }
                }
            });
        for (auto& t : th) t.join();
        std::_Exit(0);
    }
    if (ckpt) {
        close(ctl[0]);
        // Read records until the child is ready, fill the volume, then let it checkpoint.
        Rec r;
        while (::read(rd, &r, sizeof(r)) == ssize_t(sizeof(r))) {
            if (r.kind == kReady) break;
            if (r.kind == kIntent) m.k[r.key].inflight = r.op, m.k[r.key].inflight_tag = r.tag;
            if (r.kind == kAck) {
                m.k[r.key].exists = true, m.k[r.key].tag = r.tag, m.k[r.key].inflight = 0;
                m.acked[r.op]++;
            }
        }
        std::printf("[ckpt] child acked %llu updates; filling the volume\n",
                    (unsigned long long)m.acked[kUpdate]);
        listStore(a.dir);
        fillVolume();
        if (::write(ctl[1], "c", 1) != 1) std::perror("write ctl");
        close(ctl[1]);
    }
    std::thread reader(readRecords, rd, &m);
    int status = 0;
    const auto t0 = Clock::now();
    bool timed_out = false;
    while (waitpid(pid, &status, WNOHANG) != pid) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (Clock::now() - t0 > std::chrono::minutes(10)) {
            timed_out = true;
            status = killAndReap(pid, true);
            break;
        }
        std::lock_guard<std::mutex> lk(g_child_mu);
        pollChildLocked();
    }
    if (!timed_out) {
        std::lock_guard<std::mutex> lk(g_child_mu);
        childFinishedLocked();
    }
    reader.join();
    close(rd);
    std::printf("[writer] child=%s%s secs=%.1f acked_puts=%llu acked_updates=%llu "
                "volume_free=%llu\n",
                statusText(status).c_str(), timed_out ? " (timed out, killed)" : "",
                std::chrono::duration<double>(Clock::now() - t0).count(),
                (unsigned long long)m.acked[kPut], (unsigned long long)m.acked[kUpdate],
                (unsigned long long)freeBytes(vol));
    listStore(a.dir);

    // 2. Reopen while the volume is still full (in a child: it may abort).
    {
        const pid_t p2 = forkChild(&rd, &wr);
        if (p2 == 0) {
            try {
                auto st = bpt::IndexedStore::open(a.dir, storeOptions(16, false, false));
                std::printf("[reopen-full] opened\n");
                st->close();
                std::printf("[reopen-full] closed\n");
            } catch (const std::exception& e) {
                std::printf("[reopen-full] threw: %s\n", e.what());
                std::fflush(stdout);
                std::_Exit(11);
            }
            std::fflush(stdout);
            std::_Exit(0);
        }
        close(rd);
        int st2 = 0;
        while (waitpid(p2, &st2, 0) < 0 && errno == EINTR) {
        }
        {
            std::lock_guard<std::mutex> lk(g_child_mu);
            childFinishedLocked();
        }
        std::printf("[reopen-full] child=%s volume_free=%llu\n", statusText(st2).c_str(),
                    (unsigned long long)freeBytes(vol));
        listStore(a.dir);
    }

    // 3. Free space, reopen, verify; then show the store still takes writes.
    if (::unlink(a.filler.c_str()) != 0) std::perror("unlink filler");
    std::printf("[fill] filler deleted, volume_free=%llu\n", (unsigned long long)freeBytes(vol));
    VerifyResult v = verify(a.dir, m, 16, doc_size);
    printVerify("verify", v);
    {
        auto st = bpt::IndexedStore::open(a.dir, storeOptions(16, false, false));
        const uint32_t k = idx ? 0 : std::min<uint32_t>(keys - 1, preload_n);
        const uint64_t tag = uint64_t(2) << 44;
        st->put(key(k), makeDoc(k, tag, doc_size));
        m.k[k].exists = true;
        m.k[k].tag = tag;
        st->close();
    }
    VerifyResult v2 = verify(a.dir, m, 16, doc_size);
    printVerify("verify-after-write", v2);
    return v.ok && v2.ok ? 0 : 8;
}

// ---- large documents ----
int runLarge(const Args& a) {
    if (std::filesystem::exists(a.dir)) usage("--dir already exists");
    const uint64_t max_doc = 16ULL * 1024 * 1024 - 32 - 16; // 10-byte keys: align8(10) = 16
    struct Cls {
        const char* name;
        uint64_t size;
        uint32_t count, base;
    };
    const Cls classes[] = {{"64KB", 65536, 50, 0},        {"1MB", 1048576, 20, 1000},
                           {"4MB", 4194304, 8, 2000},     {"16MB", 16000000, 3, 3000},
                           {"max", max_doc, 2, 4000}};
    Model m;
    m.k.resize(6000);
    std::vector<uint64_t> sizes(6000, 0);
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        if (!ok) {
            failures++;
            std::printf("[fail] %s\n", what.c_str());
        }
    };
    auto st = bpt::IndexedStore::open(a.dir, storeOptions(64, false, false));
    for (const Cls& c : classes) {
        auto ms = [](Clock::time_point t) {
            return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        };
        // put + get
        auto t = Clock::now();
        for (uint32_t i = 0; i < c.count; ++i) {
            const uint32_t k = c.base + i;
            st->put(key(k), makeDoc(k, 1, c.size));
            m.k[k].exists = true, m.k[k].tag = 1;
            sizes[k] = c.size;
        }
        const double put_ms = ms(t) / c.count;
        t = Clock::now();
        for (uint32_t i = 0; i < c.count; ++i) {
            const uint32_t k = c.base + i;
            auto d = st->get(key(k));
            check(d && *d == makeDoc(k, 1, c.size), std::string(c.name) + " get " + key(k));
        }
        const double get_ms = ms(t) / c.count;
        // update (one field): Delta or consolidated Full
        t = Clock::now();
        uint32_t deltas = 0, consolidated = 0;
        for (uint32_t i = 0; i < c.count; ++i) {
            const uint32_t k = c.base + i;
            auto r = st->update(key(k), {doc::set("tag", doc::Value::integer(2))});
            check(r.written, std::string(c.name) + " update wrote nothing");
            (r.consolidated ? consolidated : deltas)++;
            m.k[k].tag = 2;
        }
        const double upd_ms = ms(t) / c.count;
        t = Clock::now();
        for (uint32_t i = 0; i < c.count; ++i) {
            const uint32_t k = c.base + i;
            auto d = st->get(key(k));
            int64_t id = -1, tag = -1;
            size_t pad = 0;
            check(d && docFields(*d, &id, &tag, &pad) && id == k && tag == 2 && pad > 0,
                  std::string(c.name) + " get after update " + key(k));
        }
        const double get2_ms = ms(t) / c.count;
        // scan
        t = Clock::now();
        auto docs = st->scanDocuments(key(c.base), key(c.base + c.count), c.count);
        const double scan_ms = ms(t);
        check(docs.size() == c.count, std::string(c.name) + " scan count");
        for (const auto& [k, d] : docs) {
            int64_t id = -1, tag = -1;
            check(docFields(d, &id, &tag) && key(uint64_t(id)) == k && tag == 2,
                  std::string(c.name) + " scan doc " + k);
        }
        // checkpoint
        bpt::CheckpointReport cp = st->checkpoint();
        // overwrite (old versions become garbage), then GC until nothing is left to do
        for (uint32_t i = 0; i < c.count; ++i) {
            const uint32_t k = c.base + i;
            st->put(key(k), makeDoc(k, 3, c.size));
            m.k[k].tag = 3;
        }
        st->checkpoint();
        uint64_t relocated = 0, gc_bytes = 0;
        int passes = 0;
        t = Clock::now();
        for (; passes < 64; ++passes) {
            bpt::GcReport r = st->runGcPass();
            if (r.action == bpt::GcReport::Action::None) break;
            relocated += r.keys_relocated;
            gc_bytes += r.bytes_written;
            st->checkpoint(); // retired segments are condemned/unlinked over checkpoints
        }
        const double gc_ms = ms(t);
        for (uint32_t i = 0; i < c.count; ++i) {
            const uint32_t k = c.base + i;
            auto d = st->get(key(k));
            check(d && *d == makeDoc(k, 3, c.size), std::string(c.name) + " get after GC " + key(k));
        }
        const std::string cv = st->verifyCountersForTest();
        check(cv.empty(), std::string(c.name) + " counters: " + cv);
        std::printf("[large] class=%s size=%llu count=%u put_ms=%.1f get_ms=%.1f update_ms=%.1f "
                    "deltas=%u consolidated=%u get_after_update_ms=%.1f scan_ms=%.1f "
                    "checkpoint_pages=%llu gc_passes=%d gc_relocated=%llu gc_bytes=%llu "
                    "gc_ms=%.0f rss=%llu\n",
                    c.name, (unsigned long long)c.size, c.count, put_ms, get_ms, upd_ms, deltas,
                    consolidated, get2_ms, scan_ms, (unsigned long long)cp.pages_written, passes,
                    (unsigned long long)relocated, (unsigned long long)gc_bytes, gc_ms,
                    (unsigned long long)rssNow());
    }
    // Above the maximum.
    {
        const uint32_t k = 5000;
        const std::string big(max_doc + 1, 'x');
        try {
            st->put(key(k), big);
            check(false, "put of max+1 bytes was accepted");
        } catch (const std::invalid_argument& e) {
            std::printf("[above-max] put of %llu bytes (key of 10 bytes): std::invalid_argument: "
                        "%s\n",
                        (unsigned long long)big.size(), e.what());
        }
        check(!st->get(key(k)), "max+1 put left a key behind");
        const uint32_t km = 4000; // a maximum-size document
        try {
            st->update(key(km), {doc::set("extra", doc::Value::string("grow"))});
            check(false, "update past the maximum was accepted");
        } catch (const doc::RangeError& e) {
            std::printf("[above-max] update growing a %llu-byte document: doc::RangeError: %s\n",
                        (unsigned long long)max_doc, e.what());
        }
        auto d = st->get(key(km));
        check(d && *d == makeDoc(km, 3, max_doc), "max document changed by a refused update");
        const std::string just_max = makeDoc(k, 9, max_doc);
        st->put(key(k), just_max);
        check(st->get(key(k)) == just_max, "put of exactly the maximum");
        m.k[k].exists = true, m.k[k].tag = 9;
        std::printf("[above-max] put of exactly %llu bytes: accepted and read back\n",
                    (unsigned long long)max_doc);
    }
    st->close();
    st.reset();
    VerifyResult v0 = verify(a.dir, m, 64, 0);
    printVerify("verify-before-kill", v0);
    if (!v0.ok) failures++;

    // kill -9 during 16,000,000-byte puts: three rounds, kill 0-40 ms after the third intent.
    for (int round = 0; round < 3; ++round) {
        int rd, wr;
        const pid_t pid = forkChild(&rd, &wr);
        if (pid == 0) {
            auto cst = bpt::IndexedStore::open(a.dir, storeOptions(64, true, true));
            for (uint32_t i = 0;; ++i) {
                const uint32_t k = 5100 + uint32_t(round) * 10 + i;
                const uint64_t tag = 100 + uint64_t(round) * 10 + i;
                writeRec(wr, Rec{kIntent, kPut, 0, k, tag, 0});
                const uint64_t seq = cst->put(key(k), makeDoc(k, tag, 16000000));
                writeRec(wr, Rec{kAck, kPut, 0, k, tag, seq});
                if (i >= 5) for (;;) pause();
            }
        }
        // Count records as they arrive; kill after the third intent.
        int intents = 0;
        Rec r;
        std::mt19937 rng(uint32_t(round) + 5);
        while (intents < 3) {
            if (::read(rd, &r, sizeof(r)) != ssize_t(sizeof(r))) break;
            if (r.kind == kIntent) {
                intents++;
                m.k[r.key].inflight = kPut, m.k[r.key].inflight_tag = r.tag;
            } else if (r.kind == kAck) {
                m.k[r.key].exists = true, m.k[r.key].tag = r.tag, m.k[r.key].inflight = 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 41));
        const int status = killAndReap(pid, true);
        readRecords(rd, &m); // what was sent before the kill
        close(rd);
        VerifyResult v = verify(a.dir, m, 64, 0);
        std::printf("[kill] round=%d child=%s ", round, statusText(status).c_str());
        printVerify("verify", v);
        if (!v.ok) failures++;
    }
    std::printf("[large] failures=%d\n", failures);
    return failures ? 8 : 0;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_args = parse(argc, argv);
    const Args& a = g_args;
    const uint64_t free0 = freeBytes(a.free_check_path);
    std::printf("[disk] free_before=%llu (%s)\n", (unsigned long long)free0,
                a.free_check_path.c_str());
    if (free0 < a.min_free) usage("not enough free disk to start");
    g_os0 = osStats();
    std::thread mon(monitor);
    int rc = 2;
    if (a.mode == "soak") rc = runSoak(a);
    else if (a.mode == "fill") rc = runFill(a);
    else if (a.mode == "large") rc = runLarge(a);
    else if (a.mode == "replay") rc = runReplayOnly(a);
    else usage("unknown mode");
    if (rc == 0) {
        std::error_code ec;
        std::filesystem::remove_all(a.dir, ec);
        if (ec) std::printf("[warn] remove_all failed: %s\n", ec.message().c_str());
    } else {
        std::printf("[keep] store kept as evidence: %s\n", a.dir.c_str());
    }
    std::printf("[disk] free_after=%llu (dataset %s)\n",
                (unsigned long long)freeBytes(a.free_check_path), rc == 0 ? "deleted" : "kept");
    const uint64_t w = totalWritten();
    std::printf("[final] os_written=%llu exit=%d child_rss_peak=%llu own_rss_peak=%llu\n",
                (unsigned long long)w, rc, (unsigned long long)g_child_rss_peak,
                (unsigned long long)rssPeak());
    g_stop = true;
    mon.join();
    return rc;
}
