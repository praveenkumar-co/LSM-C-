// Phase 4 store tests: delta updates through IndexedStore, chains, mixed sequences, concurrency,
// and crash recovery with Delta entries in the vLog (P4.C6, C8-C10, P4.D, P4.E, P4.F, P4.G).
#include "test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/DeltaCodec.hpp"
#include "index/IndexRecovery.hpp"
#include "index/IndexedStore.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using bpt::IndexedStore;
using doc::Value;
using Ops = std::vector<doc::UpdateOp>;
using Snapshot = std::map<std::string, std::string>; // relative path -> file bytes

// ------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------
static IndexedStore::Options storeOpts(size_t frames = 64) {
    IndexedStore::Options o;
    o.cache_frames = frames;
    o.vlog_opts = vlog::VLogWriter::Options(
        1024 * 1024, std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index.db / superblock.db write"); };
    o.auto_checkpoint = false;
    return o;
}

static vlog::VLogWriter::Options writerOpts() {
    return vlog::VLogWriter::Options(1024 * 1024,
                                     std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
}

// vLog bytes are counted once per test from the segment files (index writes are counted live).
static void accountVLog(const std::string& dir) {
    uint64_t total = 0;
    for (uint32_t id : bpt::listVLogSegments(dir + "/vlog")) {
        char name[32];
        std::snprintf(name, sizeof(name), "/vlog/vlog_%08u.log", id);
        struct stat st;
        CHECK(::stat((dir + name).c_str(), &st) == 0);
        total += static_cast<uint64_t>(st.st_size);
    }
    accountBytes(total, "vLog segments");
}

static Snapshot snapshot(const std::string& dir) {
    Snapshot s;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        s[std::filesystem::relative(e.path(), dir).string()] = ss.str();
    }
    return s;
}

// key -> (seq, vptr) for every key in the index.
using IndexState = std::map<std::string, std::pair<uint64_t, vlog::VPtr>>;
static IndexState indexState(IndexedStore& st) {
    IndexState out;
    for (const auto& [k, ref] : st.tree().scanFrom("", SIZE_MAX)) out[k] = {ref.seq, ref.vptr};
    return out;
}
static bool sameState(const IndexState& a, const IndexState& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [k, v] : a) {
        auto it = b.find(k);
        if (it == b.end() || it->second.first != v.first ||
            std::memcmp(&it->second.second, &v.second, sizeof(vlog::VPtr)) != 0)
            return false;
    }
    return true;
}

// Appends a Delta entry with the given base directly to the vLog (as update() would).
static vlog::AppendResult appendDelta(vlog::VLogWriter& w, const std::string& key,
                                      uint64_t base_seq, const vlog::VPtr& base, uint16_t depth,
                                      const Ops& ops) {
    const std::string enc = bpt::encodeOps(ops);
    vlog::DeltaPrefix pre{};
    pre.base_seq = base_seq;
    pre.base = base;
    pre.chain_depth = depth;
    pre.op_count = static_cast<uint16_t>(ops.size());
    return w.append(vlog::EntryType::Delta, key, enc.data(), static_cast<uint32_t>(enc.size()), 0,
                    0, &pre);
}

template <typename E, typename F> static std::string expectThrow(F&& f, const char* what) {
    try {
        f();
    } catch (const E& e) {
        return e.what();
    }
    std::cerr << what << ": expected exception was not thrown\n";
    CHECK(false);
    return "";
}

// A canonical JSON document with a padding field, large enough that a one-field delta stays
// under half of it (smaller documents always get a consolidated Full entry).
static std::string padDoc(int id, size_t pad = 240) {
    Value v = Value::object();
    v.set("id", Value::integer(id));
    v.set("n", Value::integer(0));
    v.set("name", Value::string("doc" + std::to_string(id)));
    v.set("pad", Value::string(std::string(pad, static_cast<char>('a' + id % 26))));
    return doc::serialize(v);
}

static std::string applyToCopy(const std::string& before, const Ops& ops) {
    Value v = doc::parse(before);
    doc::applyOps(v, ops);
    return doc::serialize(v);
}

static vlog::EntryHeader rawEntry(const std::string& vdir, const vlog::VPtr& p,
                                  std::vector<uint8_t>* payload = nullptr) {
    vlog::VLogReader r(vdir);
    vlog::EntryHeader h;
    std::string k;
    std::vector<uint8_t> pl;
    CHECK(r.readEntry(p, h, k, pl));
    if (payload) *payload = std::move(pl);
    return h;
}

