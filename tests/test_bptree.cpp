// Phase 2 tests: Pager, B+Tree, persistence, IndexedStore. See docs/PHASE2_PLAN.md section 3.
#include "test_common.hpp"

#include "index/BPTreeFormat.hpp"
#include "index/Pager.hpp"
#include "index/BPTree.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
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
    int i = 0;
    while (true) {
        Ref r = makeRef(i + 1, rng);
        BPTree::PutStatus st = t.put(bigKey(i), r);
        if (st == BPTree::PutStatus::LeafFull) break;
        CHECK(st == BPTree::PutStatus::Inserted);
        o[bigKey(i)] = r;
        ++i;
    }
    std::cout << "256-byte keys that fit in one leaf: " << i << " (cell 282 B + slot 2 B each; 4064 usable)\n";
    CHECK(i == 14);
    checkFullEq(t, o, "full leaf");
    // Erase every other key (fragmenting the cell area), then insert new 256-byte keys: needs compaction.
    for (int j = 0; j < 14; j += 2) { CHECK(t.erase(bigKey(j))); o.erase(bigKey(j)); }
    for (int j = 100; j < 107; ++j) {
        Ref r = makeRef(1000 + j, rng);
        CHECK(t.put(bigKey(j), r) == BPTree::PutStatus::Inserted);
        o[bigKey(j)] = r;
    }
    CHECK(t.put(bigKey(200), makeRef(5000, rng)) == BPTree::PutStatus::LeafFull);
    checkFullEq(t, o, "after compaction");
    checkTree(t);
    std::cout << "PASS: leaf holds exactly 14 max-size entries; after 7 erases, 7 new max-size inserts succeed via "
                 "compaction; contents match std::map.\n";
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

    printBudget();
    std::cout << "\n====================================================================\n"
              << "  ALL PHASE 2 TESTS IN THIS BUILD PASSED\n"
              << "====================================================================\n";
    return 0;
}
