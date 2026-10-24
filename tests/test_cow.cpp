// Step 2 tests: copy-on-write pager and tree, epoch-delayed page reuse, free list rebuilt at open.
#include "test_common.hpp"

#include "index/BPTree.hpp"
#include "index/IndexRecovery.hpp"
#include "index/Pager.hpp"
#include "index/SuperblockManager.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace testutil;
using bpt::BPTree;
using bpt::OpenStatus;
using bpt::Pager;
using bpt::page_id_t;
using Ref = vlog::IndexValueRef;
using Oracle = std::map<std::string, uint64_t>;  // key -> seq

static Pager::Options pagerOpts(size_t frames) {
    Pager::Options o;
    o.num_frames = frames;
    o.on_write = [](uint64_t n) { accountBytes(n, "index.db page write"); };
    return o;
}

static std::unique_ptr<bpt::SuperblockManager> newSbm(const std::string& dir) {
    bpt::SuperblockManager::Options so;
    so.on_write = [](uint64_t n) { accountBytes(n, "superblock.db write"); };
    auto m = std::make_unique<bpt::SuperblockManager>(dir + "/superblock.db", so);
    m->load();
    return m;
}

static Pager::CheckpointStats checkpoint(Pager& p, bpt::SuperblockManager& sbm, uint64_t seq) {
    Pager::CommitInfo ci;
    ci.checkpoint_seq = seq;
    return p.checkpoint(sbm, ci);
}

static std::string key(uint32_t i) {
    char b[16];
    std::snprintf(b, sizeof(b), "k%07u", i);
    std::string k(b);
    k.resize(100, '.');  // 100-byte keys: about 31 per leaf, so a few thousand keys give height 3
    return k;
}

// C3/C4 sizes: initial keys, key space, ops per round (quick mode writes about 40% as much).
static const uint32_t kInit = kQuick ? 800 : 2000;
static const uint32_t kSpace = kQuick ? 960 : 2400;
static const int kRoundOps = kQuick ? 600 : 1500;

static Ref ref(uint64_t seq) {
    Ref r{};
    r.seq = seq;
    r.kind = 1;
    r.vptr.offset = static_cast<uint32_t>(seq);
    return r;
}

// Random put (60%) / erase (40%) over n_keys keys.
static void randomOps(BPTree& t, Oracle& o, std::mt19937& rng, uint64_t& seq, uint32_t n_keys, int ops) {
    for (int i = 0; i < ops; ++i) {
        std::string k = key(rng() % n_keys);
        if (rng() % 100 < 60) {
            ++seq;
            CHECK(t.put(k, ref(seq)) != BPTree::PutStatus::InvalidKey);
            o[k] = seq;
        } else {
            CHECK(t.erase(k) == (o.erase(k) == 1));
        }
    }
}

static void checkTreeEq(const BPTree& t, const Oracle& o, const char* what) {
    BPTree::ScanResult all = t.scanFrom("", SIZE_MAX);
    if (all.size() != o.size()) std::cerr << what << ": tree has " << all.size() << " keys, oracle " << o.size() << "\n";
    CHECK(all.size() == o.size());
    size_t i = 0;
    for (const auto& [k, s] : o) {
        CHECK(all[i].first == k && all[i].second.seq == s);
        ++i;
    }
    std::string err = t.checkInvariants();
    if (!err.empty()) std::cerr << what << ": " << err << "\n";
    CHECK(err.empty());
}

// Reads a committed tree straight from disk (pread, no cache): every leaf entry.
static void readLeaves(int fd, page_id_t id, Oracle* out) {
    std::vector<uint8_t> buf(bpt::kPageSize);
    CHECK(pread(fd, buf.data(), bpt::kPageSize, static_cast<off_t>(id) * bpt::kPageSize) == bpt::kPageSize);
    std::string why;
    if (!bpt::checkPage(buf.data(), id, &why)) std::cerr << why << "\n";
    CHECK(bpt::checkPage(buf.data(), id, &why));
    bpt::Node n(buf.data());
    if (n.isLeaf()) {
        for (uint32_t i = 0; i < n.count(); ++i) (*out)[std::string(n.key(i))] = n.leafValue(i).seq;
        return;
    }
    std::vector<page_id_t> kids;
    for (uint32_t j = 0; j <= n.count(); ++j) kids.push_back(n.childAt(j));
    for (page_id_t c : kids) readLeaves(fd, c, out);
}

// A committed tree as recorded right after its checkpoint.
struct Snapshot {
    uint64_t epoch = 0;
    page_id_t root = 0;
    uint32_t page_count = 0;
    Oracle contents;
    std::map<page_id_t, uint32_t> page_crc;  // every page of the tree -> CRC32C of its 4096 bytes
};

