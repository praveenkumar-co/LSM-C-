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
    testG5RecoverTwiceAndRebuild(dir + "/g5");
    testG6BaseMismatch(dir + "/g6");
    printBudget();
    return 0;
}
