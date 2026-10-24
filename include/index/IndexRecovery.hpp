#pragma once
// IndexRecovery.hpp - open the committed index, or rebuild it from the vLog.
//
// Files: <dir>/index.db (pages; page 0 is a static header) and <dir>/superblock.db (two slots,
// SuperblockManager.hpp). The superblock is the single source of truth: it names the root page,
// the page count, the checkpoint seq and the vLog replay point.
//
// Open, in two parts:
//   planIndexOpen (read-only, BEFORE the vLog writer opens): index.db header valid, superblock
//     valid (newest valid slot), superblock nonce == index.db nonce, superblock vLog identity ==
//     this vLog, index.db not shorter than the committed page count, every page of the committed
//     tree passes its CRC and structure checks, and the replay segment exists and is at least
//     replay_offset long (else VLogBehindError: acked entries are gone; nothing is modified).
//     The plan gives the writer its tail-scan start: (replay_segment_id, replay_offset).
//   openIndexFromPlan (after the writer opened): ftruncate index.db to index_page_count (drops
//     pages written after the checkpoint), open the tree at index_root_page, rebuild the free
//     list, and replay the vLog from the replay point with should_apply().
// Copy-on-write (Pager.hpp) keeps the committed tree's pages unchanged until the next superblock
// commits, so after a crash the superblock still names an intact tree.
//
// Rebuild (any check fails):
//   1. delete superblock.db (directory fsynced), so a crash during the rebuild rebuilds again
//   2. unlink a leftover index.db.rebuild, create it (O_CREAT | O_EXCL)
//   3. apply every vLog entry, in file order, through BPTree::apply (should_apply)
//   4. write back all pages, F_FULLFSYNC
//   5. rename index.db.rebuild -> index.db, fsync the directory
//   6. commit superblock epoch 1 (creates superblock.db, fsyncs the directory)
//
// The caller must have opened the vLog first (VLogWriter's constructor truncates a torn tail).

#include "index/BPTree.hpp"
#include "index/IndexCommon.hpp"
#include "index/Pager.hpp"
#include "index/SuperblockManager.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"  // vlog::sync_directory

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

namespace bpt {

struct RecoveryHooks {
    std::function<void(uint64_t entries_applied)> on_rebuild_entry;  // after each entry applied by a rebuild or
                                                                      // recovery replay (crash tests)
    std::function<void()> on_close_after_flush;                       // close checkpoint: after the index fsync
};

// Identity of a vLog directory: id and created_unix_ns of its lowest-id segment.
struct VLogIdentity {
    uint32_t first_segment = 0;
    uint64_t created_ns = 0;
};

struct OpenIndexResult {
    std::unique_ptr<Pager> pager;
    std::unique_ptr<SuperblockManager> sbm;
    OpenStatus initial_status = OpenStatus::Ok;  // what the checks of the committed index found
    std::string why;                             // detail for TreeCorrupt and friends
    bool rebuilt = false;
    uint64_t vlog_entries_scanned = 0;
    uint64_t vlog_entries_applied = 0;  // BPTree::apply returned Applied
    uint64_t rebuild_ms = 0;
    VLogIdentity vid;
    uint64_t checkpoint_seq = 0;  // of the superblock the index was opened from (0 after a rebuild)
    // Newest vLog entry reflected in the tree, and the position right after it.
    uint64_t last_seq = 0;
    uint32_t last_segment = 0;
    uint32_t last_end = vlog::kBlockSize;

    Pager::CommitInfo commitInfo() const {
        Pager::CommitInfo ci;
        ci.checkpoint_seq = last_seq;
        ci.replay_segment = last_segment;
        ci.replay_offset = last_end;
        ci.vlog_first_segment = vid.first_segment;
        ci.vlog_created_ns = vid.created_ns;
        return ci;
    }
};

// Sorted ids of vlog_%08u.log files in dir. Every syscall is checked.
inline std::vector<uint32_t> listVLogSegments(const std::string& vlog_dir) {
    DIR* d = opendir(vlog_dir.c_str());
    if (!d) {
        if (errno == ENOENT) return {};
        fatal("opendir failed on " + vlog_dir);
    }
    std::vector<uint32_t> ids;
    errno = 0;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.size() == 17 && name.rfind("vlog_", 0) == 0 && name.compare(13, 4, ".log") == 0 &&
            std::all_of(name.begin() + 5, name.begin() + 13, [](char c) { return c >= '0' && c <= '9'; })) {
            ids.push_back(static_cast<uint32_t>(std::stoul(name.substr(5, 8))));
        }
        errno = 0;
    }
    int rd_errno = errno;
    if (closedir(d) != 0) fatal("closedir failed on " + vlog_dir);
    if (rd_errno != 0) { errno = rd_errno; fatal("readdir failed on " + vlog_dir); }
    std::sort(ids.begin(), ids.end());
    return ids;
}