// Independent reference: replays the whole vLog in seq order with the delta rule, checking that
// every Delta's base is the key's previous entry, and rebuilds each key's document (Full entries
// as stored, chains as canonical JSON), the way get() must return it.
struct Replayed {
    std::map<std::string, std::string> docs;
    uint64_t fulls = 0, consolidated = 0, deltas = 0, tombstones = 0;
};
static Replayed replayVLog(const std::string& vdir) {
    struct Cur {
        uint64_t seq;
        std::string bytes;
        uint16_t depth;
    };
    std::map<std::string, Cur> cur;
    Replayed out;
    std::vector<uint32_t> segs = bpt::listVLogSegments(vdir);
    vlog::VLogReader reader(vdir);
    uint64_t prev = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        for (const auto& e : reader.scanSegment(segs[i], i + 1 == segs.size()).entries) {
            const uint64_t seq = e.header.seq;
            CHECK(seq > prev);
            prev = seq;
            const auto type = static_cast<vlog::EntryType>(e.header.type);
            if (type == vlog::EntryType::Tombstone) {
                cur.erase(e.key);
                out.tombstones++;
            } else if (type == vlog::EntryType::Full) {
                cur[e.key] = Cur{seq, std::string(e.payload.begin(), e.payload.end()), 0};
                out.fulls++;
                if (e.header.flags & vlog::kFlagConsolidated) out.consolidated++;
            } else {
                bpt::DecodedDelta d = bpt::decodeDelta(e.payload);
                auto it = cur.find(e.key);
                if (it == cur.end() || it->second.seq != d.prefix.base_seq ||
                    d.prefix.chain_depth != it->second.depth + 1) {
                    std::cerr << "Delta seq " << seq << " for " << e.key << " has base "
                              << d.prefix.base_seq << " depth " << d.prefix.chain_depth
                              << "; the key's previous entry is "
                              << (it == cur.end() ? std::string("absent")
                                                  : std::to_string(it->second.seq))
                              << "\n";
                    CHECK(false);
                }
                it->second = Cur{seq, applyToCopy(it->second.bytes, d.ops), d.prefix.chain_depth};
                out.deltas++;
            }
        }
    }
    for (auto& [k, c] : cur) out.docs[k] = std::move(c.bytes);
    return out;
}

static void checkStoreEqualsReplay(IndexedStore& st, const char* what) {
    Replayed r = replayVLog(st.vlogDir());
    bpt::BPTree::ScanResult keys = st.tree().scanFrom("", SIZE_MAX);
    if (keys.size() != r.docs.size())
        std::cerr << what << ": index has " << keys.size() << " keys, replay " << r.docs.size()
                  << "\n";
    CHECK(keys.size() == r.docs.size());
    for (const auto& [k, d] : r.docs) {
        std::optional<std::string> got = st.get(k);
        if (!got || *got != d) {
            std::cerr << what << ": key " << k << " get() = " << (got ? *got : "(none)")
                      << ", replay = " << d << "\n";
            CHECK(false);
        }
    }
}

// Expects f() to throw E and to leave every file under dir unchanged.
template <typename E, typename F>
static void expectErrorWritesNothing(IndexedStore& st, const std::string& dir, F&& f,
                                     const char* what) {
    const Snapshot before = snapshot(dir);
    const uint64_t vbytes = st.vlogBytesWritten(), applied = st.lastAppliedSeq();
    (void)expectThrow<E>(std::forward<F>(f), what);
    CHECK(st.vlogBytesWritten() == vbytes && st.lastAppliedSeq() == applied);
    if (snapshot(dir) != before) {
        std::cerr << what << ": files changed\n";
        CHECK(false);
    }
}

// ------------------------------------------------------------------------------------------
// P4.C6, C8-C10: update() behavior at the store
// ------------------------------------------------------------------------------------------
static void testC6NoOp(const std::string& dir) {
    banner("P4.C6 An update that changes nothing writes nothing");
    auto st = IndexedStore::open(dir, storeOpts());
    const std::string d = padDoc(1);
    const uint64_t s0 = st->put("k", d);
    const uint64_t vbytes = st->vlogBytesWritten();
    int noops = 0;
    for (const Ops& ops : {Ops{doc::set("n", Value::integer(0))}, Ops{doc::unset("missing")},
                           Ops{doc::unset("x.y.z")}, Ops{doc::inc("n", 0)},
                           Ops{doc::set("id", Value::integer(1)), doc::unset("nope")}}) {
        bpt::UpdateResult r = st->update("k", ops);
        CHECK(!r.written && r.seq == s0 && r.chain_depth == 0);
        ++noops;
    }
    CHECK(st->vlogBytesWritten() == vbytes);
    uint64_t seq = 0;
    CHECK(st->get("k", &seq) == d && seq == s0);
    // A real change after a delta, then a no-op on top of the chain.
    bpt::UpdateResult r1 = st->update("k", {doc::inc("n", 1)});
    CHECK(r1.written && r1.chain_depth == 1);
    bpt::UpdateResult r2 = st->update("k", {doc::set("n", Value::integer(1))});
    CHECK(!r2.written && r2.seq == r1.seq && r2.chain_depth == 1);
    // inc by 0.0 turns the int into a double: that is a change.
    CHECK(st->update("k", {doc::inc("n", 0.0)}).written);
    st->close();
    accountVLog(dir);
    pass("P4.C6 no-op update writes nothing, seq unchanged", std::to_string(noops) + " no-ops");
}

static void testC8NotFound(const std::string& dir) {
    banner("P4.C8 NotFoundError on missing and erased keys");
    auto st = IndexedStore::open(dir, storeOpts());
    expectErrorWritesNothing<doc::NotFoundError>(
        *st, dir, [&] { st->update("never", {doc::inc("n", 1)}); }, "missing key");
    st->put("k", padDoc(2));
    st->update("k", {doc::inc("n", 1)});
    st->erase("k");
    expectErrorWritesNothing<doc::NotFoundError>(
        *st, dir, [&] { st->update("k", {doc::inc("n", 1)}); }, "erased key");
    CHECK(!st->get("k"));
    st->close();
    accountVLog(dir);
    pass("P4.C8 NotFound on missing and erased keys (nothing written)");
}

