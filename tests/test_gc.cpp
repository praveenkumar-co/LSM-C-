// Phase 5 tests: vLog garbage collection (docs/PHASE5_PLAN.md). Relocated entries in replay and
// rebuild, segment-id gaps and the vLog identity anchor (P5.R, P5.G), counters (P5.A).
#include "test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/DeltaCodec.hpp"
#include "index/IndexRecovery.hpp"
#include "index/IndexedStore.hpp"
#include "index/SuperblockManager.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace testutil;
using bpt::IndexedStore;
using doc::Value;
using Ops = std::vector<doc::UpdateOp>;
using Snapshot = std::map<std::string, std::string>; // relative path -> file bytes

// ------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------
// seg_limit 8 KiB: one ~3 KB document per segment, so tests control which segment holds what.
static IndexedStore::Options storeOpts(uint32_t seg_limit = 8 * 1024) {
    IndexedStore::Options o;
    o.cache_frames = 64;
    o.vlog_opts = vlog::VLogWriter::Options(
        seg_limit, std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index.db / superblock.db write"); };
    o.auto_checkpoint = false;
    return o;
}

static vlog::VLogWriter::Options writerOpts(uint32_t seg_limit = 8 * 1024) {
    return vlog::VLogWriter::Options(seg_limit,
                                     std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
}

static std::string segPath(const std::string& vdir, uint32_t id) {
    char name[32];
    std::snprintf(name, sizeof(name), "/vlog_%08u.log", id);
    return vdir + name;
}

static uint64_t fileSize(const std::string& path) {
    struct stat st;
    CHECK(::stat(path.c_str(), &st) == 0);
    return static_cast<uint64_t>(st.st_size);
}

// vLog bytes are counted once per test from the segment files (index writes are counted live).
// Segments a test unlinked are added by the test itself (countUnlinked).
static void accountVLog(const std::string& dir) {
    uint64_t total = 0;
    for (uint32_t id : bpt::listVLogSegments(dir + "/vlog"))
        total += fileSize(segPath(dir + "/vlog", id));
    accountBytes(total, "vLog segments");
}
static void countUnlinked(const std::string& vdir, uint32_t id) {
    accountBytes(fileSize(segPath(vdir, id)), "vLog segment removed by the test");
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

// Canonical JSON document of about `pad` + 60 bytes.
static std::string padDoc(int id, size_t pad = 240) {
    Value v = Value::object();
    v.set("id", Value::integer(id));
    v.set("n", Value::integer(0));
    v.set("name", Value::string("doc" + std::to_string(id)));
    v.set("pad", Value::string(std::string(pad, static_cast<char>('a' + id % 26))));
    return doc::serialize(v);
}
static std::string bigDoc(int id) { return padDoc(id, 2900); } // fills one 8 KiB segment

static vlog::EntryHeader rawEntry(const std::string& vdir, const vlog::VPtr& p,
                                  std::vector<uint8_t>* payload = nullptr,
                                  std::string* key = nullptr) {
    vlog::VLogReader r(vdir);
    vlog::EntryHeader h;
    std::string k;
    std::vector<uint8_t> pl;
    CHECK(r.readEntry(p, h, k, pl));
    if (payload) *payload = std::move(pl);
    if (key) *key = k;
    return h;
}

// A GC copy of the entry at p (same key, payload, seq; flags | extra).
static vlog::VLogWriter::RelocatedEntry copyOf(const std::string& vdir, const vlog::VPtr& p,
                                               uint8_t extra = vlog::kFlagRelocated) {
    std::vector<uint8_t> payload;
    std::string key;
    vlog::EntryHeader h = rawEntry(vdir, p, &payload, &key);
    vlog::VLogWriter::RelocatedEntry r;
    r.type = static_cast<vlog::EntryType>(h.type);
    r.key = key;
    r.seq = h.seq;
    r.flags = static_cast<uint8_t>(h.flags | extra);
    r.raw_len = h.raw_len;
    r.payload = std::move(payload);
    return r;
}

// Independent oracle (plan section 5.2): per key the highest seq wins; on an equal seq the rank
// decides (consolidated Full > relocated Full > Full > Delta). A winning tombstone means absent.
// A winning Delta is materialized by walking its chain. Returns key -> bytes get() must return.
static int oracleRank(uint8_t type, uint8_t flags) {
    if (type == static_cast<uint8_t>(vlog::EntryType::Delta)) return 0;
    if (type == static_cast<uint8_t>(vlog::EntryType::Full) && (flags & vlog::kFlagConsolidated))
        return 3;
    return (flags & vlog::kFlagRelocated) ? 2 : 1;
}
static std::map<std::string, std::string> replayWinners(const std::string& vdir) {
    struct W {
        uint64_t seq;
        int rank;
        uint8_t type;
        vlog::VPtr vptr;
    };
    std::map<std::string, W> best;
    std::vector<uint32_t> segs = bpt::listVLogSegments(vdir);
    vlog::VLogReader reader(vdir);
    for (size_t i = 0; i < segs.size(); ++i) {
        for (const auto& e : reader.scanSegment(segs[i], i + 1 == segs.size()).entries) {
            const uint64_t seq = e.header.seq;
            const int rank = oracleRank(e.header.type, e.header.flags);
            auto it = best.find(e.key);
            if (it == best.end() || seq > it->second.seq ||
                (seq == it->second.seq && rank > it->second.rank))
                best[e.key] = W{seq, rank, e.header.type, e.vptr};
        }
    }
    std::map<std::string, std::string> docs;
    for (const auto& [k, w] : best) {
        if (w.type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) continue;
        std::vector<std::vector<doc::UpdateOp>> chain; // newest first
        vlog::VPtr at = w.vptr;
        uint64_t want = w.seq;
        while (true) {
            std::vector<uint8_t> pl;
            std::string key;
            vlog::EntryHeader h = rawEntry(vdir, at, &pl, &key);
            CHECK(key == k && h.seq == want);
            if (h.type == static_cast<uint8_t>(vlog::EntryType::Full)) {
                std::string full(pl.begin(), pl.end());
                if (chain.empty()) {
                    docs[k] = full;
                } else {
                    Value v = doc::parse(full);
                    for (auto it = chain.rbegin(); it != chain.rend(); ++it) doc::applyOps(v, *it);
                    docs[k] = doc::serialize(v);
                }
                break;
            }
            CHECK(h.type == static_cast<uint8_t>(vlog::EntryType::Delta));
            bpt::DecodedDelta d = bpt::decodeDelta(pl);
            chain.push_back(d.ops);
            want = d.prefix.base_seq;
            at = d.prefix.base;
        }
    }
    return docs;
}

// The store's documents equal the oracle's, key for key.
static void checkStoreEqualsOracle(IndexedStore& st, const char* what) {
    std::map<std::string, std::string> want = replayWinners(st.vlogDir());
    bpt::BPTree::ScanResult keys = st.tree().scanFrom("", SIZE_MAX);
    if (keys.size() != want.size())
        std::cerr << what << ": index has " << keys.size() << " keys, oracle " << want.size()
                  << "\n";
    CHECK(keys.size() == want.size());
    for (const auto& [k, d] : want) {
        std::optional<std::string> got = st.get(k);
        if (!got || *got != d) {
            std::cerr << what << ": key " << k << " get() = " << (got ? got->substr(0, 80) : "(none)")
                      << ", oracle = " << d.substr(0, 80) << "\n";
            CHECK(false);
        }
    }
}

static void removeSuperblock(const std::string& dir) {
    CHECK(::unlink((dir + "/superblock.db").c_str()) == 0);
}

// ------------------------------------------------------------------------------------------
// P5.R1 appendRelocated: validation writes nothing; a batch is one fsync; seqs are kept
// ------------------------------------------------------------------------------------------
static void testR1AppendRelocated(const std::string& dir) {
    banner("P5.R1 appendRelocated");
    {
        auto st = IndexedStore::open(dir, storeOpts(64 * 1024));
        for (int i = 0; i < 3; ++i) st->put("k" + std::to_string(i), padDoc(i));
        st->close();
    }
    std::vector<vlog::VPtr> orig;
    {
        auto st = IndexedStore::open(dir, storeOpts(64 * 1024));
        for (int i = 0; i < 3; ++i) orig.push_back(st->tree().get("k" + std::to_string(i))->vptr);
        st->close();
    }
    const std::string vdir = dir + "/vlog";
    vlog::VLogWriter w(vdir, writerOpts(64 * 1024));
    const uint64_t seq0 = w.getCurrentSeq();
    const Snapshot before = snapshot(dir);
    auto bad_flag = copyOf(vdir, orig[0], 0);
    bad_flag.flags = 0;
    expectThrow<std::invalid_argument>([&] { w.appendRelocated({bad_flag}); }, "no flag");
    auto bad_seq = copyOf(vdir, orig[0]);
    bad_seq.seq = seq0 + 1;
    expectThrow<std::invalid_argument>([&] { w.appendRelocated({copyOf(vdir, orig[1]), bad_seq}); },
                                       "seq above the last assigned");
    auto zero = copyOf(vdir, orig[0]);
    zero.seq = 0;
    expectThrow<std::invalid_argument>([&] { w.appendRelocated({zero}); }, "seq 0");
    CHECK(snapshot(dir) == before); // nothing written by a rejected batch

    const uint64_t fsyncs0 = w.getTotalFsyncs();
    std::vector<vlog::VLogWriter::RelocatedEntry> batch;
    for (const auto& p : orig) batch.push_back(copyOf(vdir, p));
    std::vector<vlog::VPtr> got = w.appendRelocated(batch);
    CHECK(got.size() == 3);
    CHECK(w.getTotalFsyncs() == fsyncs0 + 1); // one fsync for the whole batch
    CHECK(w.getCurrentSeq() == seq0);         // no new seq consumed
    for (size_t i = 0; i < got.size(); ++i) {
        std::vector<uint8_t> a, b;
        vlog::EntryHeader ho = rawEntry(vdir, orig[i], &a);
        vlog::EntryHeader hc = rawEntry(vdir, got[i], &b);
        CHECK(hc.seq == ho.seq && a == b && hc.flags == (ho.flags | vlog::kFlagRelocated));
    }
    // A normal append after the batch still gets the next seq.
    CHECK(w.append(vlog::EntryType::Full, "k9", "{}", 2).seq == seq0 + 1);
    accountVLog(dir);
    pass("P5.R1 appendRelocated: bad batches write nothing; 3 copies in 1 fsync keep their seqs",
         "3 rejected batches, 1 accepted");
}

// ------------------------------------------------------------------------------------------
// P5.R2 A relocated copy after the replay point is applied by replay; after the original's
// segment is removed, a full rebuild finds the copy
// ------------------------------------------------------------------------------------------
static void testR2CopyReplacesOriginal(const std::string& dir) {
    banner("P5.R2 relocated copy: replay applies it, rebuild keeps it");
    const std::string vdir = dir + "/vlog";
    std::map<std::string, std::string> docs;
    vlog::IndexValueRef ref1{};
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 4; ++i) {
            docs["k" + std::to_string(i)] = bigDoc(i);
            st->put("k" + std::to_string(i), docs["k" + std::to_string(i)]);
        }
        ref1 = *st->tree().get("k1");
        st->close();
    }
    {
        vlog::VLogWriter w(vdir, writerOpts());
        w.appendRelocated({copyOf(vdir, ref1.vptr)});
    }
    uint32_t copy_seg = 0;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(!st->openInfo().rebuilt);
        CHECK(st->openInfo().vlog_entries_applied == 1); // the copy (its seq is below the cp seq)
        vlog::IndexValueRef r = *st->tree().get("k1");
        CHECK(r.seq == ref1.seq && r.vptr.segment_id != ref1.vptr.segment_id);
        copy_seg = r.vptr.segment_id;
        uint64_t seq = 0;
        CHECK(st->get("k1", &seq) == docs["k1"] && seq == ref1.seq);
        checkStoreEqualsOracle(*st, "P5.R2 after replay");
        st->close();
    }
    // The original's segment holds only k1's old version: remove it (as GC would), then rebuild.
    countUnlinked(vdir, ref1.vptr.segment_id);
    CHECK(::unlink(segPath(vdir, ref1.vptr.segment_id).c_str()) == 0);
    removeSuperblock(dir);
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(st->openInfo().rebuilt);
        CHECK(st->tree().get("k1")->vptr.segment_id == copy_seg);
        for (const auto& [k, d] : docs) CHECK(st->get(k) == d);
        checkStoreEqualsOracle(*st, "P5.R2 after rebuild");
        st->close();
    }
    accountVLog(dir);
    pass("P5.R2 relocated copy: replay applies it (same seq, bytes); rebuild keeps it after the "
         "original's segment is gone");
}