inline VLogIdentity readVLogIdentity(const std::string& vlog_dir) {
    VLogIdentity id;
    std::vector<uint32_t> segs = listVLogSegments(vlog_dir);
    if (segs.empty()) return id;
    char name[32];
    std::snprintf(name, sizeof(name), "/vlog_%08u.log", segs[0]);
    std::string path = vlog_dir + name;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) fatal("open failed for " + path);
    vlog::SegmentHeader h;
    ssize_t r = ::pread(fd, &h, sizeof(h), 0);
    if (r < 0) fatal("pread of segment header failed for " + path);
    if (::close(fd) != 0) fatal("close failed for " + path);
    if (r != static_cast<ssize_t>(sizeof(h)) || h.magic != vlog::kSegmentMagic ||
        h.crc32c != vlog::crc32c(&h, offsetof(vlog::SegmentHeader, crc32c))) {
        return id;  // unreadable identity: never matches a recorded one
    }
    id.first_segment = h.segment_id;
    id.created_ns = h.created_unix_ns;
    return id;
}

// Thrown when the committed index reflects vLog entries that the vLog no longer has
// (acked writes missing from the vLog). Index files are left untouched, so every open fails the same way.
class VLogBehindError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------------------------------------
// Committed-tree verification
// ------------------------------------------------------------------------------------------
struct TreeWalk {
    bool ok = false;
    std::string why;
    uint64_t keys = 0;
    uint32_t height = 0;
    uint64_t pages_read = 0;
    std::vector<bool> reachable;  // indexed by page id
};

// Walks the tree at root with plain preads (no cache): every page must pass checkPage, have the
// expected level, valid slot/cell bounds, children inside [1, page_count), and be reached once.
// read_leaves = false: leaf ids are collected from their parents but leaves are not read.
inline TreeWalk walkTree(int fd, page_id_t root, uint32_t page_count, bool read_leaves = true) {
    TreeWalk w;
    w.reachable.assign(page_count, false);
    auto fail = [&](page_id_t id, const std::string& m) {
        w.ok = false;
        w.why = "page " + std::to_string(id) + ": " + m;
        return w;
    };
    std::vector<std::pair<page_id_t, int>> stack{{root, -1}};
    std::vector<uint8_t> buf(kPageSize);
    while (!stack.empty()) {
        auto [id, expect] = stack.back();
        stack.pop_back();
        if (id == kNoPage || id >= page_count) return fail(id, "page id outside [1, " + std::to_string(page_count) + ")");
        if (w.reachable[id]) return fail(id, "reached twice");
        w.reachable[id] = true;
        if (!read_leaves && expect == 0) continue;
        ssize_t r = ::pread(fd, buf.data(), kPageSize, static_cast<off_t>(id) * kPageSize);
        if (r < 0) fatal("pread of page " + std::to_string(id) + " failed");
        if (r != static_cast<ssize_t>(kPageSize)) return fail(id, "short read");
        w.pages_read++;
        std::string why;
        if (!checkPage(buf.data(), id, &why)) {
            w.ok = false;
            w.why = why;  // already starts with "page <id>: "
            return w;
        }
        PageHeader h = loadHeader(buf.data());
        if (h.page_type == static_cast<uint8_t>(PageType::Meta)) return fail(id, "header page inside the tree");
        if (expect >= 0 && h.level != expect) return fail(id, "level " + std::to_string(h.level) + ", expected " + std::to_string(expect));
        if (expect < 0) w.height = h.level + 1u;
        Node n(buf.data());
        const uint32_t vsize = n.valueSize();
        for (uint32_t i = 0; i < h.slot_count; ++i) {
            uint32_t off = n.slot(i);
            if (off < h.cell_start || off + 2 > kPageSize) return fail(id, "slot out of range");
            uint32_t klen = n.keyLen(i);
            if (!validKeyLen(klen) || off + 2 + klen + vsize > kPageSize) return fail(id, "cell out of range");
        }
        if (n.isLeaf()) {
            w.keys += h.slot_count;
        } else {
            if (h.slot_count == 0 || h.leftmost_child == kNoPage) return fail(id, "internal page without children");
            for (uint32_t j = 0; j <= h.slot_count; ++j) stack.emplace_back(n.childAt(j), h.level - 1);
        }
    }
    w.ok = true;
    return w;
}