static void testC9NotAnObject(const std::string& dir) {
    banner("P4.C9 TypeError when the stored value is not a JSON object");
    auto st = IndexedStore::open(dir, storeOpts());
    int n = 0;
    for (const std::string& stored :
         {std::string("hello"), std::string("\x00\x01\xFF", 3), std::string("[1,2,3]"),
          std::string("42"), std::string("\"str\""), std::string("null"), std::string("{bad"),
          std::string("{\"a\":1} trailing"), std::string()}) {
        const std::string key = "k" + std::to_string(n++);
        st->put(key, stored);
        expectErrorWritesNothing<doc::TypeError>(
            *st, dir, [&] { st->update(key, {doc::set("a", Value::integer(1))}); },
            "update of a non-object value");
        CHECK(st->get(key) == stored);
    }
    st->close();
    accountVLog(dir);
    pass("P4.C9 TypeError on raw bytes, JSON array, number", std::to_string(n) + " stored values");
}

static void testC10ErrorsWriteNothing(const std::string& dir) {
    banner("P4.C10 Every error case leaves the vLog and index unchanged");
    IndexedStore::Options o = storeOpts();
    o.max_document_bytes = 2048;
    auto st = IndexedStore::open(dir, o);
    st->put("k", "{\"s\":\"str\",\"n\":9223372036854775807,\"l\":[1,2],\"o\":{\"x\":1}}");
    st->put("raw", "not json");
    st->put("erased", "{}");
    st->erase("erased");
    int cases = 0;
    auto expectNone = [&](auto tag, auto&& f, const char* what) {
        using E = typename decltype(tag)::type;
        expectErrorWritesNothing<E>(*st, dir, f, what);
        ++cases;
    };
    auto deep = Value::array();
    for (int i = 0; i < 63; ++i) {
        Value outer = Value::array();
        outer.push(std::move(deep));
        deep = std::move(outer);
    }
    struct NF {
        using type = doc::NotFoundError;
    };
    struct TE {
        using type = doc::TypeError;
    };
    struct CE {
        using type = doc::ConflictError;
    };
    struct RE {
        using type = doc::RangeError;
    };
    struct PE {
        using type = doc::PathError;
    };
    struct IA {
        using type = std::invalid_argument;
    };
    expectNone(NF{}, [&] { st->update("missing", {doc::inc("n", 1)}); }, "missing key");
    expectNone(NF{}, [&] { st->update("erased", {doc::inc("n", 1)}); }, "erased key");
    expectNone(TE{}, [&] { st->update("raw", {doc::inc("n", 1)}); }, "raw bytes");
    expectNone(TE{}, [&] { st->update("k", {doc::set("s.x", Value())}); }, "field in a string");
    expectNone(TE{}, [&] { st->update("k", {doc::inc("s", 1)}); }, "$inc on a string");
    expectNone(TE{}, [&] { st->update("k", {doc::unset("l.0")}); }, "$unset of an element");
    expectNone(
        TE{}, [&] { st->update("k", {doc::inc("o.x", 1), doc::inc("s", 1)}); }, "second op fails");
    expectNone(
        CE{}, [&] { st->update("k", {doc::set("o", Value()), doc::set("o.x", Value())}); },
        "overlapping paths");
    expectNone(RE{}, [&] { st->update("k", {doc::inc("n", 1)}); }, "int64 overflow");
    expectNone(RE{}, [&] { st->update("k", {doc::set("l.2", Value())}); }, "index out of range");
    expectNone(
        RE{}, [&] { st->update("k", {doc::set("big", Value::string(std::string(3000, 'b')))}); },
        "document over max_document_bytes");
    expectNone(RE{}, [&] { st->update("k", {doc::set("deep", deep)}); }, "document deeper than 64");
    expectNone(PE{}, [&] { st->update("k", {doc::set("a..b", Value())}); }, "bad path");
    expectNone(PE{}, [&] { st->update("k", {doc::set("l.01", Value())}); }, "bad index");
    expectNone(IA{}, [&] { st->update("k", {}); }, "no ops");
    expectNone(IA{}, [&] { st->update("", {doc::inc("n", 1)}); }, "empty key");
    expectNone(IA{}, [&] { st->update(std::string(300, 'k'), {doc::inc("n", 1)}); }, "long key");
    st->close();
    accountVLog(dir);
    pass("P4.C10 every error case: vLog and index files identical before and after",
         std::to_string(cases) + " error cases");
}