// ------------------------------------------------------------------------------------------
// P5.R3 Order independence: a relocated copy placed after the key's tombstone in the file never
// brings the key back while the tombstone exists (replay and rebuild)
// ------------------------------------------------------------------------------------------
static void testR3TombstoneBeatsLaterCopy(const std::string& dir) {
    banner("P5.R3 tombstone beats a copy written after it");
    const std::string vdir = dir + "/vlog";
    vlog::IndexValueRef ref{};
    {
        auto st = IndexedStore::open(dir, storeOpts());
        st->put("gone", bigDoc(1));
        st->put("kept", bigDoc(2));
        ref = *st->tree().get("gone");
        CHECK(st->erase("gone").existed);
        st->close();
    }
    {
        vlog::VLogWriter w(vdir, writerOpts());
        w.appendRelocated({copyOf(vdir, ref.vptr)}); // the order the stripe lock rules out
    }
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(!st->openInfo().rebuilt && !st->get("gone") && st->get("kept") == bigDoc(2));
        CHECK(st->openInfo().vlog_entries_applied == 0); // relocation rule: key absent
        st->close();
    }
    removeSuperblock(dir);
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(st->openInfo().rebuilt && !st->get("gone") && st->get("kept") == bigDoc(2));
        checkStoreEqualsOracle(*st, "P5.R3 rebuild");
        st->close();
    }
    accountVLog(dir);
    pass("P5.R3 copy after the tombstone in file order: replay and rebuild keep the key absent");
}

