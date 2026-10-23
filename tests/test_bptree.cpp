// Phase 2 tests: Pager, B+Tree, persistence, IndexedStore. See docs/PHASE2_PLAN.md section 3.
#include "test_common.hpp"

#include "index/BPTreeFormat.hpp"
#include "index/Pager.hpp"
#include "index/BPTree.hpp"
#include "index/IndexRecovery.hpp"
#include "index/IndexedStore.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <thread>
#include <set>
#include <random>
#include <string>
#include <vector>

using namespace testutil;
using bpt::kPageSize;
using bpt::OpenStatus;
using bpt::PageGuard;
using bpt::PageType;
using bpt::Pager;
using bpt::BPTree;
using Ref = vlog::IndexValueRef;
using Oracle = std::map<std::string, Ref>;

// ------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------
static Pager::Options pagerOpts(size_t frames) {
    Pager::Options o;
    o.num_frames = frames;
    o.on_write = [](uint64_t n) { accountBytes(n, "index.db page write"); };
    return o;
}

// Flips one bit of a file in place (test-side corruption). The byte write counts toward the budget.
static void flipBitInFile(const std::string& path, uint64_t byte_off, int bit) {
    int fd = open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    uint8_t b = 0;
    CHECK(pread(fd, &b, 1, static_cast<off_t>(byte_off)) == 1);
    b = static_cast<uint8_t>(b ^ (1u << bit));
    CHECK(pwrite(fd, &b, 1, static_cast<off_t>(byte_off)) == 1);
    accountBytes(1, "test pwrite (bit flip)");
    CHECK(vlog::full_fsync(fd) == 0);
    CHECK(close(fd) == 0);
}

static uint64_t fileSize(const std::string& path) {
    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0);
    return static_cast<uint64_t>(st.st_size);
}

// Runs fn in a forked child and returns the raw waitpid status.
template <typename Fn>
static int runInChild(Fn fn) {
    std::cout.flush();
    std::cerr.flush();
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    return status;
}

static bool diedWithSigabrt(int status) { return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT; }

// Pager test pattern in bytes [100, 4000) of a page: inside the free gap, which checkPage ignores.
static uint8_t patternByte(uint32_t id, uint32_t i, uint32_t salt) {
    return static_cast<uint8_t>((id * 2654435761u + i * 40503u + salt * 977u) >> 7);
}
static void writePattern(uint8_t* page, uint32_t id, uint32_t salt) {
    for (uint32_t i = 100; i < 4000; ++i) page[i] = patternByte(id, i, salt);
}
static bool checkPattern(const uint8_t* page, uint32_t id, uint32_t salt) {
    for (uint32_t i = 100; i < 4000; ++i) {
        if (page[i] != patternByte(id, i, salt)) return false;
    }
    return true;
}


// ------------------------------------------------------------------------------------------
// Tree test helpers
// ------------------------------------------------------------------------------------------
static Ref makeRef(uint64_t seq, std::mt19937_64& rng) {
    Ref r{};
    r.seq = seq;
    r.kind = 1;
    r.vptr.segment_id = static_cast<uint32_t>(rng());
    r.vptr.offset = static_cast<uint32_t>(rng());
    r.vptr.length = static_cast<uint32_t>(rng());
    return r;
}

static bool sameRef(const Ref& a, const Ref& b) { return std::memcmp(&a, &b, sizeof(Ref)) == 0; }

static std::string printable(const std::string& k) {
    std::string out;
    for (unsigned char c : k.substr(0, 24)) {
        if (c >= 33 && c < 127) out += static_cast<char>(c);
        else { char b[5]; std::snprintf(b, sizeof(b), "\\x%02x", c); out += b; }
    }
    if (k.size() > 24) out += "...(" + std::to_string(k.size()) + " bytes)";
    return out;
}

// Expected scan [start, end) with limit from the oracle. end == nullptr: no upper bound.
static BPTree::ScanResult oracleScan(const Oracle& o, const std::string& start, const std::string* end, size_t limit) {
    BPTree::ScanResult out;
    if (limit == 0 || (end && start.compare(*end) >= 0)) return out;
    for (auto it = o.lower_bound(start); it != o.end() && out.size() < limit; ++it) {
        if (end && it->first.compare(*end) >= 0) break;
        out.emplace_back(it->first, it->second);
    }
    return out;
}

static void checkScanEq(const BPTree::ScanResult& got, const BPTree::ScanResult& want, const char* what) {
    if (got.size() != want.size()) {
        std::cerr << what << ": scan size " << got.size() << " != expected " << want.size() << "\n";
        CHECK(got.size() == want.size());
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].first != want[i].first || !sameRef(got[i].second, want[i].second)) {
            std::cerr << what << ": scan mismatch at " << i << ": got " << printable(got[i].first) << " want "
                      << printable(want[i].first) << "\n";
            CHECK(false);
        }
    }
}

static void checkFullEq(const BPTree& t, const Oracle& o, const char* what) {
    checkScanEq(t.scanFrom(std::string(), SIZE_MAX), oracleScan(o, std::string(), nullptr, SIZE_MAX), what);
    CHECK(t.size() == o.size());
}

static void checkTree(const BPTree& t, BPTree::Shape* shape = nullptr) {
    std::string err = t.checkInvariants(shape);
    if (!err.empty()) std::cerr << "checkInvariants: " << err << "\n";
    CHECK(err.empty());
}

static std::string randomBytes(std::mt19937_64& rng, size_t len) {
    std::string k(len, '\0');
    for (auto& c : k) c = static_cast<char>(rng() & 0xFF);
    return k;
}