// ------------------------------------------------------------------------------------------
// P4.D: chains
// ------------------------------------------------------------------------------------------
static void testD1D2Chain(const std::string& dir) {
    banner("P4.D1/D2 Chain depth 1..cap, then a consolidated Full entry");
    auto st = IndexedStore::open(dir, storeOpts());
    std::string oracle = padDoc(3);
    st->put("k", oracle);
    bpt::ReadStats rs;
    CHECK(st->get("k", nullptr, &rs) == oracle && rs.vlog_reads == 1 && rs.chain_depth == 0);
    for (uint16_t d = 1; d <= vlog::kMaxDeltaChain; ++d) {
        Ops ops{doc::inc("n", 1), doc::set("f" + std::to_string(d), Value::integer(d))};
        oracle = applyToCopy(oracle, ops);
        bpt::UpdateResult r = st->update("k", ops);
        CHECK(r.written && !r.consolidated && r.chain_depth == d);
        CHECK(st->get("k", nullptr, &rs) == oracle);
        CHECK(rs.chain_depth == d && rs.vlog_reads == d + 1u);
    }
    pass("P4.D1 depth 1..cap: correct doc, vlog_reads = depth + 1",
         "cap " + std::to_string(vlog::kMaxDeltaChain));
    oracle = applyToCopy(oracle, {doc::inc("n", 1)});
    bpt::UpdateResult r = st->update("k", {doc::inc("n", 1)});
    CHECK(r.written && r.consolidated && r.chain_depth == 0);
    const vlog::IndexValueRef ref = *st->tree().get("k");
    std::vector<uint8_t> payload;
    vlog::EntryHeader h = rawEntry(st->vlogDir(), ref.vptr, &payload);
    CHECK(h.type == static_cast<uint8_t>(vlog::EntryType::Full) &&
          (h.flags & vlog::kFlagConsolidated));
    CHECK(std::string(payload.begin(), payload.end()) == oracle);
    CHECK(st->get("k", nullptr, &rs) == oracle && rs.vlog_reads == 1 && rs.chain_depth == 0);
    // The next update starts a new chain on the consolidated entry.
    r = st->update("k", {doc::inc("n", 1)});
    oracle = applyToCopy(oracle, {doc::inc("n", 1)});
    CHECK(r.chain_depth == 1 && st->get("k", nullptr, &rs) == oracle && rs.vlog_reads == 2);
    checkStoreEqualsReplay(*st, "P4.D2");
    st.reset(); // crash: the chain is replayed at the next open
    st = IndexedStore::open(dir, storeOpts());
    CHECK(st->get("k") == oracle);
    st->close();
    accountVLog(dir);
    pass("P4.D2 update after cap deltas -> Full with kFlagConsolidated; depth resets");
}

static void testD3LargeDelta(const std::string& dir) {
    banner("P4.D3 A delta larger than half the document is written as a Full entry");
    auto st = IndexedStore::open(dir, storeOpts());
    std::string oracle = padDoc(4);
    st->put("k", oracle);
    bpt::UpdateResult r = st->update("k", {doc::inc("n", 1)});
    oracle = applyToCopy(oracle, {doc::inc("n", 1)});
    CHECK(r.chain_depth == 1 && !r.consolidated);
    Ops big{doc::set("big", Value::string(std::string(400, 'z')))};
    oracle = applyToCopy(oracle, big);
    r = st->update("k", big);
    CHECK(r.written && r.consolidated && r.chain_depth == 0);
    CHECK(st->get("k") == oracle);
    // A tiny document: even a one-field delta is more than half of it.
    st->put("tiny", "{\"n\":0}");
    r = st->update("tiny", {doc::inc("n", 1)});
    CHECK(r.consolidated && st->get("tiny") == "{\"n\":1}");
    st->close();
    accountVLog(dir);
    pass("P4.D3 delta > 50% of doc -> Full");
}

static void testD4ChainAcrossSegments(const std::string& dir) {
    banner("P4.D4 A chain whose entries are in different segments");
    IndexedStore::Options o = storeOpts();
    o.vlog_opts =
        vlog::VLogWriter::Options(8 * 1024, std::min<uint64_t>(4 << 20, remainingBudget()));
    auto st = IndexedStore::open(dir, o);
    std::string oracle = padDoc(5);
    st->put("chain", oracle);
    for (int i = 0; i < 4; ++i) {
        st->put("filler" + std::to_string(i), std::string(3000, 'f')); // fills segments
        st->put("filler" + std::to_string(i) + "b", std::string(3000, 'g'));
        oracle = applyToCopy(oracle, {doc::inc("n", 1)});
        CHECK(st->update("chain", {doc::inc("n", 1)}).chain_depth == i + 1);
    }
    // Walk the chain in the raw vLog and collect the segment of every entry.
    std::set<uint32_t> segs;
    vlog::VPtr at = st->tree().get("chain")->vptr;
    while (true) {
        segs.insert(at.segment_id);
        std::vector<uint8_t> pl;
        vlog::EntryHeader h = rawEntry(st->vlogDir(), at, &pl);
        if (h.type == static_cast<uint8_t>(vlog::EntryType::Full)) break;
        at = bpt::decodeDelta(pl).prefix.base;
    }
    CHECK(segs.size() >= 3);
    bpt::ReadStats rs;
    CHECK(st->get("chain", nullptr, &rs) == oracle && rs.vlog_reads == 5);
    st.reset();
    st = IndexedStore::open(dir, o);
    CHECK(st->get("chain") == oracle);
    st->close();
    accountVLog(dir);
    pass("P4.D4 chain spans segments", "5 entries in " + std::to_string(segs.size()) + " segments");
}

static void testD5NearMaxSize(const std::string& dir) {
    banner("P4.D5 Small update on a document near the size limit");
    // Quick mode uses a 64 KiB limit (max_document_bytes) to stay inside the write budget; full
    // mode uses the vLog's own limit (about 16 MiB).
    const size_t limit = kQuick ? 64 * 1024 : doc::kMaxDocBytes;
    IndexedStore::Options o = storeOpts();
    o.max_document_bytes = kQuick ? static_cast<uint32_t>(limit) : 0;
    o.vlog_opts = vlog::VLogWriter::Options(std::max<uint64_t>(1 << 20, 2 * limit + 65536),
                                            std::min<uint64_t>(64ULL << 20, remainingBudget()));
    auto st = IndexedStore::open(dir, o);
    const size_t overhead = std::strlen("{\"n\":0,\"pad\":\"\"}");
    std::string base = "{\"n\":0,\"pad\":\"" + std::string(limit - overhead - 10, 'p') + "\"}";
    CHECK(base.size() == limit - 10);
    expectThrow<std::invalid_argument>([&] { st->put("big", base + std::string(11, ' ')); },
                                       "put over the limit");
    st->put("big", base);
    std::string oracle = applyToCopy(base, {doc::inc("n", 7)});
    bpt::UpdateResult r = st->update("big", {doc::inc("n", 7)});
    CHECK(r.written && !r.consolidated && r.chain_depth == 1);
    CHECK(st->get("big") == oracle);
    expectErrorWritesNothing<doc::RangeError>(
        *st, dir, [&] { st->update("big", {doc::set("x", Value::string(std::string(20, 'x')))}); },
        "update past the limit");
    st->close();
    accountVLog(dir);
    pass("P4.D5 small update on a doc near the max entry size",
         std::to_string(limit) + "-byte limit");
}