// ------------------------------------------------------------------------------------------
// P5.R4 A consolidated copy (head seq) beats the Delta with the same seq; with the base's
// segment removed, rebuild still returns the document. P5.G4: without the copy, the same removal
// is corruption: VLogCorruptError, files unchanged
// ------------------------------------------------------------------------------------------
static void testR4ConsolidatedCopy(const std::string& base_dir) {
    banner("P5.R4 consolidated copy beats an equal-seq Delta; P5.G4 broken chain");
    CHECK(::mkdir(base_dir.c_str(), 0755) == 0);
    for (int variant = 0; variant < 2; ++variant) { // 0: with the copy, 1: without
        const std::string dir = base_dir + "/v" + std::to_string(variant);
        const std::string vdir = dir + "/vlog";
        std::string want;
        vlog::IndexValueRef base{}, head{};
        {
            auto st = IndexedStore::open(dir, storeOpts());
            st->put("k", padDoc(7, 2900));
            base = *st->tree().get("k");
            st->put("filler", bigDoc(8)); // moves the writer past the base's segment
            CHECK(st->update("k", {doc::inc("n", 1)}).chain_depth == 1);
            CHECK(st->update("k", {doc::set("name", Value::string("x"))}).chain_depth == 2);
            head = *st->tree().get("k");
            want = *st->get("k");
            st->close();
        }
        CHECK(head.vptr.segment_id != base.vptr.segment_id);
        if (variant == 0) {
            vlog::VLogWriter w(vdir, writerOpts());
            vlog::VLogWriter::RelocatedEntry c;
            c.type = vlog::EntryType::Full;
            c.key = "k";
            c.seq = head.seq;
            c.flags = vlog::kFlagRelocated | vlog::kFlagConsolidated;
            c.raw_len = 0;
            c.payload.assign(want.begin(), want.end());
            w.appendRelocated({c});
        }
        countUnlinked(vdir, base.vptr.segment_id);
        CHECK(::unlink(segPath(vdir, base.vptr.segment_id).c_str()) == 0);
        removeSuperblock(dir);
        if (variant == 0) {
            auto st = IndexedStore::open(dir, storeOpts());
            CHECK(st->openInfo().rebuilt);
            bpt::ReadStats rs;
            uint64_t seq = 0;
            CHECK(st->get("k", &seq, &rs) == want && seq == head.seq && rs.chain_depth == 0);
            CHECK(st->get("filler") == bigDoc(8));
            checkStoreEqualsOracle(*st, "P5.R4 rebuild");
            // update() continues from the consolidated copy.
            CHECK(st->update("k", {doc::inc("n", 5)}).chain_depth == 1);
            Value v = doc::parse(want);
            doc::applyOps(v, {doc::inc("n", 5)});
            CHECK(st->get("k") == doc::serialize(v));
            st->close();
        } else {
            const Snapshot before = snapshot(dir);
            for (int attempt = 0; attempt < 2; ++attempt) {
                std::string msg = expectThrow<bpt::VLogCorruptError>(
                    [&] { (void)IndexedStore::open(dir, storeOpts()); }, "P5.G4 open");
                CHECK(msg.find("base seq") != std::string::npos);
                CHECK(snapshot(dir) == before);
            }
        }
        accountVLog(dir);
    }
    pass("P5.R4 consolidated copy wins over the equal-seq Delta; rebuild works without the base",
         "then update() continues from it");
    pass("P5.G4 winning Delta whose base segment is gone -> VLogCorruptError; files unchanged",
         "2 opens");
}