struct BootCheck {
    OpenStatus status = OpenStatus::Ok;
    std::string why;
    SuperblockManager::LoadResult sb;
    MetaBody header{};
    Pager::Committed committed;
    bool older_tree_ok = false;
    uint64_t reachable_pages = 0;  // pages of the current tree
};

// Read-only checks of index.db + superblock.db. vid == nullptr skips the vLog identity check.
inline BootCheck checkCommittedIndex(const std::string& dir, SuperblockManager& sbm, const VLogIdentity* vid) {
    BootCheck bc;
    const std::string path = dir + "/index.db";
    OpenStatus hs = Pager::readHeader(path, &bc.header);
    bc.sb = sbm.load();
    if (hs != OpenStatus::Ok) { bc.status = hs; return bc; }
    if (bc.sb.status == SuperblockManager::LoadResult::Status::Missing) { bc.status = OpenStatus::NoSuperblock; return bc; }
    if (bc.sb.status == SuperblockManager::LoadResult::Status::Corrupt) {
        bc.status = OpenStatus::SuperblockCorrupt;
        bc.why = std::string("slot 0 ") + toString(bc.sb.slot[0]) + ", slot 1 " + toString(bc.sb.slot[1]);
        return bc;
    }
    const SuperblockRecord& cur = bc.sb.current;
    if (cur.ext.index_nonce != bc.header.index_nonce) { bc.status = OpenStatus::IndexMismatch; return bc; }
    if (vid && (cur.ext.vlog_first_segment != vid->first_segment || cur.ext.vlog_created_ns != vid->created_ns)) {
        bc.status = OpenStatus::VLogMismatch;
        return bc;
    }
    // decodeSlot already checked these bounds; the casts below cannot lose bits.
    const uint32_t page_count = static_cast<uint32_t>(cur.sb.index_page_count);
    const page_id_t root = static_cast<page_id_t>(cur.sb.index_root_page);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) fatal("open failed for " + path);
    struct stat st;
    if (::fstat(fd, &st) != 0) fatal("fstat failed for " + path);
    // Shorter than the committed page count: committed pages are missing. Longer is normal after a
    // crash (pages written after the last checkpoint); Pager::openCommitted truncates them.
    if (static_cast<uint64_t>(st.st_size) < static_cast<uint64_t>(page_count) * kPageSize) {
        if (::close(fd) != 0) fatal("close failed for " + path);
        bc.status = OpenStatus::SizeMismatch;
        bc.why = "file size " + std::to_string(st.st_size) + ", superblock page count " + std::to_string(page_count);
        return bc;
    }
    TreeWalk w = walkTree(fd, root, page_count);
    if (!w.ok) {
        if (::close(fd) != 0) fatal("close failed for " + path);
        bc.status = OpenStatus::TreeCorrupt;
        bc.why = w.why;
        return bc;
    }
    // Free list: every page in [1, page_count) that neither superblock's tree uses. Pages used only
    // by the previous superblock's tree stay reserved until one more epoch commits. Only that
    // tree's internal pages are read (leaf ids come from their parents). If that tree cannot be
    // walked it cannot be recovered either, so it gets no protection.
    std::vector<bool> older_reach;
    if (bc.sb.has_older && bc.sb.older.ext.index_nonce == bc.header.index_nonce &&
        bc.sb.older.sb.index_page_count <= page_count) {
        TreeWalk ow = walkTree(fd, static_cast<page_id_t>(bc.sb.older.sb.index_root_page),
                               static_cast<uint32_t>(bc.sb.older.sb.index_page_count), /*read_leaves=*/false);
        if (ow.ok) older_reach = std::move(ow.reachable);
        bc.older_tree_ok = ow.ok;
    }
    if (::close(fd) != 0) fatal("close failed for " + path);
    for (page_id_t id = 1; id < page_count; ++id) {
        if (w.reachable[id]) continue;
        if (id < older_reach.size() && older_reach[id]) bc.committed.free_older.push_back(id);
        else bc.committed.free_now.push_back(id);
    }
    bc.reachable_pages = 0;
    for (page_id_t id = 1; id < page_count; ++id) bc.reachable_pages += w.reachable[id] ? 1 : 0;
    bc.committed.root = root;
    bc.committed.page_count = page_count;
    bc.committed.height = w.height;
    bc.committed.key_count = w.keys;
    bc.committed.epoch = cur.sb.epoch;
    return bc;
}