static void testD6CapsAgree(const std::string& base_dir) {
    banner("P4.D6 Chain caps 0-4 give identical documents");
    CHECK(::mkdir(base_dir.c_str(), 0755) == 0);
    const int kOps = kQuick ? 30 : 120;
    std::map<std::string, std::string> first;
    std::string summary;
    for (uint16_t cap = 0; cap <= vlog::kMaxDeltaChain; ++cap) {
        const std::string dir = base_dir + "/cap" + std::to_string(cap);
        IndexedStore::Options o = storeOpts();
        o.delta_chain_cap = cap;
        auto st = IndexedStore::open(dir, o);
        std::mt19937 rng(66);
        for (int k = 0; k < 3; ++k) st->put("k" + std::to_string(k), padDoc(k));
        for (int i = 0; i < kOps; ++i) {
            const std::string key = "k" + std::to_string(rng() % 3);
            switch (rng() % 4) {
            case 0:
                st->update(key, {doc::inc("n", static_cast<int>(rng() % 10))});
                break;
            case 1:
                st->update(key, {doc::set("s" + std::to_string(rng() % 5), Value::integer(i))});
                break;
            case 2:
                st->update(key, {doc::unset("s" + std::to_string(rng() % 5))});
                break;
            default:
                st->update(key, {doc::inc("d.x", 0.25), doc::set("t", Value::boolean(i % 2))});
            }
        }
        std::map<std::string, std::string> docs;
        for (int k = 0; k < 3; ++k)
            docs["k" + std::to_string(k)] = *st->get("k" + std::to_string(k));
        if (cap == 0) first = docs;
        CHECK(docs == first);
        Replayed r = replayVLog(st->vlogDir());
        CHECK(r.docs == docs);
        if (cap == 0) CHECK(r.deltas == 0);
        summary += (summary.empty() ? "" : ", ") + std::string("cap ") + std::to_string(cap) +
                   ": " + std::to_string(r.deltas) + " deltas";
        st->close();
        accountVLog(dir);
    }
    pass("P4.D6 caps 0/1/2/3/4 (4 = max) give identical docs", summary);
}

static void testD7FullByteForByte(const std::string& dir) {
    banner("P4.D7 get() returns a Full entry from put() byte for byte");
    auto st = IndexedStore::open(dir, storeOpts());
    int n = 0;
    for (const std::string& v :
         {std::string(" { \"a\" : 1 ,\n \"b\" : [ 1 , 2 ] } "), std::string("{\"a\":1.50}"),
          std::string("not json at all"), std::string("\x00\x01\x02\xFE\xFF", 5), std::string(),
          std::string("{\"\\u0041\":\"\\u00e9\"}")}) {
        const std::string key = "k" + std::to_string(n++);
        st->put(key, v);
        bpt::ReadStats rs;
        CHECK(st->get(key, nullptr, &rs) == v && rs.vlog_reads == 1);
    }
    // After an update, the document comes back in canonical form.
    st->update("k0", {doc::inc("a", 1)});
    CHECK(st->get("k0") == "{\"a\":2,\"b\":[1,2]}");
    st->close();
    accountVLog(dir);
    pass("P4.D7 a Full entry from put() is returned byte-for-byte by get()",
         std::to_string(n) + " values");
}

// ------------------------------------------------------------------------------------------
// P4.E: mixed sequences
// ------------------------------------------------------------------------------------------
static void testE1PutUpdateUpdate(const std::string& dir) {
    banner("P4.E1 put -> update -> update -> get");
    auto st = IndexedStore::open(dir, storeOpts());
    std::string oracle = padDoc(7);
    const uint64_t s0 = st->put("k", oracle);
    Ops u1{doc::set("profile.score", Value::integer(10)), doc::inc("n", 1)};
    Ops u2{doc::inc("profile.score", 5), doc::set("tags", doc::parse("[\"a\",\"b\"]"))};
    bpt::UpdateResult r1 = st->update("k", u1);
    bpt::UpdateResult r2 = st->update("k", u2);
    oracle = applyToCopy(applyToCopy(oracle, u1), u2);
    uint64_t seq = 0;
    CHECK(r1.seq == s0 + 1 && r2.seq == s0 + 2 && st->get("k", &seq) == oracle && seq == r2.seq);
    CHECK(doc::parse(oracle).find("profile")->find("score")->asInt() == 15);
    st->close();
    accountVLog(dir);
    pass("P4.E1 put -> update -> update -> get");
}