// ------------------------------------------------------------------------------------------
// B1. Pager
// ------------------------------------------------------------------------------------------
static void testB1PageCrcBitFlips() {
    banner("B1.1 Page CRC32C detects every single-bit flip");
    std::vector<uint8_t> page(kPageSize);
    bpt::initPage(page.data(), 7, PageType::Leaf, 0);
    std::mt19937 rng(101);
    for (uint32_t i = bpt::kHeaderSize; i < kPageSize; ++i) page[i] = static_cast<uint8_t>(rng());
    uint32_t crc = bpt::computePageCrc(page.data());
    std::memcpy(page.data(), &crc, 4);

    std::string why;
    CHECK(bpt::checkPage(page.data(), 7, &why));
    CHECK(!bpt::checkPage(page.data(), 8, &why));  // misdirected read: page_id field mismatch
    std::cout << "Wrong page id rejected: " << why << "\n";

    uint32_t detected = 0;
    const uint32_t total_bits = kPageSize * 8;
    for (uint32_t bit = 0; bit < total_bits; ++bit) {
        page[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        if (!bpt::checkPage(page.data(), 7, nullptr)) detected++;
        page[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    }
    CHECK(bpt::checkPage(page.data(), 7, nullptr));
    std::cout << "Single-bit flips rejected: " << detected << " of " << total_bits << "\n";
    CHECK(detected == total_bits);
    std::cout << "PASS: all " << total_bits << " single-bit flips of a 4096-byte page fail checkPage.\n";
}

static void testB1MetaSurvivesReopen(const std::string& dir) {
    banner("B1.2 Pager allocate/read/write, eviction, meta survives reopen");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string path = dir + "/index.db";
    const uint32_t kPages = 200;  // with 16 frames: constant eviction and reload

    {
        auto p = Pager::create(path, pagerOpts(16));
        CHECK(p->pageCount() == 2);
        CHECK(p->rootPageId() == 1);
        for (uint32_t i = 0; i < kPages; ++i) {
            PageGuard g = p->allocate(PageType::Leaf, 0);
            writePattern(g.data(), g.id(), 1);
            g.markDirty();
        }
        // Read every page back through the cache (most were evicted and are reloaded with CRC checks).
        for (uint32_t id = 2; id < 2 + kPages; ++id) {
            PageGuard g = p->fetch(id);
            CHECK(checkPattern(g.data(), id, 1));
        }
        // Overwrite odd pages, read all again.
        for (uint32_t id = 3; id < 2 + kPages; id += 2) {
            PageGuard g = p->fetch(id);
            writePattern(g.data(), id, 2);
            g.markDirty();
        }
        for (uint32_t id = 2; id < 2 + kPages; ++id) {
            PageGuard g = p->fetch(id);
            CHECK(checkPattern(g.data(), id, (id % 2) ? 2 : 1));
        }
        p->setRoot(1, 1);
        p->setKeyCount(12345);
        p->setMaxAppliedSeq(987654321);
        std::string err = p->verifyPagesOnDisk();
        CHECK(err.empty());
        Pager::Stats s = p->stats();
        std::cout << "Session 1: pages " << p->pageCount() << ", frames 16, reads " << s.page_reads << ", writes "
                  << s.page_writes << ", evictions " << s.evictions << "\n";
        CHECK(s.evictions > 0);
        p->closeClean();
    }

    bpt::MetaBody m{};
    CHECK(Pager::inspect(path, &m) == OpenStatus::Ok);
    std::cout << "On disk after clean close: clean_shutdown=" << m.clean_shutdown << " page_count=" << m.page_count
              << " key_count=" << m.key_count << " max_applied_seq=" << m.max_applied_seq
              << " open_count=" << m.open_count << " file_size=" << fileSize(path) << "\n";
    CHECK(m.clean_shutdown == 1);
    CHECK(m.page_count == 2 + kPages);
    CHECK(m.key_count == 12345 && m.max_applied_seq == 987654321 && m.open_count == 1);
    CHECK(fileSize(path) == uint64_t(m.page_count) * kPageSize);

    {
        OpenStatus st;
        auto p = Pager::open(path, pagerOpts(32), &st);
        CHECK(st == OpenStatus::Ok && p != nullptr);
        CHECK(p->pageCount() == 2 + kPages && p->keyCount() == 12345 && p->maxAppliedSeq() == 987654321);
        CHECK(p->openCount() == 2);
        bpt::MetaBody m2{};
        CHECK(Pager::inspect(path, &m2) == OpenStatus::NotClean);  // clean flag cleared on disk at open
        std::cout << "On disk right after open: clean_shutdown=" << m2.clean_shutdown << " open_count=" << m2.open_count
                  << "\n";
        CHECK(m2.clean_shutdown == 0 && m2.open_count == 2);
        for (uint32_t id = 2; id < 2 + kPages; ++id) {
            PageGuard g = p->fetch(id);
            CHECK(checkPattern(g.data(), id, (id % 2) ? 2 : 1));
        }
        p->closeClean();
    }
    std::cout << "Reopen with 32 frames: all " << kPages << " page patterns and meta fields match.\n";

    // Open, then drop the pager without closeClean (simulated crash): next open must refuse.
    {
        OpenStatus st;
        auto p = Pager::open(path, pagerOpts(16), &st);
        CHECK(st == OpenStatus::Ok);
    }
    {
        OpenStatus st;
        auto p = Pager::open(path, pagerOpts(16), &st);
        CHECK(p == nullptr && st == OpenStatus::NotClean);
        std::cout << "Open after a close without closeClean: " << bpt::toString(st) << "\n";
    }
    std::cout << "PASS: meta survives clean close + reopen; clean_shutdown is 1 after close, 0 after open; an "
                 "unclean close is refused.\n";
}

static void testB1Corruption(const std::string& dir) {
    banner("B1.3 Corruption: flipped bit in a page, in the meta page, wrong file size");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string path = dir + "/index.db";
    {
        auto p = Pager::create(path, pagerOpts(16));
        for (uint32_t i = 0; i < 10; ++i) {
            PageGuard g = p->allocate(PageType::Leaf, 0);
            writePattern(g.data(), g.id(), 1);
            g.markDirty();
        }
        p->closeClean();
    }
    // Flip one bit in the middle of page 5.
    flipBitInFile(path, 5ULL * kPageSize + 2000, 3);
    std::cout << "Flipped bit 3 of byte 2000 in page 5.\n";

    // Meta is intact, so open succeeds. Page 4 loads; page 5 must abort in the checker.
    std::cout << "Child: open, fetch page 4, fetch page 5 (expected to abort). Child stderr follows:\n";
    int status = runInChild([&]() {
        childExpectsAbort();
        OpenStatus st;
        auto p = Pager::open(path, pagerOpts(16), &st);
        CHECK(st == OpenStatus::Ok);
        { PageGuard g = p->fetch(4); CHECK(checkPattern(g.data(), 4, 1)); }
        { PageGuard g = p->fetch(5); }
        std::cerr << "child: page 5 loaded without error (unexpected)\n";
        _exit(3);
    });
    std::cout << "Child status: " << (diedWithSigabrt(status) ? "killed by SIGABRT" : "NOT SIGABRT") << "\n";
    CHECK(diedWithSigabrt(status));
    bpt::MetaBody m{};
    OpenStatus st_after = Pager::inspect(path, &m);
    std::cout << "Meta after the child's abort: " << bpt::toString(st_after) << " (clean_shutdown=" << m.clean_shutdown
              << ")\n";
    CHECK(st_after == OpenStatus::NotClean);  // the child's open cleared the flag, so the next open rebuilds

    // Wrong file size after a clean close.
    const std::string path2 = dir + "/index2.db";
    {
        auto p = Pager::create(path2, pagerOpts(16));
        p->closeClean();
    }
    CHECK(Pager::inspect(path2, &m) == OpenStatus::Ok);
    CHECK(truncate(path2.c_str(), 3 * kPageSize) == 0);
    accountBytes(kPageSize, "test truncate (extend by one page)");
    OpenStatus st;
    CHECK(Pager::open(path2, pagerOpts(16), &st) == nullptr && st == OpenStatus::SizeMismatch);
    std::cout << "File extended to 3 pages after clean close: " << bpt::toString(st) << "\n";
    CHECK(truncate(path2.c_str(), 2 * kPageSize) == 0);
    CHECK(Pager::inspect(path2, &m) == OpenStatus::Ok);

    // Flip one bit in the meta body.
    flipBitInFile(path2, 32 + 17, 0);
    CHECK(Pager::open(path2, pagerOpts(16), &st) == nullptr && st == OpenStatus::BadMeta);
    std::cout << "Flipped bit in meta page: " << bpt::toString(st) << "\n";

    // Missing file.
    CHECK(Pager::open(dir + "/nope.db", pagerOpts(16), &st) == nullptr && st == OpenStatus::Missing);
    std::cout << "PASS: page bit flip aborts the reader; meta bit flip -> BadMeta; wrong size -> SizeMismatch; "
                 "missing -> Missing.\n";
}


// ------------------------------------------------------------------------------------------
// B2. Single-leaf tree
// ------------------------------------------------------------------------------------------
static void testB2Basics(const std::string& dir) {
    banner("B2.1 Single leaf: put/get/erase/scan basics, key length limits");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    auto p = Pager::create(dir + "/index.db", pagerOpts(16));
    BPTree t(*p);
    std::mt19937_64 rng(202);
    Oracle o;

    CHECK(!t.get("a").has_value());
    CHECK(!t.erase("a"));
    CHECK(t.scanFrom("", 10).empty());

    for (const char* k : {"m", "c", "x", "a", "q"}) {
        Ref r = makeRef(o.size() + 1, rng);
        CHECK(t.put(k, r) == BPTree::PutStatus::Inserted);
        o[k] = r;
    }
    checkFullEq(t, o, "after 5 puts");
    Ref r2 = makeRef(100, rng);
    CHECK(t.put("c", r2) == BPTree::PutStatus::Updated);
    o["c"] = r2;
    CHECK(sameRef(*t.get("c"), r2));
    CHECK(t.erase("x") && !t.erase("x"));
    o.erase("x");
    checkFullEq(t, o, "after update + erase");

    // Scan edge cases.
    std::string m = "m", q = "q", a = "a", c = "c", z = "z", b = "b";
    checkScanEq(t.scan("c", "q", 100), oracleScan(o, c, &q, 100), "scan [c,q)");
    checkScanEq(t.scan("q", "c", 100), {}, "scan start > end");
    checkScanEq(t.scan("m", "m", 100), {}, "scan start == end");
    checkScanEq(t.scan("a", "z", 1), oracleScan(o, a, &z, 1), "limit 1");
    checkScanEq(t.scan("a", "z", 0), {}, "limit 0");
    checkScanEq(t.scan("b", "z", 100), oracleScan(o, b, &z, 100), "start between keys");
    checkScanEq(t.scan("", "zzzz", 100), oracleScan(o, "", nullptr, 100), "whole range");

    // Key length limits: 257 and 0 rejected without changing the tree; 256 accepted.
    std::string k256(256, 'k'), k257(257, 'k');
    Ref r3 = makeRef(200, rng);
    CHECK(t.put(k257, r3) == BPTree::PutStatus::InvalidKey);
    CHECK(t.put("", r3) == BPTree::PutStatus::InvalidKey);
    CHECK(!t.get(k257).has_value() && !t.erase(k257));
    checkFullEq(t, o, "after rejected puts");
    CHECK(t.put(k256, r3) == BPTree::PutStatus::Inserted);
    o[k256] = r3;
    checkFullEq(t, o, "after 256-byte key");
    checkTree(t);
    std::cout << "PASS: put/get/erase/scan edge cases match std::map; 257-byte and empty keys rejected, tree "
                 "unchanged; 256-byte key accepted.\n";
}

static void testB2RandomVsMap(const std::string& dir) {
    banner("B2.2 Single leaf: 20,000 random ops vs std::map");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    auto p = Pager::create(dir + "/index.db", pagerOpts(16));
    BPTree t(*p);
    const uint64_t seed = 2020;
    std::mt19937_64 rng(seed);
    // 40 distinct keys of 1-16 bytes (bytes include 0x00 and 0xFF): at most 40 * 44 = 1760 bytes,
    // so everything fits in the single root leaf.
    std::vector<std::string> keys;
    std::set<std::string> seen;
    while (keys.size() < 40) {
        std::string k = randomBytes(rng, 1 + rng() % 16);
        if (seen.insert(k).second) keys.push_back(k);
    }
    keys[0] = std::string(1, '\0');
    keys[1] = std::string(16, '\xff');
    Oracle o;
    uint64_t seq = 0, n_put = 0, n_get = 0, n_erase = 0, n_scan = 0;
    for (int op = 0; op < 20000; ++op) {
        const std::string& k = keys[rng() % keys.size()];
        uint32_t dice = rng() % 100;
        if (dice < 40) {
            Ref r = makeRef(++seq, rng);
            BPTree::PutStatus st = t.put(k, r);
            CHECK(st == (o.count(k) ? BPTree::PutStatus::Updated : BPTree::PutStatus::Inserted));
            o[k] = r;
            n_put++;
        } else if (dice < 65) {
            auto got = t.get(k);
            auto it = o.find(k);
            CHECK(got.has_value() == (it != o.end()));
            if (got) CHECK(sameRef(*got, it->second));
            n_get++;
        } else if (dice < 85) {
            CHECK(t.erase(k) == (o.erase(k) == 1));
            n_erase++;
        } else {
            std::string s = keys[rng() % keys.size()], e = keys[rng() % keys.size()];
            size_t limit = 1 + rng() % 50;
            checkScanEq(t.scan(s, e, limit), oracleScan(o, s, &e, limit), "random scan");
            n_scan++;
        }
        if (op % 1000 == 999) {
            checkFullEq(t, o, "periodic full scan");
            checkTree(t);
        }
    }
    BPTree::Shape sh;
    checkTree(t, &sh);
    CHECK(sh.height == 1 && p->pageCount() == 2);
    CHECK(p->verifyPagesOnDisk().empty());
    std::cout << "seed " << seed << ": put " << n_put << ", get " << n_get << ", erase " << n_erase << ", scan "
              << n_scan << "; final keys " << o.size() << ", height " << sh.height << ", pages " << p->pageCount()
              << "\n";
    std::cout << "PASS: every op result matched std::map; invariants held every 1,000 ops; page CRCs valid on disk.\n";
}

static void testB2FullLeafAndCompaction(const std::string& dir) {
    banner("B2.3 Single leaf: capacity with 256-byte keys, compaction after erase");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    auto p = Pager::create(dir + "/index.db", pagerOpts(16));
    BPTree t(*p);
    std::mt19937_64 rng(203);
    Oracle o;
    auto bigKey = [](int i) { std::string k(256, 'A'); k[0] = static_cast<char>('a' + i / 26); k[1] = static_cast<char>('a' + i % 26); return k; };
    BPTree::Shape sh;
    for (int i = 0; i < 14; ++i) {
        Ref r = makeRef(i + 1, rng);
        CHECK(t.put(bigKey(i), r) == BPTree::PutStatus::Inserted);
        o[bigKey(i)] = r;
    }
    checkTree(t, &sh);
    std::cout << "14 keys of 256 bytes (cell 282 B + slot 2 B each; 4064 usable): height " << sh.height << ", leaves "
              << sh.leaves << "\n";
    CHECK(sh.height == 1 && sh.leaves == 1);
    // Erase every other key (fragmenting the cell area), then insert 7 new 256-byte keys: needs compaction.
    for (int j = 0; j < 14; j += 2) { CHECK(t.erase(bigKey(j))); o.erase(bigKey(j)); }
    for (int j = 100; j < 107; ++j) {
        Ref r = makeRef(1000 + j, rng);
        CHECK(t.put(bigKey(j), r) == BPTree::PutStatus::Inserted);
        o[bigKey(j)] = r;
    }
    checkTree(t, &sh);
    CHECK(sh.height == 1 && sh.leaves == 1);
    checkFullEq(t, o, "after compaction");
    std::cout << "After 7 erases + 7 inserts of 256-byte keys: height " << sh.height << ", leaves " << sh.leaves
              << " (fit by compaction, no split)\n";
    // The 15th max-size entry does not fit: the leaf splits and a root is created.
    Ref r = makeRef(5000, rng);
    CHECK(t.put(bigKey(200), r) == BPTree::PutStatus::Inserted);
    o[bigKey(200)] = r;
    checkTree(t, &sh);
    std::cout << "15th 256-byte key: height " << sh.height << ", leaves " << sh.leaves << ", internal "
              << sh.internals << "\n";
    CHECK(sh.height == 2 && sh.leaves == 2 && sh.internals == 1);
    checkFullEq(t, o, "after first split");
    std::cout << "PASS: one leaf holds exactly 14 max-size entries; compaction reuses erased space; the 15th entry "
                 "splits the root leaf.\n";
}

// ------------------------------------------------------------------------------------------
// B3. Splits
// ------------------------------------------------------------------------------------------

// T1 key space: 70% 1-32 bytes, 10% exactly 256, 10% 200-255, 10% sharing a 180-byte prefix.
// Random bytes, so 0x00 and 0xFF occur; two keys are forced to be all-0x00 and all-0xFF.
static std::vector<std::string> makeKeySpace(std::mt19937_64& rng, size_t n) {
    std::vector<std::string> keys;
    std::set<std::string> seen;
    std::string prefix = randomBytes(rng, 180);
    while (keys.size() < n) {
        uint32_t d = rng() % 100;
        std::string k;
        if (d < 70) k = randomBytes(rng, 1 + rng() % 32);
        else if (d < 80) k = randomBytes(rng, 256);
        else if (d < 90) k = randomBytes(rng, 200 + rng() % 56);
        else k = prefix + randomBytes(rng, 1 + rng() % 40);
        if (seen.insert(k).second) keys.push_back(std::move(k));
    }
    keys[0] = std::string(1, '\0');
    keys[1] = std::string(256, '\xff');
    return keys;
}

struct T1Result {
    uint64_t puts = 0, gets = 0, erases = 0, scans = 0, scanned = 0;
};

static void runRandomOps(BPTree& t, Oracle& o, const std::vector<std::string>& keys, std::mt19937_64& rng,
                         uint64_t& seq, int ops, int check_every, T1Result* res) {
    for (int op = 0; op < ops; ++op) {
        uint32_t dice = rng() % 100;
        const std::string& k = keys[rng() % keys.size()];
        if (dice < 40) {
            Ref r = makeRef(++seq, rng);
            BPTree::PutStatus st = t.put(k, r);
            CHECK(st == (o.count(k) ? BPTree::PutStatus::Updated : BPTree::PutStatus::Inserted));
            o[k] = r;
            res->puts++;
        } else if (dice < 65) {
            auto got = t.get(k);
            auto it = o.find(k);
            CHECK(got.has_value() == (it != o.end()));
            if (got) CHECK(sameRef(*got, it->second));
            res->gets++;
        } else if (dice < 85) {
            CHECK(t.erase(k) == (o.erase(k) == 1));
            res->erases++;
        } else {
            // Bounds: existing keys or random byte strings (which fall between keys).
            std::string a = (rng() % 2) ? keys[rng() % keys.size()] : randomBytes(rng, 1 + rng() % 40);
            std::string b = (rng() % 2) ? keys[rng() % keys.size()] : randomBytes(rng, 1 + rng() % 40);
            if (a > b) std::swap(a, b);
            size_t limit = 1 + rng() % 500;
            BPTree::ScanResult got = t.scan(a, b, limit);
            checkScanEq(got, oracleScan(o, a, &b, limit), "random scan");
            res->scans++;
            res->scanned += got.size();
        }
        if (check_every > 0 && (op + 1) % check_every == 0) {
            checkFullEq(t, o, "periodic full scan");
            checkTree(t);
        }
    }
}

static void testB3RandomVsMap(const std::string& dir, uint64_t seed, size_t frames, size_t key_space, int ops) {
    banner("B3.1 " + std::to_string(ops) + " random ops vs std::map (seed " + std::to_string(seed) + ", " + std::to_string(frames) +
           " frames, " + std::to_string(key_space) + " keys)");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    uint64_t bytes_before = bytesWritten();
    auto t0 = std::chrono::steady_clock::now();
    auto p = Pager::create(dir + "/index.db", pagerOpts(frames));
    BPTree t(*p);
    std::mt19937_64 rng(seed);
    std::vector<std::string> keys = makeKeySpace(rng, key_space);
    Oracle o;
    uint64_t seq = 0;
    T1Result res;
    runRandomOps(t, o, keys, rng, seq, ops, std::min(ops, 10000), &res);
    BPTree::Shape sh;
    checkTree(t, &sh);
    std::string disk_err = p->verifyPagesOnDisk();
    CHECK(disk_err.empty());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    Pager::Stats ps = p->stats();
    std::cout << "ops: put " << res.puts << ", get " << res.gets << ", erase " << res.erases << ", scan " << res.scans
              << " (" << res.scanned << " entries returned)\n";
    std::cout << "final: keys " << o.size() << ", height " << sh.height << ", pages " << p->pageCount() << ", leaves "
              << sh.leaves << " (" << sh.empty_leaves << " empty), internal " << sh.internals << "\n";
    std::cout << "pager: page reads " << ps.page_reads << ", page writes " << ps.page_writes << ", evictions "
              << ps.evictions << "; bytes written by this test " << (bytesWritten() - bytes_before) << "; wall time "
              << ms << " ms\n";
    CHECK(sh.height >= (kQuick ? 2u : 3u));
    std::cout << "PASS: every op matched std::map; full scan + checkInvariants passed every 10,000 ops; all page "
                 "CRCs valid on disk.\n";
}

static void testB3Sequential(const std::string& dir) {
    banner("B3.2 Ascending and descending bulk inserts (splits at the right and left edges)");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    std::mt19937_64 rng(302);
    for (int pass = 0; pass < 2; ++pass) {
        auto p = Pager::create(dir + (pass ? "/desc.db" : "/asc.db"), pagerOpts(1024));  // tree fits: no eviction writes
        BPTree t(*p);
        Oracle o;
        const int n = 20000;
        for (int i = 0; i < n; ++i) {
            int v = pass ? (n - 1 - i) : i;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "key%08d", v);
            Ref r = makeRef(i + 1, rng);
            CHECK(t.put(buf, r) == BPTree::PutStatus::Inserted);
            o[buf] = r;
        }
        BPTree::Shape sh;
        checkTree(t, &sh);
        checkFullEq(t, o, pass ? "descending" : "ascending");
        std::cout << (pass ? "descending" : "ascending ") << ": " << n << " keys, height " << sh.height << ", leaves "
                  << sh.leaves << ", internal " << sh.internals << ", avg keys/leaf "
                  << (sh.keys / sh.leaves) << "\n";
    }
    std::cout << "PASS: both orders match std::map and pass checkInvariants.\n";
}

static void testB3ScanBoundaries(const std::string& dir) {
    banner("B3.3 Range scans across page boundaries");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    auto p = Pager::create(dir + "/index.db", pagerOpts(32));
    BPTree t(*p);
    std::mt19937_64 rng(303);
    Oracle o;
    // Fixed 100-byte keys: cell 126 B + slot 2 B = 128 B, so at most 31 entries per leaf.
    auto key = [](int i) { char b[16]; std::snprintf(b, sizeof(b), "k%06d", i); std::string k(b); k.resize(100, '.'); return k; };
    for (int i = 0; i < 3000; i += 2) {  // even ids only, so odd ids fall between keys
        Ref r = makeRef(i + 1, rng);
        CHECK(t.put(key(i), r) == BPTree::PutStatus::Inserted);
        o[key(i)] = r;
    }
    auto leaves = t.leafKeysForTest();
    CHECK(leaves.size() >= 10);
    size_t max_per_leaf = 0;
    for (auto& l : leaves) max_per_leaf = std::max(max_per_leaf, l.size());
    std::cout << "1500 keys of 100 bytes in " << leaves.size() << " leaves (max " << max_per_leaf << " per leaf)\n";
    CHECK(max_per_leaf <= 31);

    int checks = 0;
    auto check = [&](const std::string& a, const std::string& b, size_t limit, const char* what) {
        checkScanEq(t.scan(a, b, limit), oracleScan(o, a, &b, limit), what);
        checks++;
    };
    const std::string top = std::string(1, '\x7f');
    for (size_t li = 0; li + 3 < leaves.size(); ++li) {
        const auto& L = leaves[li];
        const auto& N = leaves[li + 1];
        CHECK(!L.empty() && !N.empty());
        check(L.back(), top, 3, "start at last key of a leaf");
        check(L.front(), N.front(), 1000, "end exactly at first key of next leaf");
        check(L.front(), top, L.size(), "limit ends exactly on a leaf boundary");
        check(L.front(), top, L.size() + 1, "limit one past a leaf boundary");
        check(L[L.size() / 2], leaves[li + 3].back(), 1000, "spans 4 leaves");
    }
    check("", top, SIZE_MAX, "whole tree");
    check(key(1), key(9), 100, "start and end between keys");
    check("", key(0), 100, "end at the smallest key");
    check("", "k", 100, "range below the smallest key");
    check(key(2998) + "x", top, 100, "start above the largest key");
    check(key(10), key(10), 100, "start == end");
    check(key(20), key(10), 100, "start > end");
    check(key(10), key(500), 1, "limit 1");

    // Empty one middle leaf, then scan across it.
    size_t mid = leaves.size() / 2;
    for (const auto& k : leaves[mid]) { CHECK(t.erase(k)); o.erase(k); }
    BPTree::Shape sh;
    checkTree(t, &sh);
    CHECK(sh.empty_leaves == 1);
    check(leaves[mid - 1].front(), leaves[mid + 1].back(), 1000, "across an empty leaf");
    check(leaves[mid - 1].back(), top, 2, "limit 2 across an empty leaf");
    check(leaves[mid].front(), leaves[mid].back(), 1000, "inside the empty leaf's range");
    // Erase everything.
    for (int i = 0; i < 3000; i += 2) { t.erase(key(i)); o.erase(key(i)); }
    checkTree(t, &sh);
    CHECK(sh.keys == 0 && o.empty());
    check("", top, SIZE_MAX, "empty tree");
    std::cout << "After erasing all keys: " << sh.leaves << " leaves (" << sh.empty_leaves
              << " empty) remain; no merges in Phase 2.\n";
    std::cout << "PASS: " << checks << " boundary scans matched std::map lower_bound(start)..lower_bound(end).\n";
}

static void testB3KeyLimits(const std::string& dir) {
    banner("B3.4 Keys > 256 bytes rejected on a multi-level tree");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    auto p = Pager::create(dir + "/index.db", pagerOpts(1024));
    BPTree t(*p);
    std::mt19937_64 rng(304);
    std::vector<std::string> keys = makeKeySpace(rng, 3000);
    Oracle o;
    uint64_t seq = 0;
    for (const auto& k : keys) { Ref r = makeRef(++seq, rng); CHECK(t.put(k, r) == BPTree::PutStatus::Inserted); o[k] = r; }
    BPTree::Shape before;
    checkTree(t, &before);
    uint32_t pages_before = p->pageCount();
    int rejected = 0;
    for (size_t len : {257u, 300u, 1000u, 65535u, 70000u, 0u}) {
        std::string k = randomBytes(rng, len);
        CHECK(t.put(k, makeRef(++seq, rng)) == BPTree::PutStatus::InvalidKey);
        CHECK(!t.get(k).has_value());
        CHECK(!t.erase(k));
        CHECK(t.apply(k, vlog::EntryType::Full, ++seq, 0, vlog::VPtr{1, 4096, 64}) == BPTree::ApplyResult::InvalidKey);
        rejected++;
    }
    BPTree::Shape after;
    checkTree(t, &after);
    CHECK(after.keys == before.keys && p->pageCount() == pages_before);
    checkFullEq(t, o, "after rejected keys");
    std::string k256 = randomBytes(rng, 256);
    Ref r = makeRef(++seq, rng);
    CHECK(t.put(k256, r) == BPTree::PutStatus::Inserted);
    o[k256] = r;
    checkFullEq(t, o, "after 256-byte key");
    std::cout << "Tree height " << before.height << ", " << before.keys << " keys. Rejected key lengths: 257, 300, "
                 "1000, 65535, 70000, 0 (" << rejected << " cases, via put/get/erase/apply).\n";
    std::cout << "PASS: oversize and empty keys rejected; tree and page count unchanged; 256-byte key accepted.\n";
}

// ------------------------------------------------------------------------------------------
// B4. Persistence: clean close + reopen, crash detection, rebuild from the vLog
// ------------------------------------------------------------------------------------------

// VLogWriter that adds its byte count to the global budget when it goes out of scope.
class TrackedWriter : public vlog::VLogWriter {
public:
    TrackedWriter(const std::string& dir, uint64_t local_cap = 64ULL * 1024 * 1024)
        : vlog::VLogWriter(dir, vlog::VLogWriter::Options(64 * 1024 * 1024, std::min(local_cap, remainingBudget()))) {}
    ~TrackedWriter() { accountBytes(getTotalBytesWritten(), "VLogWriter"); }
};

// Reference state: replay the vLog into a std::map with should_apply() (independent of the tree).
static Oracle vlogOracle(const std::string& vlog_dir) {
    Oracle o;
    std::vector<uint32_t> segs = bpt::listVLogSegments(vlog_dir);
    vlog::VLogReader reader(vlog_dir);
    for (size_t i = 0; i < segs.size(); ++i) {
        vlog::ScanResult sr = reader.scanSegment(segs[i], i + 1 == segs.size());
        for (const auto& e : sr.entries) {
            auto it = o.find(e.key);
            bool has = it != o.end();
            if (!vlog::should_apply(has, has ? it->second.seq : 0, e.header.seq, e.header.flags)) continue;
            if (e.header.type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) {
                o.erase(e.key);
            } else {
                Ref r{};
                r.seq = e.header.seq;
                r.kind = 1;
                r.vptr = e.vptr;
                o[e.key] = r;
            }
        }
    }
    return o;
}

// Single-threaded put/erase through vLog append + tree.apply; the oracle tracks the expected index.
static void vlogOps(vlog::VLogWriter& w, BPTree& t, Oracle& o, const std::vector<std::string>& keys,
                    std::mt19937_64& rng, int n) {
    std::vector<uint8_t> payload(64);
    for (int i = 0; i < n; ++i) {
        const std::string& k = keys[rng() % keys.size()];
        if (rng() % 100 < 65) {
            for (auto& b : payload) b = static_cast<uint8_t>(rng());
            vlog::AppendResult ar = w.append(vlog::EntryType::Full, k, payload.data(), static_cast<uint32_t>(payload.size()));
            CHECK(t.apply(k, vlog::EntryType::Full, ar.seq, 0, ar.vptr) == BPTree::ApplyResult::Applied);
            Ref r{};
            r.seq = ar.seq;
            r.kind = 1;
            r.vptr = ar.vptr;
            o[k] = r;
        } else {
            vlog::AppendResult ar = w.append(vlog::EntryType::Tombstone, k, nullptr, 0);
            CHECK(t.apply(k, vlog::EntryType::Tombstone, ar.seq, 0, ar.vptr) == BPTree::ApplyResult::Applied);
            o.erase(k);
        }
    }
}

static void testB4CleanReopen(const std::string& dir) {
    banner("B4.1 Clean close + reopen equals the oracle (T3)");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string path = dir + "/index.db";
    const uint64_t seed = 401;
    const uint64_t bytes_before = bytesWritten();
    std::mt19937_64 rng(seed);
    std::vector<std::string> keys = makeKeySpace(rng, 20000);
    Oracle o;
    uint64_t seq = 0;
    T1Result res;
    {
        auto p = Pager::create(path, pagerOpts(1024));
        BPTree t(*p);
        runRandomOps(t, o, keys, rng, seq, kQuick ? 10000 : 50000, 0, &res);
        checkTree(t);
        p->closeClean();
    }
    struct Round { size_t frames; int more_ops; };
    const int more = kQuick ? 1000 : 10000;
    for (Round rd : {Round{1024, more}, Round{64, more}, Round{16, 0}}) {
        bpt::MetaBody m{};
        CHECK(Pager::inspect(path, &m) == OpenStatus::Ok && m.clean_shutdown == 1);
        OpenStatus st;
        auto p = Pager::open(path, pagerOpts(rd.frames), &st);
        CHECK(st == OpenStatus::Ok);
        CHECK(Pager::inspect(path, &m) == OpenStatus::NotClean && m.clean_shutdown == 0);
        BPTree t(*p);
        BPTree::Shape sh;
        checkTree(t, &sh);
        checkFullEq(t, o, "after reopen");
        std::cout << "reopen #" << p->openCount() - 1 << " with " << rd.frames << " frames: on-disk clean_shutdown 1 -> 0, "
                  << o.size() << " keys == oracle, height " << sh.height << ", pages " << p->pageCount() << "\n";
        if (rd.more_ops > 0) {
            runRandomOps(t, o, keys, rng, seq, rd.more_ops, 0, &res);
            checkFullEq(t, o, "after more ops");
            checkTree(t);
        }
        p->closeClean();
    }
    std::cout << "bytes written by this test: " << (bytesWritten() - bytes_before) << "\n";
    std::cout << "ops total: put " << res.puts << ", get " << res.gets << ", erase " << res.erases << ", scan "
              << res.scans << " (seed " << seed << ")\n";
    std::cout << "PASS: " << (kQuick ? 10000 : 50000) << " ops + clean close; reopened with 1024, 64, 16 frames ("
              << more << " more ops after the first two); every reopen matched std::map and passed checkInvariants.\n";
}

static void checkIndexEquals(const std::string& dir, const std::string& vlog_dir, Pager& p, const Oracle& o,
                             const char* what) {
    BPTree t(p);
    checkTree(t);
    checkFullEq(t, o, what);
    Oracle ref = vlogOracle(vlog_dir);
    checkFullEq(t, ref, what);
    (void)dir;
}

static void testB4CrashRebuild(const std::string& dir) {
    banner("B4.2 Crash detection and rebuild from the vLog");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string vdir = dir + "/vlog";
    const std::string idx = dir + "/index.db";
    std::mt19937_64 rng(402);
    std::vector<std::string> keys = makeKeySpace(rng, 300);
    Oracle o;
    auto opts = pagerOpts(32);

    // Session 1: 1,500 ops through vLog + index, then "crash": drop the pager without closeClean.
    {
        TrackedWriter w(vdir);
        bpt::OpenIndexResult r = bpt::openOrRebuildIndex(dir, vdir, opts);
        CHECK(r.initial_status == OpenStatus::Missing && r.rebuilt && r.vlog_entries_scanned == 0);
        BPTree t(*r.pager);
        vlogOps(w, t, o, keys, rng, 1500);
        checkFullEq(t, o, "before crash");
    }
    auto reopen = [&](OpenStatus expect, bool expect_rebuild, const char* what) {
        TrackedWriter w(vdir);
        bpt::OpenIndexResult r = bpt::openOrRebuildIndex(dir, vdir, opts);
        CHECK(r.initial_status == expect && r.rebuilt == expect_rebuild);
        checkIndexEquals(dir, vdir, *r.pager, o, what);
        BPTree t(*r.pager);
        BPTree::Shape sh;
        checkTree(t, &sh);
        std::cout << what << ": first open " << bpt::toString(r.initial_status) << ", rebuilt " << (r.rebuilt ? "yes" : "no")
                  << ", vLog entries scanned " << r.vlog_entries_scanned << ", applied " << r.vlog_entries_applied
                  << ", rebuild " << r.rebuild_ms << " ms, keys " << sh.keys << ", height " << sh.height << ", pages "
                  << r.pager->pageCount() << "\n";
        r.pager->closeClean();
    };
    reopen(OpenStatus::NotClean, true, "after crash (no clean close)");
    reopen(OpenStatus::Ok, false, "after clean close");

    // Corrupt one bit in a non-meta page after a clean close. A child that walks the tree must abort;
    // its open already cleared clean_shutdown, so the next open rebuilds.
    uint64_t npages = fileSize(idx) / kPageSize;
    flipBitInFile(idx, (npages - 1) * kPageSize + 3000, 5);
    std::cout << "Flipped a bit in page " << npages - 1 << " of " << npages << ". Child stderr follows:\n";
    int status = runInChild([&]() {
        childExpectsAbort();
        OpenStatus st;
        auto p = Pager::open(idx, pagerOpts(32), &st);
        CHECK(st == OpenStatus::Ok);
        BPTree t(*p);
        (void)t.checkInvariants();  // touches every page
        _exit(3);
    });
    CHECK(diedWithSigabrt(status));
    std::cout << "Child status: killed by SIGABRT\n";
    reopen(OpenStatus::NotClean, true, "after page corruption");

    flipBitInFile(idx, 40, 1);  // meta body
    reopen(OpenStatus::BadMeta, true, "after meta corruption");

    CHECK(unlink(idx.c_str()) == 0);
    reopen(OpenStatus::Missing, true, "after index.db deleted");

    CHECK(truncate(idx.c_str(), static_cast<off_t>(fileSize(idx) + kPageSize)) == 0);
    accountBytes(kPageSize, "test truncate (extend by one page)");
    reopen(OpenStatus::SizeMismatch, true, "after file size change");

    // Leftover index.db.rebuild from an interrupted rebuild, plus a dirty index.db.
    {
        int fd = open((dir + "/index.db.rebuild").c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        CHECK(fd >= 0);
        std::vector<uint8_t> junk(3 * kPageSize + 17, 0xAB);
        CHECK(write(fd, junk.data(), junk.size()) == static_cast<ssize_t>(junk.size()));
        accountBytes(junk.size(), "test write (junk rebuild file)");
        CHECK(close(fd) == 0);
        OpenStatus st;
        auto p = Pager::open(idx, opts, &st);  // clears clean_shutdown
        CHECK(st == OpenStatus::Ok);
    }
    reopen(OpenStatus::NotClean, true, "with leftover index.db.rebuild");
    struct stat sb;
    CHECK(stat((dir + "/index.db.rebuild").c_str(), &sb) != 0 && errno == ENOENT);

    // vLog entries written after the index's clean close are applied at the next clean open.
    {
        TrackedWriter w(vdir);
        std::vector<uint8_t> payload(64, 0x5A);
        for (int i = 0; i < 20; ++i) {
            const std::string& k = keys[rng() % keys.size()];
            if (i % 4 == 3) {
                w.append(vlog::EntryType::Tombstone, k, nullptr, 0);
                o.erase(k);
            } else {
                vlog::AppendResult ar = w.append(vlog::EntryType::Full, k, payload.data(), 64);
                Ref r{};
                r.seq = ar.seq;
                r.kind = 1;
                r.vptr = ar.vptr;
                o[k] = r;
            }
        }
    }
    reopen(OpenStatus::Ok, false, "after 20 vLog appends with the index closed");
    std::cout << "PASS: every rebuilt or reopened index equals both the op oracle and an independent should_apply() "
                 "replay of the vLog, and passes checkInvariants.\n";
}

// ------------------------------------------------------------------------------------------
// B5. Engine API (IndexedStore) and kill -9
// ------------------------------------------------------------------------------------------
using bpt::IndexedStore;

static IndexedStore::Options storeOpts(size_t frames) {
    IndexedStore::Options o;
    o.cache_frames = frames;
    o.vlog_opts = vlog::VLogWriter::Options(64 * 1024 * 1024, std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index.db page write"); };
    return o;
}

// Closes (or drops) the store and accounts its vLog bytes.
static void finishStore(std::unique_ptr<IndexedStore>& st, bool clean) {
    uint64_t vb = st->vlogBytesWritten();
    if (clean) st->close();
    st.reset();
    accountBytes(vb, "IndexedStore vLog writes");
}

static void testB5Api(const std::string& dir) {
    banner("B5.1 IndexedStore API: put/get/erase through vLog + index");
    auto st = IndexedStore::open(dir, storeOpts(64));
    CHECK(st->openInfo().rebuilt && st->openInfo().initial_status == OpenStatus::Missing);
    uint64_t s1 = st->put("alpha", "{\"v\":1}");
    uint64_t s2 = st->put("beta", "{\"v\":2}");
    uint64_t s3 = st->put("alpha", "{\"v\":3}");
    CHECK(s1 < s2 && s2 < s3);
    uint64_t seq = 0;
    CHECK(st->get("alpha", &seq) == std::optional<std::string>("{\"v\":3}") && seq == s3);
    CHECK(st->get("beta") == std::optional<std::string>("{\"v\":2}"));
    CHECK(!st->get("gamma").has_value());
    IndexedStore::EraseResult e1 = st->erase("beta");
    IndexedStore::EraseResult e2 = st->erase("beta");
    CHECK(e1.existed && !e2.existed && e1.seq < e2.seq);
    CHECK(!st->get("beta").has_value());
    std::string empty_doc;
    st->put("empty-doc", empty_doc);
    CHECK(st->get("empty-doc") == std::optional<std::string>(""));
    // Bad keys are rejected before anything reaches the vLog.
    uint64_t vbytes = st->vlogBytesWritten();
    int rejected = 0;
    for (size_t len : {0u, 257u, 5000u}) {
        try { st->put(std::string(len, 'x'), "doc"); } catch (const std::invalid_argument&) { rejected++; }
        try { st->erase(std::string(len, 'x')); } catch (const std::invalid_argument&) { rejected++; }
    }
    CHECK(rejected == 6 && st->vlogBytesWritten() == vbytes);
    std::cout << "seqs: put alpha " << s1 << ", put beta " << s2 << ", put alpha " << s3 << ", erase beta " << e1.seq
              << " (existed) and " << e2.seq << " (absent)\n";
    std::cout << "6 bad-key calls rejected with std::invalid_argument; vLog bytes unchanged (" << vbytes << ")\n";
    finishStore(st, true);
    auto st2 = IndexedStore::open(dir, storeOpts(64));
    CHECK(!st2->openInfo().rebuilt && st2->openInfo().initial_status == OpenStatus::Ok);
    CHECK(st2->get("alpha") == std::optional<std::string>("{\"v\":3}") && !st2->get("beta").has_value());
    finishStore(st2, true);
    std::cout << "PASS: get returns the newest document read from the vLog; erase writes a tombstone; values "
                 "survive clean close + reopen.\n";
}

#ifndef TSAN_BUILD
// Pipe record from the child. 24 bytes < PIPE_BUF, so each write() is atomic.
struct AckRec {
    uint64_t seq;
    uint32_t op;       // 1 = put acked, 2 = erase acked, 9 = child paused at a crash point
    uint32_t key_id;
    uint32_t writer;
    uint32_t op_idx;
};
static_assert(sizeof(AckRec) == 24);

static std::string b5Key(uint32_t id) { return "user:" + std::to_string(id); }

static std::string b5Doc(uint32_t writer, uint32_t op_idx, uint32_t key_id) {
    std::string d = "{\"w\":" + std::to_string(writer) + ",\"i\":" + std::to_string(op_idx) + ",\"k\":" +
                    std::to_string(key_id) + ",\"pad\":\"";
    d.append(40 + (op_idx * 7 + writer) % 80, static_cast<char>('a' + (op_idx + key_id) % 26));
    return d + "\"}";
}

static void sendRec(int fd, const AckRec& r) { CHECK(write(fd, &r, sizeof(r)) == static_cast<ssize_t>(sizeof(r))); }

// Reads one record; returns false on EOF.
static bool readRec(int fd, AckRec* r) {
    while (true) {
        ssize_t n = read(fd, r, sizeof(*r));
        if (n == static_cast<ssize_t>(sizeof(*r))) return true;
        if (n == 0) return false;
        CHECK(n < 0 && errno == EINTR);  // a short read of a 24-byte record is a bug
    }
}

static void pauseForever() {
    while (true) pause();
}

static uint64_t dirBytes(const std::string& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::end(it);
         it.increment(ec)) {
        if (it->is_regular_file()) total += it->file_size();
    }
    CHECK(!ec);
    return total;
}

// Child body: 4 writer threads doing put/erase on 64 keys, acking each op after it returns.
// stop_after_ops > 0: each thread does that many ops, then the store is clean-closed (with a hook
// that pauses between the page fsync and the meta write).
static void b5ChildWriters(const std::string& sdir, int wfd, uint32_t ops_per_thread, bool close_and_pause) {
    try {
        IndexedStore::Options o = storeOpts(32);
        if (close_and_pause) {
            o.hooks.on_close_after_flush = [wfd]() { sendRec(wfd, AckRec{0, 9, 0, 0, 0}); pauseForever(); };
        }
        auto st = IndexedStore::open(sdir, o);
        std::vector<std::thread> th;
        for (uint32_t w = 0; w < 4; ++w) {
            th.emplace_back([&, w]() {
                std::mt19937 rng(1000 + w);
                for (uint32_t i = 0; i < ops_per_thread; ++i) {
                    uint32_t k = rng() % 64;
                    AckRec r{0, 0, k, w, i};
                    if (rng() % 100 < 65) {
                        r.seq = st->put(b5Key(k), b5Doc(w, i, k));
                        r.op = 1;
                    } else {
                        r.seq = st->erase(b5Key(k)).seq;
                        r.op = 2;
                    }
                    sendRec(wfd, r);
                }
            });
        }
        for (auto& t : th) t.join();
        if (close_and_pause) st->close();  // pauses inside the hook; never returns
    } catch (const std::exception& e) {
        std::cerr << "[Child] exception: " << e.what() << "\n";
        _exit(2);
    }
    _exit(0);
}

struct VEntry {
    uint64_t seq;
    uint8_t type;
    std::vector<uint8_t> payload;
};

static void testB5KillDuringWrites(const std::string& base) {
    banner("B5.2 kill -9 during writes / rebuild / clean close, reopen, check every acked key");
    CHECK(mkdir(base.c_str(), 0755) == 0);
    const int kIters = kQuick ? 10 : 50;
    std::mt19937 rng(505);
    std::uniform_int_distribution<int> delay_ms(5, 400);
    uint64_t total_acked = 0, total_unacked = 0, total_checked_keys = 0;
    int kinds[3] = {0, 0, 0};

    for (int iter = 0; iter < kIters; ++iter) {
        const std::string sdir = base + "/iter_" + std::to_string(iter);
        // 0: kill during writes; 1: kill during writes, then kill the rebuilding child; 2: kill during clean close.
        int kind = (iter % 5 == 3) ? 1 : (iter % 5 == 4) ? 2 : 0;
        kinds[kind]++;
        int pfd[2];
        CHECK(pipe(pfd) == 0);
        std::cout.flush();
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            CHECK(close(pfd[0]) == 0);
            b5ChildWriters(sdir, pfd[1], kind == 2 ? 60 : 2000, kind == 2);
        }
        CHECK(close(pfd[1]) == 0);

        std::vector<AckRec> acks;
        int kill_after = -1;
        bool paused = false;
        AckRec r;
        if (kind == 2) {
            while (readRec(pfd[0], &r)) {
                if (r.op == 9) { paused = true; break; }
                acks.push_back(r);
            }
            CHECK(paused && acks.size() == 240);
        } else {
            while (acks.size() < 200 && readRec(pfd[0], &r)) acks.push_back(r);
            CHECK(acks.size() == 200);
            kill_after = delay_ms(rng);
            std::this_thread::sleep_for(std::chrono::milliseconds(kill_after));
        }
        CHECK(kill(pid, SIGKILL) == 0);
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        while (readRec(pfd[0], &r)) { CHECK(r.op != 9); acks.push_back(r); }
        CHECK(close(pfd[0]) == 0);
        accountBytes(dirBytes(sdir), "B5 child files (vLog + index.db)");

        bool killed_rebuild = false;
        if (kind == 1) {
            // A second child opens the store (rebuild), pauses after 100 applied entries, and is killed.
            int qfd[2];
            CHECK(pipe(qfd) == 0);
            std::cout.flush();
            pid_t p2 = fork();
            CHECK(p2 >= 0);
            if (p2 == 0) {
                CHECK(close(qfd[0]) == 0);
                IndexedStore::Options o = storeOpts(32);
                o.hooks.on_rebuild_entry = [&](uint64_t n) {
                    if (n == 100) { sendRec(qfd[1], AckRec{0, 9, 0, 0, 0}); pauseForever(); }
                };
                auto st = IndexedStore::open(sdir, o);
                _exit(0);  // not reached if the hook fired
            }
            CHECK(close(qfd[1]) == 0);
            CHECK(readRec(qfd[0], &r) && r.op == 9);
            struct stat sb;
            CHECK(stat((sdir + "/index.db.rebuild").c_str(), &sb) == 0);  // rebuild in progress
            CHECK(kill(p2, SIGKILL) == 0);
            CHECK(waitpid(p2, &status, 0) == p2);
            CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            CHECK(close(qfd[0]) == 0);
            accountBytes(sb.st_size + 4096, "B5 killed rebuild (partial index.db.rebuild + segment header)");
            killed_rebuild = true;
        }

        // Independent reference: the recovered vLog, read directly.
        // (Opening the store below runs vLog recovery first; read the vLog after that.)
        IndexedStore::Options o = storeOpts(64);
        auto st = IndexedStore::open(sdir, o);
        const bpt::OpenIndexResult& info = st->openInfo();
        CHECK(info.rebuilt);
        CHECK(info.initial_status == OpenStatus::NotClean);
        struct stat sb;
        CHECK(stat((sdir + "/index.db.rebuild").c_str(), &sb) != 0 && errno == ENOENT);

        std::map<std::string, std::vector<VEntry>> per_key;
        std::set<uint64_t> vlog_seqs;
        {
            std::vector<uint32_t> segs = bpt::listVLogSegments(st->vlogDir());
            vlog::VLogReader reader(st->vlogDir());
            for (size_t i = 0; i < segs.size(); ++i) {
                vlog::ScanResult sr = reader.scanSegment(segs[i], i + 1 == segs.size());
                for (auto& e : sr.entries) {
                    const uint64_t seq = e.header.seq;  // copy: EntryHeader is packed, no reference binding
                    per_key[e.key].push_back(VEntry{seq, e.header.type, e.payload});
                    vlog_seqs.insert(seq);
                }
            }
        }
        // Check 1: index == should_apply() replay of the recovered vLog.
        Oracle ref = vlogOracle(st->vlogDir());
        checkFullEq(st->tree(), ref, "index vs vLog replay");
        checkTree(st->tree());

        // Check 2: every acked op is reflected. Use the newest acked op per key.
        std::map<uint32_t, AckRec> latest;
        for (const AckRec& a : acks) {
            CHECK(vlog_seqs.count(a.seq) == 1);  // every acked seq is in the vLog
            auto it = latest.find(a.key_id);
            if (it == latest.end() || a.seq > it->second.seq) latest[a.key_id] = a;
        }
        for (const auto& [kid, a] : latest) {
            std::string key = b5Key(kid);
            uint64_t got_seq = 0;
            std::optional<std::string> doc = st->get(key, &got_seq);
            const auto& hist = per_key[key];
            if (a.op == 1) {
                if (doc) {
                    CHECK(got_seq >= a.seq);
                    if (got_seq == a.seq) CHECK(*doc == b5Doc(a.writer, a.op_idx, kid));  // the acked value
                } else {
                    // Absent only if a later tombstone (acked or not) is in the vLog.
                    bool later_tomb = false;
                    for (const VEntry& e : hist) {
                        if (e.seq > a.seq && e.type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) later_tomb = true;
                    }
                    CHECK(later_tomb);
                }
            } else {
                if (doc) CHECK(got_seq > a.seq);  // re-created by a later put
            }
            // Whatever the store returns must be the newest vLog entry's payload for that key.
            if (doc) {
                const VEntry& last = hist.back();
                CHECK(last.seq == got_seq && std::string(last.payload.begin(), last.payload.end()) == *doc);
            }
            total_checked_keys++;
        }
        uint64_t unacked = vlog_seqs.size() - acks.size();
        total_acked += acks.size();
        total_unacked += unacked;
        const char* kind_name = kind == 0 ? "kill during writes" : kind == 1 ? "kill during rebuild" : "kill in clean close";
        std::printf("Iter %2d/%d: %-19s %s acked %4zu, vLog entries %4zu (unacked %2llu), keys %2zu, rebuild %3llu ms\n",
                    iter + 1, kIters, kind_name,
                    kill_after >= 0 ? ("(+" + std::to_string(kill_after) + " ms),").c_str() : "",
                    acks.size(), vlog_seqs.size(), (unsigned long long)unacked, ref.size(),
                    (unsigned long long)info.rebuild_ms);
        (void)killed_rebuild;

        // Clean close + reopen: no rebuild, same contents.
        finishStore(st, true);
        auto st2 = IndexedStore::open(sdir, storeOpts(64));
        CHECK(!st2->openInfo().rebuilt && st2->openInfo().initial_status == OpenStatus::Ok);
        checkFullEq(st2->tree(), ref, "after clean reopen");
        finishStore(st2, true);
        std::error_code ec;
        std::filesystem::remove_all(sdir, ec);
        CHECK(!ec);
    }
    std::cout << "Crash points: " << kinds[0] << " during writes, " << kinds[1] << " during rebuild (after a write "
              << "crash), " << kinds[2] << " between page fsync and meta write in clean close.\n";
    std::cout << "PASS: " << kIters << " iterations; " << total_acked << " acked ops all present in the vLog; for "
              << total_checked_keys << " keys the newest acked op is reflected (acked value returned, or a newer "
              << "write); index == vLog replay every time; " << total_unacked << " recovered-but-unacked entries.\n";
}
#endif

// ------------------------------------------------------------------------------------------
// B6. Concurrency
// ------------------------------------------------------------------------------------------
// 100-byte keys, so 256 keys span several leaves and splits happen while the threads run.
static std::string b6Key(uint32_t id) { std::string k = "acct:" + std::to_string(id); k.resize(100, '.'); return k; }

static void testB6StoreMixed(const std::string& dir) {
    banner("B6.1 IndexedStore: 8 threads, mixed put/erase/get/scan; final state == vLog replay");
    const uint32_t kThreads = 8, kOps = 500, kKeys = 256;
    auto st = IndexedStore::open(dir, storeOpts(64));

    struct Obs { std::string key; uint64_t seq; std::string doc; };
    struct WriteRec { uint64_t seq; std::string key; bool is_put; std::string doc; };
    std::vector<std::vector<Obs>> observed(kThreads);
    std::vector<std::vector<WriteRec>> writes(kThreads);
    std::atomic<uint64_t> n_put{0}, n_erase{0}, n_get{0}, n_get_hit{0}, n_scan{0}, n_scanned{0};
    std::atomic<bool> failed{false};

    auto worker = [&](uint32_t tid) {
        std::mt19937 rng(600 + tid);
        std::map<std::string, uint64_t> last_seen;  // per key: highest seq this thread has observed or written
        for (uint32_t i = 0; i < kOps && !failed.load(); ++i) {
            uint32_t d = rng() % 100;
            std::string key = b6Key(rng() % kKeys);
            if (d < 35) {
                std::string doc = "{\"t\":" + std::to_string(tid) + ",\"i\":" + std::to_string(i) + ",\"key\":\"" + key + "\"}";
                uint64_t seq = st->put(key, doc);
                writes[tid].push_back(WriteRec{seq, key, true, doc});
                last_seen[key] = std::max(last_seen[key], seq);
                n_put++;
            } else if (d < 50) {
                uint64_t seq = st->erase(key).seq;
                writes[tid].push_back(WriteRec{seq, key, false, ""});
                last_seen[key] = std::max(last_seen[key], seq);
                n_erase++;
            } else if (d < 80) {
                uint64_t seq = 0;
                std::optional<std::string> doc = st->get(key, &seq);
                n_get++;
                if (doc) {
                    n_get_hit++;
                    if (seq < last_seen[key]) { std::cerr << "seq went backwards for " << key << "\n"; failed = true; }
                    last_seen[key] = seq;
                    observed[tid].push_back(Obs{key, seq, *doc});
                }
            } else {
                std::string a = b6Key(rng() % kKeys), b = b6Key(rng() % kKeys);
                if (a > b) std::swap(a, b);
                size_t limit = 1 + rng() % 64;
                BPTree::ScanResult r = st->scan(a, b, limit);
                if (r.size() > limit) failed = true;
                for (size_t j = 0; j < r.size(); ++j) {
                    if (r[j].first < a || r[j].first >= b) failed = true;
                    if (j > 0 && r[j - 1].first >= r[j].first) failed = true;
                }
                n_scan++;
                n_scanned += r.size();
            }
        }
    };
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> th;
    for (uint32_t t = 0; t < kThreads; ++t) th.emplace_back(worker, t);
    for (auto& t : th) t.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!failed.load());

    // Every document a reader saw is exactly what a writer wrote with that seq for that key.
    std::map<uint64_t, const WriteRec*> by_seq;
    for (auto& v : writes) for (auto& w : v) CHECK(by_seq.emplace(w.seq, &w).second);
    uint64_t n_obs = 0;
    for (auto& v : observed) {
        for (auto& o : v) {
            auto it = by_seq.find(o.seq);
            CHECK(it != by_seq.end() && it->second->is_put && it->second->key == o.key && it->second->doc == o.doc);
            n_obs++;
        }
    }
    // Seqs are dense: every assigned seq was acked to some thread.
    CHECK(!by_seq.empty() && by_seq.begin()->first == 1 && by_seq.rbegin()->first == by_seq.size());

    // Final state == replay of the acked writes in seq order == should_apply() replay of the vLog.
    Oracle from_writes;
    for (auto& [seq, w] : by_seq) {
        if (w->is_put) {
            Ref r{};
            r.seq = seq;
            from_writes[w->key] = r;
        } else {
            from_writes.erase(w->key);
        }
    }
    Oracle ref = vlogOracle(st->vlogDir());
    checkFullEq(st->tree(), ref, "index vs vLog replay");
    CHECK(ref.size() == from_writes.size());
    for (auto& [k, r] : ref) CHECK(from_writes.count(k) && from_writes[k].seq == r.seq);
    for (auto& [k, r] : ref) {
        uint64_t seq = 0;
        std::optional<std::string> doc = st->get(k, &seq);
        CHECK(doc && seq == r.seq && *doc == by_seq[seq]->doc);
    }
    BPTree::Shape sh;
    checkTree(st->tree(), &sh);
    std::cout << "ops: put " << n_put << ", erase " << n_erase << ", get " << n_get << " (" << n_get_hit << " hits), scan "
              << n_scan << " (" << n_scanned << " entries); " << ms << " ms; final keys " << ref.size() << ", height "
              << sh.height << "\n";
    std::cout << n_obs << " documents read by get() matched the acked write with the same seq; seqs 1.."
              << by_seq.size() << " all acked.\n";
    finishStore(st, true);
    auto st2 = IndexedStore::open(dir, storeOpts(64));
    CHECK(!st2->openInfo().rebuilt);
    checkFullEq(st2->tree(), ref, "after clean reopen");
    finishStore(st2, true);
    std::cout << "PASS: final index == replay of acked writes in seq order == should_apply() replay of the vLog; "
                 "scans sorted and in bounds; per-thread seqs never went backwards.\n";
}