// ------------------------------------------------------------------------------------------
// P5.G1 Segment-id gaps: files 1, 2, 4, 7 -> reopen (replay), full rebuild, next segment, get()
// ------------------------------------------------------------------------------------------
static void testG1SegmentGaps(const std::string& dir) {
    banner("P5.G1 segment ids 1, 2, 4, 7");
    const std::string vdir = dir + "/vlog";
    // One document per 8 KiB segment: 1:a 2:b 3:c1 4:d 5:c2 6:c3 7:c4. Segments 3, 5, 6 then hold
    // only dead versions of c.
    const char* keys[] = {"a", "b", "c", "d", "c", "c", "c"};
    std::map<std::string, std::string> docs;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 7; ++i) {
            docs[keys[i]] = bigDoc(i);
            st->put(keys[i], docs[keys[i]]);
            CHECK(st->tree().get(keys[i])->vptr.segment_id == static_cast<uint32_t>(i + 1));
        }
        st->close();
    }
    for (uint32_t id : {3u, 5u, 6u}) {
        countUnlinked(vdir, id);
        CHECK(::unlink(segPath(vdir, id).c_str()) == 0);
    }
    CHECK((bpt::listVLogSegments(vdir) == std::vector<uint32_t>{1, 2, 4, 7}));
    auto check = [&](IndexedStore& st) {
        for (const auto& [k, d] : docs) CHECK(st.get(k) == d);
        checkStoreEqualsOracle(st, "P5.G1");
    };
    {
        auto st = IndexedStore::open(dir, storeOpts()); // replay from the superblock
        CHECK(!st->openInfo().rebuilt);
        check(*st);
        st->put("e", bigDoc(9));
        CHECK(st->tree().get("e")->vptr.segment_id == 8); // the writer's next id is max + 1
        docs["e"] = bigDoc(9);
        st->close();
    }
    removeSuperblock(dir);
    {
        auto st = IndexedStore::open(dir, storeOpts()); // full rebuild over the gaps
        CHECK(st->openInfo().rebuilt);
        check(*st);
        st->close();
    }
    {
        auto st = IndexedStore::open(dir, storeOpts()); // reopen after the rebuild
        CHECK(!st->openInfo().rebuilt);
        check(*st);
        st->close();
    }
    accountVLog(dir);
    pass("P5.G1 segment ids 1, 2, 4, 7: replay, full rebuild, next segment 8, get() all work");
}