static void testE2EraseAndPut(const std::string& dir) {
    banner("P4.E2 erase -> update = NotFound; erase -> put -> update uses the new put");
    auto st = IndexedStore::open(dir, storeOpts());
    st->put("k", padDoc(8));
    st->update("k", {doc::set("old", Value::boolean(true))});
    st->erase("k");
    expectThrow<doc::NotFoundError>([&] { st->update("k", {doc::inc("n", 1)}); }, "after erase");
    const std::string fresh = padDoc(9);
    const uint64_t put_seq = st->put("k", fresh);
    bpt::UpdateResult r = st->update("k", {doc::inc("n", 1)});
    CHECK(r.chain_depth == 1);
    std::vector<uint8_t> pl;
    rawEntry(st->vlogDir(), st->tree().get("k")->vptr, &pl);
    CHECK(bpt::decodeDelta(pl).prefix.base_seq == put_seq);
    const std::string got = *st->get("k");
    CHECK(got == applyToCopy(fresh, {doc::inc("n", 1)}));
    CHECK(got.find("old") == std::string::npos);
    checkStoreEqualsReplay(*st, "P4.E2");
    st->close();
    accountVLog(dir);
    pass("P4.E2 erase -> update = NotFound; erase -> put -> update uses the new put as base");
}

static void testE3RawThenJson(const std::string& dir) {
    banner("P4.E3 raw bytes -> update = TypeError -> put JSON -> update ok");
    auto st = IndexedStore::open(dir, storeOpts());
    st->put("k", "raw \x01 bytes");
    expectThrow<doc::TypeError>([&] { st->update("k", {doc::inc("n", 1)}); }, "raw bytes");
    st->put("k", padDoc(10));
    CHECK(st->update("k", {doc::inc("n", 1)}).written);
    CHECK(st->get("k") == applyToCopy(padDoc(10), {doc::inc("n", 1)}));
    st->close();
    accountVLog(dir);
    pass("P4.E3 put raw bytes -> update = TypeError -> put JSON -> update ok");
}

// ------------------------------------------------------------------------------------------
// P4.F: concurrency
// ------------------------------------------------------------------------------------------
static void testF1SameKeyIncs(const std::string& dir) {
    banner("P4.F1 8 threads increment one key");
    const int kThreads = 8, kPer = kQuick ? 100 : 1000;
    auto st = IndexedStore::open(dir, storeOpts());
    st->put("ctr", padDoc(11));
    std::vector<std::thread> th;
    std::atomic<int> written{0};
    for (int t = 0; t < kThreads; ++t)
        th.emplace_back([&] {
            for (int i = 0; i < kPer; ++i)
                if (st->update("ctr", {doc::inc("n", 1)}).written) written++;
        });
    for (auto& t : th) t.join();
    const int64_t n = doc::parse(*st->get("ctr")).find("n")->asInt();
    CHECK(n == kThreads * kPer && written == kThreads * kPer);
    checkStoreEqualsReplay(*st, "P4.F1");
    Replayed r = replayVLog(st->vlogDir());
    st->close();
    accountVLog(dir);
    pass("P4.F1 " + std::to_string(kThreads) + " threads x " + std::to_string(kPer) +
             " $inc on one key -> exactly " + std::to_string(kThreads * kPer),
         std::to_string(r.deltas) + " deltas, " + std::to_string(r.consolidated) + " consolidated");
}

static void testF2UpdateAndPut(const std::string& dir) {
    banner("P4.F2 Concurrent update and put on one key");
    const int kUpd = kQuick ? 60 : 300, kPut = kQuick ? 20 : 100;
    auto st = IndexedStore::open(dir, storeOpts());
    st->put("k", padDoc(12));
    std::vector<std::thread> th;
    for (int t = 0; t < 2; ++t)
        th.emplace_back([&, t] {
            for (int i = 0; i < kUpd; ++i)
                st->update(
                    "k", {doc::inc("n", 1), doc::set("u" + std::to_string(t), Value::integer(i))});
        });
    for (int t = 0; t < 2; ++t)
        th.emplace_back([&, t] {
            for (int i = 0; i < kPut; ++i) st->put("k", padDoc(1000 * (t + 1) + i));
        });
    for (auto& t : th) t.join();
    checkStoreEqualsReplay(*st, "P4.F2");
    st->close();
    accountVLog(dir);
    pass("P4.F2 concurrent update + put on one key: final state == seq-order vLog replay");
}

static void testF3UpdateAndErase(const std::string& dir) {
    banner("P4.F3 Concurrent update and erase on one key");
    const int kUpd = kQuick ? 60 : 300, kCycles = kQuick ? 15 : 80;
    auto st = IndexedStore::open(dir, storeOpts());
    st->put("k", padDoc(13));
    std::atomic<int> not_found{0}, written{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 2; ++t)
        th.emplace_back([&] {
            for (int i = 0; i < kUpd; ++i) {
                try {
                    if (st->update("k", {doc::inc("n", 1)}).written) written++;
                } catch (const doc::NotFoundError&) {
                    not_found++;
                }
            }
        });
    th.emplace_back([&] {
        for (int i = 0; i < kCycles; ++i) {
            st->erase("k");
            st->put("k", padDoc(i));
        }
    });
    for (auto& t : th) t.join();
    // replayVLog checks that every Delta's base is the key's previous entry, never a tombstone.
    Replayed r = replayVLog(st->vlogDir());
    CHECK(r.deltas + r.consolidated == static_cast<uint64_t>(written.load()));
    CHECK(r.tombstones == static_cast<uint64_t>(kCycles));
    checkStoreEqualsReplay(*st, "P4.F3");
    st->close();
    accountVLog(dir);
    pass("P4.F3 concurrent update + erase on one key: no update applies to an erased base",
         std::to_string(written.load()) + " updates, " + std::to_string(not_found.load()) +
             " NotFound");
}