static void testB6TreeStress(const std::string& dir) {
    banner("B6.2 BPTree: 4 writers (own key ranges) + 4 readers, 24-frame cache (evictions under readers)");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const uint64_t bytes_before = bytesWritten();
    auto p = Pager::create(dir + "/index.db", pagerOpts(24));
    BPTree t(*p);
    const uint32_t kWriters = 4, kReaders = 4, kWriterOps = kQuick ? 400 : 3000, kReaderOps = kQuick ? 2000 : 6000, kKeysPerWriter = 1500;
    std::vector<Oracle> local(kWriters);
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> reads{0}, hits{0}, scanned{0};
    // Key layout: "w<writer>/<id>" padded to 60 bytes. seq = (writer << 40) | per-writer counter.
    auto key = [](uint32_t w, uint32_t id) { std::string k = "w" + std::to_string(w) + "/" + std::to_string(id); k.resize(60, '#'); return k; };

    auto writer = [&](uint32_t w) {
        std::mt19937_64 rng(700 + w);
        uint64_t counter = 0;
        for (uint32_t i = 0; i < kWriterOps; ++i) {
            std::string k = key(w, static_cast<uint32_t>(rng() % kKeysPerWriter));
            if (rng() % 100 < 70) {
                Ref r = makeRef((uint64_t(w) << 40) | ++counter, rng);
                t.put(k, r);
                local[w][k] = r;
            } else {
                bool had = t.erase(k);
                if (had != (local[w].erase(k) == 1)) failed = true;
            }
        }
    };
    auto reader = [&](uint32_t rid) {
        std::mt19937_64 rng(800 + rid);
        std::map<std::string, uint64_t> last;  // monotonic reads per key (one writer per key)
        for (uint32_t i = 0; i < kReaderOps; ++i) {
            uint32_t w = static_cast<uint32_t>(rng() % kWriters);
            std::string k = key(w, static_cast<uint32_t>(rng() % kKeysPerWriter));
            if (rng() % 100 < 70) {
                auto r = t.get(k);
                reads++;
                if (r) {
                    hits++;
                    if ((r->seq >> 40) != w) failed = true;
                    if (r->seq < last[k]) failed = true;
                    last[k] = r->seq;
                }
            } else {
                std::string a = key(w, 0), b = key(w, static_cast<uint32_t>(rng() % kKeysPerWriter));
                if (a > b) std::swap(a, b);
                BPTree::ScanResult res = t.scan(a, b, 1 + rng() % 100);
                for (size_t j = 0; j < res.size(); ++j) {
                    if (res[j].first < a || res[j].first >= b) failed = true;
                    if (j > 0 && res[j - 1].first >= res[j].first) failed = true;
                }
                scanned += res.size();
            }
        }
    };
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> th;
    for (uint32_t w = 0; w < kWriters; ++w) th.emplace_back(writer, w);
    for (uint32_t r = 0; r < kReaders; ++r) th.emplace_back(reader, r);
    for (auto& x : th) x.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!failed.load());
    Oracle all;
    for (auto& o : local) all.insert(o.begin(), o.end());
    checkFullEq(t, all, "after concurrent stress");
    BPTree::Shape sh;
    checkTree(t, &sh);
    CHECK(p->verifyPagesOnDisk().empty());
    Pager::Stats ps = p->stats();
    std::cout << "writer ops " << kWriters * kWriterOps << ", reader ops " << kReaders * kReaderOps << " (gets " << reads
              << ", hits " << hits << ", scanned entries " << scanned << "); " << ms << " ms\n";
    std::cout << "final keys " << all.size() << ", height " << sh.height << ", pages " << p->pageCount()
              << "; page reads " << ps.page_reads << ", page writes " << ps.page_writes << ", evictions " << ps.evictions
              << "; bytes written by this test " << (bytesWritten() - bytes_before) << "\n";
    CHECK(ps.evictions > 0);
    std::cout << "PASS: final tree == union of writer oracles; reads were per-key monotonic and in the right key "
                 "range; checkInvariants and on-disk CRCs pass.\n";
}