inline SuperblockManager::Options sbOpts(const Pager::Options& po) {
    SuperblockManager::Options so;
    so.on_write = po.on_write;
    return so;
}

// Opens the committed index in dir without any vLog (tree-only tests). On failure pager is null
// and initial_status says why.
inline OpenIndexResult openCommittedIndex(const std::string& dir, const Pager::Options& opts) {
    OpenIndexResult res;
    res.sbm.reset(new SuperblockManager(dir + "/superblock.db", sbOpts(opts)));
    BootCheck bc = checkCommittedIndex(dir, *res.sbm, nullptr);
    res.initial_status = bc.status;
    res.why = bc.why;
    if (bc.status != OpenStatus::Ok) return res;
    res.checkpoint_seq = bc.sb.current.sb.checkpoint_seq;
    res.last_seq = res.checkpoint_seq;
    res.last_segment = bc.sb.current.sb.replay_segment_id;
    res.last_end = bc.sb.current.sb.replay_offset;
    res.pager = Pager::openCommitted(dir + "/index.db", opts, bc.committed);
    return res;
}

// Applies vLog entries at or after (from_segment, from_offset) with seq > after_seq, in file order.
// from_segment == 0: from the first segment. When expect_next is true, the first entry found must
// have seq after_seq + 1 (the replay point is the position right after entry after_seq).
inline void replayVLogFrom(const std::string& vlog_dir, BPTree& tree, uint32_t from_segment, uint32_t from_offset,
                           uint64_t after_seq, bool expect_next, const RecoveryHooks& hooks, OpenIndexResult* res) {
    std::vector<uint32_t> segs = listVLogSegments(vlog_dir);
    vlog::VLogReader reader(vlog_dir);
    uint64_t prev_seq = after_seq;
    bool first = true;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (segs[i] < from_segment) continue;
        uint32_t start = segs[i] == from_segment ? from_offset : vlog::kBlockSize;
        vlog::ScanResult sr = reader.scanSegment(segs[i], /*is_last_segment=*/i + 1 == segs.size(), start);
        for (const vlog::RecoveredEntry& e : sr.entries) {
            res->vlog_entries_scanned++;
            const uint64_t seq = e.header.seq;
            if (first && expect_next && seq != after_seq + 1) {
                errno = 0;
                fatal("vLog entry after the replay point has seq " + std::to_string(seq) + ", expected " +
                      std::to_string(after_seq + 1));
            }
            if (!first && seq <= prev_seq) {
                errno = 0;
                fatal("vLog seq not increasing during replay: " + std::to_string(seq) + " after " + std::to_string(prev_seq));
            }
            first = false;
            prev_seq = seq;
            res->last_seq = seq;
            res->last_segment = e.vptr.segment_id;
            res->last_end = e.vptr.offset + e.vptr.length;
            if (seq <= after_seq) continue;
            BPTree::ApplyResult ar = tree.apply(e.key, static_cast<vlog::EntryType>(e.header.type), seq,
                                                e.header.flags, e.vptr);
            if (ar == BPTree::ApplyResult::InvalidKey) {
                errno = 0;
                fatal("vLog holds a key the index cannot store (length " + std::to_string(e.key.size()) + ")");
            }
            if (ar == BPTree::ApplyResult::Applied) res->vlog_entries_applied++;
            if (hooks.on_rebuild_entry) hooks.on_rebuild_entry(res->vlog_entries_applied);
        }
    }
}

struct IndexOpenPlan {
    BootCheck bc;
    VLogIdentity vid;
    std::unique_ptr<SuperblockManager> sbm;
    bool from_checkpoint = false;  // false: full rebuild
    uint64_t checkpoint_seq = 0;
    uint32_t replay_segment = 0;
    uint32_t replay_offset = vlog::kBlockSize;

    // Tail-scan start for VLogWriter (only from a checkpoint; a rebuild scans everything).
    void applyToWriterOptions(vlog::VLogWriter::Options* o) const {
        if (!from_checkpoint) return;
        o->scan_from_segment = replay_segment;
        o->scan_from_offset = replay_offset;
        o->min_seq = checkpoint_seq;
    }
};