static void testF4NoPartialDocuments(const std::string& dir) {
    banner("P4.F4 Parallel updates on many keys with concurrent gets");
    const int kWriters = 4, kKeysPer = 4, kRounds = kQuick ? 12 : 60;
    auto st = IndexedStore::open(dir, storeOpts());
    for (int k = 0; k < kWriters * kKeysPer; ++k) {
        Value v = doc::parse(padDoc(k));
        v.set("a", Value::integer(-1));
        v.set("b", Value::integer(-1));
        st->put("k" + std::to_string(k), doc::serialize(v));
    }
    std::atomic<bool> done{false};
    std::atomic<uint64_t> reads{0};
    std::vector<std::thread> th;
    for (int w = 0; w < kWriters; ++w)
        th.emplace_back([&, w] {
            for (int i = 0; i < kRounds; ++i)
                for (int k = 0; k < kKeysPer; ++k)
                    st->update("k" + std::to_string(w * kKeysPer + k),
                               {doc::set("a", Value::integer(i)), doc::set("b", Value::integer(i)),
                                doc::inc("n", 1)});
        });
    for (int r = 0; r < 2; ++r)
        th.emplace_back([&, r] {
            std::mt19937 rng(77 + r);
            while (!done.load()) {
                std::optional<std::string> d =
                    st->get("k" + std::to_string(rng() % (kWriters * kKeysPer)));
                CHECK(d.has_value());
                Value v = doc::parse(*d);
                const int64_t a = v.find("a")->asInt(), b = v.find("b")->asInt();
                const int64_t n = v.find("n")->asInt();
                CHECK(a == b && n == a + 1);
                reads++;
            }
        });
    for (int w = 0; w < kWriters; ++w) th[w].join();
    done = true;
    for (size_t i = kWriters; i < th.size(); ++i) th[i].join();
    for (int k = 0; k < kWriters * kKeysPer; ++k)
        CHECK(doc::parse(*st->get("k" + std::to_string(k))).find("n")->asInt() == kRounds);
    st->close();
    accountVLog(dir);
    pass("P4.F4 parallel updates on many keys + concurrent gets: no partial document returned",
         std::to_string(reads.load()) + " concurrent gets");
}

static void testF5CheckpointsDuringUpdates(const std::string& dir) {
    banner("P4.F5 Checkpoints while updates run");
    const int kThreads = 4, kKeysPer = 2, kRounds = kQuick ? 20 : 100, kEvery = 25;
    auto st = IndexedStore::open(dir, storeOpts(32));
    std::vector<std::string> oracle(kThreads * kKeysPer);
    for (int k = 0; k < kThreads * kKeysPer; ++k) {
        oracle[k] = padDoc(k);
        st->put("k" + std::to_string(k), oracle[k]);
    }
    std::atomic<int> updates{0};
    std::vector<std::thread> th;
    for (int t = 0; t < kThreads; ++t)
        th.emplace_back([&, t] {
            for (int i = 0; i < kRounds; ++i)
                for (int k = t * kKeysPer; k < (t + 1) * kKeysPer; ++k) {
                    Ops ops{doc::inc("n", 1), doc::set("w", Value::integer(i))};
                    st->update("k" + std::to_string(k), ops);
                    oracle[k] = applyToCopy(oracle[k], ops); // each thread owns its keys
                    updates++;
                }
        });
    const int total = kThreads * kKeysPer * kRounds;
    int checkpoints = 0;
    std::thread cp([&] {
        for (int next = kEvery; next < total; next += kEvery) {
            while (updates.load() < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            st->checkpoint();
            ++checkpoints;
        }
    });
    for (auto& t : th) t.join();
    cp.join();
    for (int k = 0; k < kThreads * kKeysPer; ++k)
        CHECK(st->get("k" + std::to_string(k)) == oracle[k]);
    st.reset(); // crash after the last checkpoint: recovery replays the rest
    st = IndexedStore::open(dir, storeOpts(32));
    CHECK(!st->openInfo().rebuilt);
    for (int k = 0; k < kThreads * kKeysPer; ++k)
        CHECK(st->get("k" + std::to_string(k)) == oracle[k]);
    checkStoreEqualsReplay(*st, "P4.F5");
    st->close();
    accountVLog(dir);
    pass("P4.F5 checkpoints during updates: no deadlock, correct final state",
         std::to_string(checkpoints) + " checkpoints during " + std::to_string(total) + " updates");
}

// ------------------------------------------------------------------------------------------
// P4.G5, G6: replay rule
// ------------------------------------------------------------------------------------------
static void testG5RecoverTwiceAndRebuild(const std::string& dir) {
    banner("P4.G5 Recover twice, then full rebuild: identical state");
    std::map<std::string, vlog::IndexValueRef> base;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 6; ++i)
            st->put("k" + std::to_string(i), "{\"i\":" + std::to_string(i) + ",\"n\":0}");
        for (int i = 0; i < 6; ++i)
            base["k" + std::to_string(i)] = *st->tree().get("k" + std::to_string(i));
        st->close(); // checkpoint: the deltas below are all after the replay point
    }
    std::map<std::string, uint64_t> expect_seq;
    {
        vlog::VLogWriter w(dir + "/vlog", writerOpts());
        // k0: chain of 3, k1: chain of 1, k2: chain of 2 interleaved with k0.
        std::map<std::string, uint16_t> depth;
        auto chain = [&](const std::string& k, int n) {
            for (int i = 0; i < n; ++i) {
                vlog::AppendResult ar =
                    appendDelta(w, k, base[k].seq, base[k].vptr, ++depth[k], {doc::inc("n", 1)});
                base[k] = vlog::IndexValueRef{ar.seq, 1, {0, 0, 0}, ar.vptr};
                expect_seq[k] = ar.seq;
            }
        };
        chain("k0", 2);
        chain("k2", 2);
        chain("k0", 1);
        chain("k1", 1);
    }
    IndexState s1, s2, s3;
    uint64_t applied1 = 0, applied2 = 0;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(!st->openInfo().rebuilt);
        applied1 = st->openInfo().vlog_entries_applied;
        s1 = indexState(*st);
        // st is dropped without close(): a crash, so the next open replays the same tail again.
    }
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(!st->openInfo().rebuilt);
        applied2 = st->openInfo().vlog_entries_applied;
        s2 = indexState(*st);
    }
    CHECK(::unlink((dir + "/superblock.db").c_str()) == 0);
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(st->openInfo().rebuilt);
        s3 = indexState(*st);
        st->close();
    }
    CHECK(applied1 == 6 && applied2 == 6);
    CHECK(sameState(s1, s2) && sameState(s1, s3));
    for (const auto& [k, seq] : expect_seq) CHECK(s1.at(k).first == seq);
    CHECK(s1.at("k3").first == base["k3"].seq);
    accountVLog(dir);
    pass("P4.G5 recover twice + full rebuild -> identical state",
         std::to_string(applied1) + " deltas replayed each time");
}