int main() {
    std::cout << "====================================================================\n"
              << "  PHASE 2: B+TREE INDEX TESTS\n"
              << "====================================================================\n";
    std::string test_dir = setupRun("bptree_test");
    Cleaner cleaner{test_dir};

    testB1PageCrcBitFlips();
    testB1MetaSurvivesReopen(test_dir + "/b1_meta");
    testB1Corruption(test_dir + "/b1_corrupt");
    testB2Basics(test_dir + "/b2_basic");
    testB2RandomVsMap(test_dir + "/b2_random");
    testB2FullLeafAndCompaction(test_dir + "/b2_full");
    // Seed 1 uses 32 frames (eviction + CRC-checked reload on most ops). Measured: one 32-frame
    // seed writes about 196 MB of page write-backs, so three would exceed the 512 MiB budget.
    // Seeds 2 and 3 use the default 1024 frames (the tree fits; no eviction).
    // Quick mode: 3,000 ops on the 32-frame seed, 10,000 on the others.
    testB3RandomVsMap(test_dir + "/b3_seed1", 31, 32, 20000, kQuick ? 3000 : 100000);
    testB3RandomVsMap(test_dir + "/b3_seed2", 32, 1024, 20000, kQuick ? 10000 : 100000);
    testB3RandomVsMap(test_dir + "/b3_seed3", 33, 1024, 20000, kQuick ? 10000 : 100000);
    testB3Sequential(test_dir + "/b3_seq");
    testB3ScanBoundaries(test_dir + "/b3_scan");
    testB3KeyLimits(test_dir + "/b3_limits");
    testB4CleanReopen(test_dir + "/b4_reopen");
    testB4CrashRebuild(test_dir + "/b4_rebuild");
    testB5Api(test_dir + "/b5_api");
#ifndef TSAN_BUILD
    testB5KillDuringWrites(test_dir + "/b5_kill");
#else
    std::cout << "\n[SKIP] B5.2 fork/kill -9 test skipped under ThreadSanitizer (same rule as Phase 1).\n";
#endif

    testB6StoreMixed(test_dir + "/b6_store");
    testB6TreeStress(test_dir + "/b6_tree");

    printBudget();
    std::cout << "\n====================================================================\n"
              << "  ALL PHASE 2 TESTS IN THIS BUILD PASSED\n"
              << "====================================================================\n";
    return 0;
}