static uint32_t pageCrc(int fd, page_id_t id) {
    std::vector<uint8_t> buf(bpt::kPageSize);
    CHECK(pread(fd, buf.data(), bpt::kPageSize, static_cast<off_t>(id) * bpt::kPageSize) == bpt::kPageSize);
    return vlog::crc32c(buf.data(), buf.size());
}

static Snapshot takeSnapshot(const std::string& path, uint64_t epoch, page_id_t root, uint32_t page_count, const Oracle& o) {
    Snapshot s;
    s.epoch = epoch;
    s.root = root;
    s.page_count = page_count;
    s.contents = o;
    int fd = open(path.c_str(), O_RDONLY);
    CHECK(fd >= 0);
    bpt::TreeWalk w = bpt::walkTree(fd, root, page_count);
    if (!w.ok) std::cerr << w.why << "\n";
    CHECK(w.ok);
    for (page_id_t id = 1; id < page_count; ++id) {
        if (w.reachable[id]) s.page_crc[id] = pageCrc(fd, id);
    }
    CHECK(close(fd) == 0);
    return s;
}

// Every page of the snapshot is byte-for-byte unchanged on disk, and the tree still reads back
// as the contents it had at its checkpoint.
static void checkSnapshotIntact(const std::string& path, const Snapshot& s, const char* what) {
    int fd = open(path.c_str(), O_RDONLY);
    CHECK(fd >= 0);
    size_t changed = 0;
    for (const auto& [id, crc] : s.page_crc) {
        if (pageCrc(fd, id) != crc) changed++;
    }
    Oracle got;
    readLeaves(fd, s.root, &got);
    CHECK(close(fd) == 0);
    if (changed != 0) std::cerr << what << ": " << changed << " pages of the epoch " << s.epoch << " tree changed\n";
    CHECK(changed == 0);
    CHECK(got == s.contents);
}

// ------------------------------------------------------------------------------------------
static void testC1CommittedTreeIntact(const std::string& dir) {
    banner("C1 After a checkpoint, changes never touch the committed tree's pages");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string path = dir + "/index.db";
    auto p = Pager::create(path, pagerOpts(256));  // tree + copies exceed 256 frames: evictions write transient pages
    BPTree t(*p);
    auto sbm = newSbm(dir);
    std::mt19937 rng(11);
    Oracle o;
    uint64_t seq = 0;
    for (uint32_t i = 0; i < 3000; ++i) { ++seq; t.put(key(i), ref(seq)); o[key(i)] = seq; }
    Pager::CheckpointStats cs = checkpoint(*p, *sbm, seq);
    Snapshot s1 = takeSnapshot(path, cs.epoch, cs.root, cs.page_count, o);
    const uint64_t frees0 = p->stats().frees;
    randomOps(t, o, rng, seq, 4000, kQuick ? 600 : 3000);
    p->flushDirty();
    checkSnapshotIntact(path, s1, "C1");
    checkTreeEq(t, o, "C1 live tree");
    Pager::Stats st = p->stats();
    std::cout << "epoch 1 tree: root " << s1.root << ", " << s1.page_crc.size() << " pages, " << s1.contents.size()
              << " keys. After " << (kQuick ? 600 : 3000) << " ops: root " << p->rootPageId() << ", page count " << p->pageCount() << ", copies "
              << t.cowCopies() << ", frees " << st.frees - frees0 << ", reuses " << st.reuses << ", evictions "
              << st.evictions << "\n";
    CHECK(p->rootPageId() != s1.root);
    CHECK(t.cowCopies() > 0 && st.frees - frees0 == t.cowCopies());
    CHECK(st.reuses == 0);  // nothing freed in epoch 1 is reusable before epoch 3 commits
    CHECK(st.evictions > 0);
    std::cout << "PASS: all " << s1.page_crc.size() << " pages of the committed tree are unchanged on disk and it still "
                 "reads back as its " << s1.contents.size() << " keys; the live tree matches std::map.\n";
}