static void testG6BaseMismatch(const std::string& base_dir) {
    banner("P4.G6 Delta with a wrong base_seq: VLogCorruptError on open, files unchanged");
    CHECK(::mkdir(base_dir.c_str(), 0755) == 0);
    int cases = 0;
    for (int variant = 0; variant < 3; ++variant) {
        // 0: wrong base_seq (replay path); 1: delta for an absent key (replay path);
        // 2: wrong base_seq with superblock.db deleted (full-rebuild path).
        const std::string dir = base_dir + "/v" + std::to_string(variant);
        vlog::IndexValueRef a{}, b{};
        {
            auto st = IndexedStore::open(dir, storeOpts());
            st->put("a", "{\"x\":1}");
            st->put("b", "{\"y\":1}");
            a = *st->tree().get("a");
            b = *st->tree().get("b");
            st->close();
        }
        {
            vlog::VLogWriter w(dir + "/vlog", writerOpts());
            appendDelta(w, "b", b.seq, b.vptr, 1, {doc::inc("y", 1)}); // valid, applied first
            if (variant == 1)
                appendDelta(w, "zz", a.seq, a.vptr, 1, {doc::inc("x", 1)});
            else
                appendDelta(w, "a", a.seq - 1, a.vptr, 1, {doc::inc("x", 1)});
        }
        if (variant == 2) CHECK(::unlink((dir + "/superblock.db").c_str()) == 0);
        const Snapshot before = snapshot(dir);
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::string msg = expectThrow<bpt::VLogCorruptError>(
                [&] { (void)IndexedStore::open(dir, storeOpts()); }, "P4.G6 open");
            CHECK(msg.find(variant == 1 ? "absent" : "base seq") != std::string::npos);
            const Snapshot after = snapshot(dir);
            if (after != before) {
                for (const auto& [f, bytes] : after)
                    if (!before.count(f) || before.at(f) != bytes)
                        std::cerr << "changed: " << f << "\n";
                for (const auto& [f, bytes] : before)
                    if (!after.count(f)) std::cerr << "removed: " << f << "\n";
            }
            CHECK(after == before);
        }
        accountVLog(dir);
        ++cases;
    }
    pass("P4.G6 base_seq mismatch on replay -> VLogCorruptError on open; nothing modified",
         std::to_string(cases) + " cases (wrong base, absent key, rebuild path), 2 opens each");
}

int main() {
    std::string dir = setupRun("delta_test");
    Cleaner cleaner{dir};
    testC6NoOp(dir + "/c6");
    testC8NotFound(dir + "/c8");
    testC9NotAnObject(dir + "/c9");
    testC10ErrorsWriteNothing(dir + "/c10");
    testD1D2Chain(dir + "/d1");
    testD3LargeDelta(dir + "/d3");
    testD4ChainAcrossSegments(dir + "/d4");
    testD5NearMaxSize(dir + "/d5");
    testD6CapsAgree(dir + "/d6");
    testD7FullByteForByte(dir + "/d7");
    testE1PutUpdateUpdate(dir + "/e1");
    testE2EraseAndPut(dir + "/e2");
    testE3RawThenJson(dir + "/e3");
    testF1SameKeyIncs(dir + "/f1");
    testF2UpdateAndPut(dir + "/f2");
    testF3UpdateAndErase(dir + "/f3");
    testF4NoPartialDocuments(dir + "/f4");
    testF5CheckpointsDuringUpdates(dir + "/f5");
    testG5RecoverTwiceAndRebuild(dir + "/g5");
    testG6BaseMismatch(dir + "/g6");
    printBudget();
    return 0;
}