// ------------------------------------------------------------------------------------------
// P5.G5 D2: a superblock whose identity anchor is the lowest segment (as Phase 4 wrote it)
// opens without a rebuild; the next checkpoint records the replay segment instead
// ------------------------------------------------------------------------------------------
static void rewriteAnchorToLowest(const std::string& dir) {
    const std::string vdir = dir + "/vlog";
    const bpt::VLogIdentity low = bpt::readVLogIdentity(vdir);
    CHECK(low.first_segment != 0);
    const std::string path = dir + "/superblock.db";
    int fd = ::open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    int rewritten = 0;
    for (int slot = 0; slot < 2; ++slot) {
        uint8_t buf[bpt::kSuperSlotSize];
        CHECK(::pread(fd, buf, sizeof(buf), slot * bpt::kSuperSlotSize) ==
              static_cast<ssize_t>(sizeof(buf)));
        bpt::SuperblockRecord rec;
        if (bpt::decodeSlot(buf, sizeof(buf), &rec) != bpt::SlotState::Valid) continue;
        rec.ext.vlog_first_segment = low.first_segment;
        rec.ext.vlog_created_ns = low.created_ns;
        rec.sb.oldest_live_segment = low.first_segment;
        bpt::encodeSlot(rec, buf);
        CHECK(::pwrite(fd, buf, sizeof(buf), slot * bpt::kSuperSlotSize) ==
              static_cast<ssize_t>(sizeof(buf)));
        accountBytes(sizeof(buf), "superblock slot rewrite");
        ++rewritten;
    }
    CHECK(rewritten >= 1);
    CHECK(::fsync(fd) == 0 && ::close(fd) == 0);
}