static void testC2Cascade(const std::string& dir) {
    banner("C2 Changing one key copies the root-to-leaf path once per epoch");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    auto p = Pager::create(dir + "/index.db", pagerOpts(256));
    BPTree t(*p);
    auto sbm = newSbm(dir);
    uint64_t seq = 0;
    uint32_t n = 0;
    while (p->treeHeight() < 3) { ++seq; t.put(key(n++), ref(seq)); }
    for (uint32_t i = 0; i < 500; ++i) { ++seq; t.put(key(n++), ref(seq)); }
    checkpoint(*p, *sbm, seq);
    const uint32_t h = p->treeHeight();
    const page_id_t root0 = p->rootPageId();
    const uint32_t count0 = p->pageCount();
    uint64_t c0 = t.cowCopies();
    CHECK(t.put(key(7), ref(++seq)) == BPTree::PutStatus::Updated);
    uint64_t first = t.cowCopies() - c0;
    std::cout << "height " << h << ": first update after the checkpoint copied " << first << " pages; root " << root0
              << " -> " << p->rootPageId() << "; page count " << count0 << " -> " << p->pageCount() << "\n";
    CHECK(first == h && p->rootPageId() != root0 && p->pageCount() == count0 + h);
    c0 = t.cowCopies();
    CHECK(t.put(key(8), ref(++seq)) == BPTree::PutStatus::Updated);  // same leaf: already writable
    CHECK(t.cowCopies() == c0);
    CHECK(t.put(key(n - 1), ref(++seq)) == BPTree::PutStatus::Updated);  // other end of the tree
    uint64_t other = t.cowCopies() - c0;
    std::cout << "second update in the same leaf copied 0 pages; an update at the other end copied " << other
              << " (root already copied)\n";
    CHECK(other >= 1 && other <= h - 1);
    CHECK(!t.erase(key(n + 100)));  // absent key: nothing changes, nothing is copied
    CHECK(t.cowCopies() == c0 + other);
    CHECK(t.checkInvariants().empty());
    std::cout << "PASS: the first change copies exactly height (" << h << ") pages; pages copied in this epoch are "
                 "changed in place afterwards; erasing an absent key copies nothing.\n";
}

static void testC3ReuseDelay(const std::string& dir) {
    banner("C3 Page reuse over 6 checkpoints: pages of the last 2 committed trees are never overwritten");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string path = dir + "/index.db";
    auto p = Pager::create(path, pagerOpts(1024));  // no evictions: writes happen at checkpoints
    BPTree t(*p);
    auto sbm = newSbm(dir);
    std::mt19937 rng(33);
    Oracle o;
    uint64_t seq = 0;
    for (uint32_t i = 0; i < kInit; ++i) { ++seq; t.put(key(i), ref(seq)); o[key(i)] = seq; }
    std::vector<Snapshot> snaps;
    Pager::CheckpointStats cs = checkpoint(*p, *sbm, seq);
    snaps.push_back(takeSnapshot(path, cs.epoch, cs.root, cs.page_count, o));
    std::printf("  %-6s %-10s %-11s %-9s %-8s %-7s\n", "epoch", "pages", "tree pages", "free now", "pending", "reuses");
    std::printf("  %-6llu %-10u %-11zu %-9zu %-8zu %-7llu\n", (unsigned long long)cs.epoch, cs.page_count,
                snaps.back().page_crc.size(), p->freeNowCount(), p->pendingFreeCount(),
                (unsigned long long)p->stats().reuses);
    for (int round = 0; round < 5; ++round) {
        randomOps(t, o, rng, seq, kSpace, kRoundOps);
        p->flushDirty();
        // Both superblock slots name these two trees right now.
        checkSnapshotIntact(path, snaps[snaps.size() - 1], "C3 newest committed tree");
        if (snaps.size() >= 2) checkSnapshotIntact(path, snaps[snaps.size() - 2], "C3 previous committed tree");
        cs = checkpoint(*p, *sbm, seq);
        snaps.push_back(takeSnapshot(path, cs.epoch, cs.root, cs.page_count, o));
        // Nothing reusable may belong to either tree named by the two slots.
        std::set<page_id_t> protectedPages;
        for (size_t k = snaps.size() - 2; k < snaps.size(); ++k) {
            for (const auto& [id, crc] : snaps[k].page_crc) protectedPages.insert(id);
        }
        for (page_id_t id : p->freeNowForTest()) CHECK(protectedPages.count(id) == 0);
        std::printf("  %-6llu %-10u %-11zu %-9zu %-8zu %-7llu\n", (unsigned long long)cs.epoch, cs.page_count,
                    snaps.back().page_crc.size(), p->freeNowCount(), p->pendingFreeCount(),
                    (unsigned long long)p->stats().reuses);
    }
    checkTreeEq(t, o, "C3 live tree");
    CHECK(p->stats().reuses > 0);
    std::cout << "PASS: before each checkpoint the two committed trees were byte-identical on disk and read back "
                 "correctly; after each checkpoint the free list held no page of the last 2 trees; "
              << p->stats().reuses << " pages were reused.\n";
}

