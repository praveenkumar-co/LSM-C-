// Phase 2 tests: Pager, B+Tree, persistence, IndexedStore. See docs/PHASE2_PLAN.md section 3.
#include "test_common.hpp"

#include "index/BPTreeFormat.hpp"
#include "index/Pager.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <random>
#include <string>
#include <vector>

using namespace testutil;
using bpt::kPageSize;
using bpt::OpenStatus;
using bpt::PageGuard;
using bpt::PageType;
using bpt::Pager;

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

int main() {
    std::cout << "====================================================================\n"
              << "  PHASE 2: B+TREE INDEX TESTS\n"
              << "====================================================================\n";
    std::string test_dir = setupRun("bptree_test");
    Cleaner cleaner{test_dir};

    testB1PageCrcBitFlips();
    testB1MetaSurvivesReopen(test_dir + "/b1_meta");
    testB1Corruption(test_dir + "/b1_corrupt");

    printBudget();
    std::cout << "\n====================================================================\n"
              << "  ALL PHASE 2 TESTS IN THIS BUILD PASSED\n"
              << "====================================================================\n";
    return 0;
}