static bpt::SuperblockRecord currentSlot(const std::string& dir) {
    bpt::SuperblockManager m(dir + "/superblock.db");
    bpt::SuperblockManager::LoadResult r = m.load();
    CHECK(r.status == bpt::SuperblockManager::LoadResult::Status::Ok);
    return r.current;
}

static void testG5OldAnchor(const std::string& dir) {
    banner("P5.G5 superblock anchored at the lowest segment (Phase 4 layout)");
    std::map<std::string, std::string> docs;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 5; ++i) {
            docs["k" + std::to_string(i)] = bigDoc(i);
            st->put("k" + std::to_string(i), docs["k" + std::to_string(i)]);
        }
        st->checkpoint();
        st->put("k0", bigDoc(20)); // a second slot, so both slots get rewritten
        docs["k0"] = bigDoc(20);
        st->close();
    }
    const bpt::SuperblockRecord now = currentSlot(dir);
    CHECK(now.ext.vlog_first_segment == now.sb.replay_segment_id); // Phase 5 anchor
    CHECK(now.ext.vlog_first_segment != 1);
    rewriteAnchorToLowest(dir);
    CHECK(currentSlot(dir).ext.vlog_first_segment == 1);
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(!st->openInfo().rebuilt);
        CHECK(st->openInfo().initial_status == bpt::OpenStatus::Ok);
        for (const auto& [k, d] : docs) CHECK(st->get(k) == d);
        st->put("k1", bigDoc(21));
        docs["k1"] = bigDoc(21);
        st->close();
    }
    const bpt::SuperblockRecord after = currentSlot(dir);
    CHECK(after.ext.vlog_first_segment == after.sb.replay_segment_id);
    CHECK(after.sb.oldest_live_segment == 1);
    {
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(!st->openInfo().rebuilt);
        for (const auto& [k, d] : docs) CHECK(st->get(k) == d);
        st->close();
    }
    // A different vLog (anchor segment with another created_unix_ns) still fails the check.
    {
        const bpt::SuperblockRecord cur = currentSlot(dir);
        const std::string p = segPath(dir + "/vlog", cur.ext.vlog_first_segment);
        int fd = ::open(p.c_str(), O_RDWR);
        CHECK(fd >= 0);
        vlog::SegmentHeader h;
        CHECK(::pread(fd, &h, sizeof(h), 0) == static_cast<ssize_t>(sizeof(h)));
        h.created_unix_ns += 1;
        h.crc32c = vlog::crc32c(&h, offsetof(vlog::SegmentHeader, crc32c));
        CHECK(::pwrite(fd, &h, sizeof(h), 0) == static_cast<ssize_t>(sizeof(h)));
        CHECK(::fsync(fd) == 0 && ::close(fd) == 0);
        accountBytes(sizeof(h), "segment header rewrite");
        auto st = IndexedStore::open(dir, storeOpts());
        CHECK(st->openInfo().rebuilt &&
              st->openInfo().initial_status == bpt::OpenStatus::VLogMismatch);
        for (const auto& [k, d] : docs) CHECK(st->get(k) == d);
        st->close();
    }
    accountVLog(dir);
    pass("P5.G5 Phase 4 anchor (lowest segment) opens without a rebuild; next checkpoint "
         "anchors the replay segment; a changed anchor header is still a VLogMismatch");
}