static void testC4FreeListAtOpen(const std::string& dir) {
    banner("C4 Free list rebuilt at open after a crash: no leaked pages, previous tree still protected");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string path = dir + "/index.db";
    std::mt19937 rng(44);
    Oracle o, o2;
    uint64_t seq = 0;
    Snapshot s1, s2;
    {
        auto p = Pager::create(path, pagerOpts(128));
        BPTree t(*p);
        auto sbm = newSbm(dir);
        for (uint32_t i = 0; i < kInit; ++i) { ++seq; t.put(key(i), ref(seq)); o[key(i)] = seq; }
        Pager::CheckpointStats cs = checkpoint(*p, *sbm, seq);
        s1 = takeSnapshot(path, cs.epoch, cs.root, cs.page_count, o);
        randomOps(t, o, rng, seq, kSpace, kRoundOps);
        cs = checkpoint(*p, *sbm, seq);
        s2 = takeSnapshot(path, cs.epoch, cs.root, cs.page_count, o);
        o2 = o;
        randomOps(t, o, rng, seq, kSpace, kRoundOps);  // epoch 3 work, never committed
        p->flushDirty();
        std::cout << "before the crash: committed page count " << s2.page_count << ", live page count " << p->pageCount()
                  << "\n";
        CHECK(p->pageCount() > s2.page_count);
    }  // crash: no checkpoint
    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0);
    const uint64_t size_before = static_cast<uint64_t>(st.st_size);
    bpt::OpenIndexResult r = bpt::openCommittedIndex(dir, pagerOpts(128));
    CHECK(r.initial_status == OpenStatus::Ok);
    Pager& p = *r.pager;
    CHECK(stat(path.c_str(), &st) == 0);
    std::set<page_id_t> only_prev;
    for (const auto& [id, crc] : s1.page_crc) if (!s2.page_crc.count(id)) only_prev.insert(id);
    std::cout << "at open: file " << size_before / 4096 << " -> " << st.st_size / 4096 << " pages ("
              << p.truncatedPagesAtOpen() << " truncated); committed tree " << s2.page_crc.size()
              << " pages; reserved for the previous tree " << p.pendingFreeCount() << " (expected " << only_prev.size()
              << "); free now " << p.freeNowCount() << "\n";
    CHECK(static_cast<uint64_t>(st.st_size) == uint64_t(s2.page_count) * 4096);
    CHECK(p.truncatedPagesAtOpen() == (size_before / 4096) - s2.page_count);
    CHECK(p.pendingFreeCount() == only_prev.size());
    CHECK(s2.page_crc.size() + p.pendingFreeCount() + p.freeNowCount() == s2.page_count - 1);  // no leaks
    for (page_id_t id : p.freeNowForTest()) CHECK(!s1.page_crc.count(id) && !s2.page_crc.count(id));
    BPTree t(p);
    checkTreeEq(t, o2, "C4 reopened tree");
    // Work + one checkpoint: the previous tree (epoch 1) is no longer named by a slot after epoch 3
    // commits, so its pages become reusable.
    randomOps(t, o2, rng, seq, kSpace, kRoundOps);
    p.flushDirty();
    checkSnapshotIntact(path, s1, "C4 epoch 1 tree before epoch 3 commits");
    checkSnapshotIntact(path, s2, "C4 epoch 2 tree before epoch 3 commits");
    checkpoint(p, *r.sbm, seq);
    std::cout << "after epoch 3 commits: reserved " << p.pendingFreeCount() << ", free now " << p.freeNowCount() << "\n";
    for (page_id_t id : only_prev) {
        std::vector<page_id_t> fn = p.freeNowForTest();
        CHECK(std::find(fn.begin(), fn.end(), id) != fn.end());
    }
    checkTreeEq(t, o2, "C4 after epoch 3");
    std::cout << "PASS: pages past the committed count were truncated; every page is in the tree, reserved, or free "
                 "(no leaks); epoch 1-only pages stayed reserved until epoch 3 committed.\n";
}

int main() {
    std::cout << "====================================================================\n"
              << "  STEP 2: COPY-ON-WRITE TESTS\n"
              << "====================================================================\n";
    std::string dir = setupRun("cow_test");
    Cleaner cleaner{dir};
    testC1CommittedTreeIntact(dir + "/c1");
    testC2Cascade(dir + "/c2");
    testC3ReuseDelay(dir + "/c3");
    testC4FreeListAtOpen(dir + "/c4");
    printBudget();
    std::cout << "\nALL COW TESTS PASSED\n";
    return 0;
}