// Read-only. Call before the vLog writer opens. Throws VLogBehindError if the replay point is
// missing from the vLog.
inline IndexOpenPlan planIndexOpen(const std::string& dir, const std::string& vlog_dir, const Pager::Options& opts) {
    IndexOpenPlan plan;
    plan.vid = readVLogIdentity(vlog_dir);
    plan.sbm.reset(new SuperblockManager(dir + "/superblock.db", sbOpts(opts)));
    plan.bc = checkCommittedIndex(dir, *plan.sbm, &plan.vid);
    if (plan.bc.status != OpenStatus::Ok) return plan;
    const vlog::Superblock& sb = plan.bc.sb.current.sb;
    plan.from_checkpoint = true;
    plan.checkpoint_seq = sb.checkpoint_seq;
    plan.replay_segment = sb.replay_segment_id;
    plan.replay_offset = sb.replay_offset;
    if (plan.replay_segment != 0) {
        char name[32];
        std::snprintf(name, sizeof(name), "/vlog_%08u.log", plan.replay_segment);
        const std::string seg = vlog_dir + name;
        struct stat st;
        bool ok = ::stat(seg.c_str(), &st) == 0;
        if (!ok && errno != ENOENT) fatal("stat failed for " + seg);
        if (!ok || static_cast<uint64_t>(st.st_size) < plan.replay_offset) {
            throw VLogBehindError("committed index is ahead of the vLog: the superblock's replay point (segment " +
                                  std::to_string(plan.replay_segment) + ", offset " + std::to_string(plan.replay_offset) +
                                  ", checkpoint seq " + std::to_string(plan.checkpoint_seq) + ") is " +
                                  (ok ? "past the end of the segment (" + std::to_string(st.st_size) + " bytes)"
                                      : std::string("in a segment that does not exist")) +
                                  ". Acked writes are missing from the vLog. Nothing was modified; restore the missing "
                                  "segments, or delete index.db to rebuild from the vLog as it is.");
        }
    }
    return plan;
}

// Call after the vLog writer opened (with plan.applyToWriterOptions applied).
inline OpenIndexResult openIndexFromPlan(IndexOpenPlan plan, const std::string& dir, const std::string& vlog_dir,
                                         const Pager::Options& opts, const RecoveryHooks& hooks = {}) {
    const std::string path = dir + "/index.db";
    const std::string tmp = dir + "/index.db.rebuild";
    OpenIndexResult res;
    res.sbm = std::move(plan.sbm);
    res.initial_status = plan.bc.status;
    res.why = plan.bc.why;

    if (plan.from_checkpoint) {
        res.vid = plan.vid;
        res.checkpoint_seq = plan.checkpoint_seq;
        res.last_seq = plan.checkpoint_seq;
        res.last_segment = plan.replay_segment;
        res.last_end = plan.replay_offset;
        res.pager = Pager::openCommitted(path, opts, plan.bc.committed);  // ftruncates to index_page_count
        BPTree tree(*res.pager);
        replayVLogFrom(vlog_dir, tree, plan.replay_segment, plan.replay_offset, plan.checkpoint_seq,
                       /*expect_next=*/true, hooks, &res);
        return res;
    }

    auto t0 = std::chrono::steady_clock::now();
    res.rebuilt = true;
    res.vid = readVLogIdentity(vlog_dir);  // after the writer opened (it may have removed a ghost segment)
    res.sbm->remove();
    if (::unlink(tmp.c_str()) != 0 && errno != ENOENT) fatal("unlink of leftover " + tmp + " failed");
    std::unique_ptr<Pager> np = Pager::create(tmp, opts);
    {
        BPTree tree(*np);
        replayVLogFrom(vlog_dir, tree, 0, vlog::kBlockSize, 0, /*expect_next=*/false, hooks, &res);
    }
    np->flushAndSync();
    if (::rename(tmp.c_str(), path.c_str()) != 0) fatal("rename " + tmp + " -> " + path + " failed");
    if (vlog::sync_directory(dir) != 0) fatal("fsync of directory " + dir + " failed after rename");
    np->setPath(path);
    np->checkpoint(*res.sbm, res.commitInfo());
    res.pager = std::move(np);
    res.rebuild_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return res;
}

// Plan + open for callers that opened the vLog writer themselves (with a full tail scan).
inline OpenIndexResult openOrRebuildIndex(const std::string& dir, const std::string& vlog_dir,
                                          const Pager::Options& opts, const RecoveryHooks& hooks = {}) {
    return openIndexFromPlan(planIndexOpen(dir, vlog_dir, opts), dir, vlog_dir, opts, hooks);
}

} // namespace bpt