// ------------------------------------------------------------------------------------------
// P5.A Counters
// ------------------------------------------------------------------------------------------
static std::map<uint32_t, uint64_t> liveMap(IndexedStore& st) {
    std::map<uint32_t, uint64_t> m;
    for (const auto& si : st.gcStats().segments) m[si.id] = si.live_bytes;
    return m;
}
static std::map<uint32_t, uint64_t> totalMap(IndexedStore& st) {
    std::map<uint32_t, uint64_t> m;
    for (const auto& si : st.gcStats().segments) m[si.id] = si.total_bytes;
    return m;
}
static void expectCountersExact(IndexedStore& st, const char* what) {
    const std::string why = st.verifyCountersForTest();
    if (!why.empty()) std::cerr << what << ": " << why << "\n";
    CHECK(why.empty());
}
// Reopens and checks the open-time rebuild gives the same live bytes per segment.
static void reopenSameLive(std::unique_ptr<IndexedStore>& st, const std::string& dir,
                           const IndexedStore::Options& o) {
    const std::map<uint32_t, uint64_t> before = liveMap(*st);
    st->close();
    st = IndexedStore::open(dir, o);
    std::map<uint32_t, uint64_t> after = liveMap(*st);
    for (const auto& [id, live] : before) CHECK(after[id] == live);
    for (const auto& [id, live] : after) CHECK(before.count(id) || live == 0);
    expectCountersExact(*st, "after reopen");
}

static void testA1Overwrite(const std::string& dir) {
    banner("P5.A1 overwrite moves bytes out of live");
    auto st = IndexedStore::open(dir, storeOpts());
    st->put("k", bigDoc(1));
    const vlog::VPtr v1 = st->tree().get("k")->vptr;
    st->put("filler", bigDoc(2));
    expectCountersExact(*st, "after 2 puts");
    CHECK(liveMap(*st)[v1.segment_id] == v1.length);
    st->put("k", bigDoc(3));
    const vlog::VPtr v2 = st->tree().get("k")->vptr;
    CHECK(v2.segment_id != v1.segment_id);
    std::map<uint32_t, uint64_t> live = liveMap(*st), total = totalMap(*st);
    CHECK(live[v1.segment_id] == 0 && total[v1.segment_id] == v1.length);
    CHECK(live[v2.segment_id] == v2.length && total[v2.segment_id] == v2.length);
    expectCountersExact(*st, "after overwrite");
    reopenSameLive(st, dir, storeOpts());
    st->close();
    accountVLog(dir);
    pass("P5.A1 overwrite: the old version leaves live_bytes of its segment; reopen agrees");
}

// k: Full in s1, Delta in s2, Delta in s3 (fillers move the writer between segments).
struct Chain3 {
    vlog::VPtr full, d1, d2, f1, f2;
};
static Chain3 buildChain3(IndexedStore& st) {
    Chain3 c;
    st.put("k", padDoc(1, 2900));
    c.full = st.tree().get("k")->vptr;
    st.put("f1", bigDoc(2));
    c.f1 = st.tree().get("f1")->vptr;
    CHECK(st.update("k", {doc::inc("n", 1)}).chain_depth == 1);
    c.d1 = st.tree().get("k")->vptr;
    st.put("f2", bigDoc(3));
    c.f2 = st.tree().get("f2")->vptr;
    CHECK(st.update("k", {doc::inc("n", 1)}).chain_depth == 2);
    c.d2 = st.tree().get("k")->vptr;
    CHECK(c.full.segment_id < c.d1.segment_id && c.d1.segment_id < c.d2.segment_id);
    CHECK(c.d1.segment_id == c.f1.segment_id && c.d2.segment_id == c.f2.segment_id);
    return c;
}

static void testA2Erase(const std::string& dir) {
    banner("P5.A2 erase kills the whole chain; the tombstone counts in total only");
    auto st = IndexedStore::open(dir, storeOpts());
    Chain3 c = buildChain3(*st);
    expectCountersExact(*st, "chain built");
    std::map<uint32_t, uint64_t> total0 = totalMap(*st);
    CHECK(st->erase("k").existed);
    std::map<uint32_t, uint64_t> live = liveMap(*st), total = totalMap(*st);
    CHECK(live[c.full.segment_id] == 0);
    CHECK(live[c.d1.segment_id] == c.f1.length);
    CHECK(live[c.d2.segment_id] == c.f2.length);
    const uint64_t tomb = total[c.d2.segment_id] - total0[c.d2.segment_id];
    CHECK(tomb == vlog::entry_size(1, 0)); // tombstone of key "k": header + padded key
    expectCountersExact(*st, "after erase");
    reopenSameLive(st, dir, storeOpts());
    st->close();
    accountVLog(dir);
    pass("P5.A2 erase: chain bytes leave live_bytes in all 3 segments; tombstone in total only");
}

static void testA3DeltaKillsNothing(const std::string& dir) {
    banner("P5.A3 a Delta append kills nothing");
    auto st = IndexedStore::open(dir, storeOpts());
    st->put("k", padDoc(1, 2900));
    const vlog::VPtr base = st->tree().get("k")->vptr;
    st->put("f1", bigDoc(2));
    std::map<uint32_t, uint64_t> before = liveMap(*st);
    CHECK(st->update("k", {doc::inc("n", 1)}).chain_depth == 1);
    const vlog::VPtr d = st->tree().get("k")->vptr;
    std::map<uint32_t, uint64_t> after = liveMap(*st);
    for (const auto& [id, live] : after) {
        const uint64_t want = before[id] + (id == d.segment_id ? d.length : 0);
        CHECK(live == want);
    }
    CHECK(after[base.segment_id] == base.length); // the base stays live
    expectCountersExact(*st, "after delta");
    reopenSameLive(st, dir, storeOpts());
    st->close();
    accountVLog(dir);
    pass("P5.A3 Delta append: only the Delta's bytes are added; its base stays live");
}

static void testA4ConsolidationAcrossSegments(const std::string& dir) {
    banner("P5.A4 consolidation kills a chain spanning 3 segments");
    IndexedStore::Options o = storeOpts();
    o.delta_chain_cap = 2;
    auto st = IndexedStore::open(dir, o);
    Chain3 c = buildChain3(*st);
    expectCountersExact(*st, "chain built");
    reopenSameLive(st, dir, o); // chains_ rebuilt from the index
    bpt::UpdateResult u = st->update("k", {doc::inc("n", 1)});
    CHECK(u.consolidated && u.chain_depth == 0);
    const vlog::VPtr full2 = st->tree().get("k")->vptr;
    std::map<uint32_t, uint64_t> live = liveMap(*st);
    CHECK(live[c.full.segment_id] == 0);
    CHECK(live[c.d1.segment_id] == c.f1.length);
    CHECK(live[c.d2.segment_id] == c.f2.length + (full2.segment_id == c.d2.segment_id ? full2.length : 0));
    CHECK(live[full2.segment_id] >= full2.length);
    expectCountersExact(*st, "after consolidation");
    reopenSameLive(st, dir, o);
    CHECK(st->gcOpenStats().keys_walked == 3 && st->gcOpenStats().chain_reads == 0);
    st->close();
    accountVLog(dir);
    pass("P5.A4 consolidation: a chain over 3 segments dies in all 3; counters == recount and "
         "== open-time rebuild after every step");
}

int main() {
    std::string dir = setupRun("gc_test");
    Cleaner cleaner{dir};
    testR1AppendRelocated(dir + "/r1");
    testR2CopyReplacesOriginal(dir + "/r2");
    testR3TombstoneBeatsLaterCopy(dir + "/r3");
    testR4ConsolidatedCopy(dir + "/r4");
    testG1SegmentGaps(dir + "/g1");
    testG5OldAnchor(dir + "/g5");
    testA1Overwrite(dir + "/a1");
    testA2Erase(dir + "/a2");
    testA3DeltaKillsNothing(dir + "/a3");
    testA4ConsolidationAcrossSegments(dir + "/a4");
    printBudget();
    return 0;
}
