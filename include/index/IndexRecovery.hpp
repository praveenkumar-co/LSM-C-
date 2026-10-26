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
//     this vLog (the anchor segment the superblock names exists and has the recorded
//     created_unix_ns), index.db not shorter than the committed page count, every page of the committed
//     tree passes its CRC and structure checks, and the replay segment exists and is at least
//     replay_offset long (else VLogBehindError: acked entries are gone; nothing is modified).
//     The plan gives the writer its tail-scan start: (replay_segment_id, replay_offset).
//   openIndexFromPlan (after the writer opened): ftruncate index.db to index_page_count (drops
//     pages written after the checkpoint), open the tree at index_root_page, rebuild the free
//     list, and replay the vLog from the replay point (should_apply(); Delta entries follow the
//     delta rule in BPTree::apply; an entry with kFlagRelocated applies only if the key is at
//     that seq). Before replaying, a read-only pass checks every Delta's base against the
//     committed tree plus the tail, so a bad Delta fails the open (VLogCorruptError) before
//     anything is written. Seqs must rise in file order, except for kFlagRelocated entries (GC
//     copies keep their original seq).
// Copy-on-write (Pager.hpp) keeps the committed tree's pages unchanged until the next superblock
// commits, so after a crash the superblock still names an intact tree.
//
// Rebuild (any check fails):
//   1. unlink a leftover index.db.rebuild, create it (O_CREAT | O_EXCL)
//   2. pass 1: every vLog entry, in any order: per key the highest seq wins; on an equal seq a
//      consolidated Full > relocated Full > original Full > Delta (rebuildWinnerRank). A winning
//      tombstone removes the key and is remembered (seq per erased key) so an older entry met
//      later in the file cannot bring the key back. Pass 2: every key whose winner is a Delta has
//      its chain walked back to a Full base (key, seq == base_seq, depth); a broken chain is the
//      only corruption (VLogCorruptError). Orphan Deltas that lost to a Full are ignored.
//   3. write back all pages, F_FULLFSYNC
//   4. delete superblock.db (directory fsynced)
//   5. rename index.db.rebuild -> index.db, fsync the directory
//   6. commit superblock epoch 1 (creates superblock.db, fsyncs the directory)
// A crash before step 4 leaves the old files, which fail the same check and rebuild again; a
// crash after it leaves no superblock, which also rebuilds. If step 2 finds a bad Delta, the temp
// file is removed and VLogCorruptError is thrown with superblock.db and index.db untouched.
//
// The caller must have opened the vLog first (VLogWriter's constructor truncates a torn tail).

#include "index/BPTree.hpp"
#include "index/DeltaCodec.hpp"
#include "index/IndexCommon.hpp"
#include "index/Pager.hpp"
#include "index/SuperblockManager.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp" // vlog::sync_directory

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

namespace bpt {

struct RecoveryHooks {
    std::function<void(uint64_t entries_applied)>
        on_rebuild_entry;                       // after each entry applied by a rebuild or
                                                // recovery replay (crash tests)
    std::function<void()> on_close_after_flush; // close checkpoint: after the index fsync
};

// Identity of a vLog directory: id and created_unix_ns of one of its segments (the anchor). The
// superblock records the replay segment as anchor; GC never unlinks it, so the check survives
// GC. Superblocks written before Phase 5 name the lowest segment, which still exists in a store
// GC never ran on, so they keep matching.
struct VLogIdentity {
    uint32_t first_segment = 0;
    uint64_t created_ns = 0;
};

struct OpenIndexResult {
    std::unique_ptr<Pager> pager;
    std::unique_ptr<SuperblockManager> sbm;
    OpenStatus initial_status = OpenStatus::Ok; // what the checks of the committed index found
    std::string why;                            // detail for TreeCorrupt and friends
    bool rebuilt = false;
    uint64_t vlog_entries_scanned = 0;
    uint64_t vlog_entries_applied = 0; // BPTree::apply returned Applied
    uint64_t rebuild_ms = 0;
    VLogIdentity vid;
    uint64_t checkpoint_seq = 0; // of the superblock the index was opened from (0 after a rebuild)
    // Newest vLog entry reflected in the tree, and the position right after it.
    uint64_t last_seq = 0;
    uint32_t last_segment = 0;
    uint32_t last_end = vlog::kBlockSize;
    uint32_t oldest_segment = 0; // lowest segment id present at open (after the rebuild's scan)

    Pager::CommitInfo commitInfo() const {
        Pager::CommitInfo ci;
        ci.checkpoint_seq = last_seq;
        ci.replay_segment = last_segment;
        ci.replay_offset = last_end;
        ci.vlog_first_segment = vid.first_segment;
        ci.vlog_created_ns = vid.created_ns;
        ci.oldest_live_segment = oldest_segment;
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
            std::all_of(name.begin() + 5, name.begin() + 13,
                        [](char c) { return c >= '0' && c <= '9'; })) {
            ids.push_back(static_cast<uint32_t>(std::stoul(name.substr(5, 8))));
        }
        errno = 0;
    }
    int rd_errno = errno;
    if (closedir(d) != 0) fatal("closedir failed on " + vlog_dir);
    if (rd_errno != 0) {
        errno = rd_errno;
        fatal("readdir failed on " + vlog_dir);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Identity of segment seg_id. A missing or unreadable segment gives {0, 0}, which never matches
// a recorded identity.
inline VLogIdentity readSegmentIdentity(const std::string& vlog_dir, uint32_t seg_id) {
    VLogIdentity id;
    if (seg_id == 0) return id;
    char name[32];
    std::snprintf(name, sizeof(name), "/vlog_%08u.log", seg_id);
    std::string path = vlog_dir + name;
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return id;
        fatal("open failed for " + path);
    }
    vlog::SegmentHeader h;
    ssize_t r = ::pread(fd, &h, sizeof(h), 0);
    if (r < 0) fatal("pread of segment header failed for " + path);
    if (::close(fd) != 0) fatal("close failed for " + path);
    if (r != static_cast<ssize_t>(sizeof(h)) || h.magic != vlog::kSegmentMagic ||
        h.crc32c != vlog::crc32c(&h, offsetof(vlog::SegmentHeader, crc32c))) {
        return id; // unreadable identity: never matches a recorded one
    }
    if (h.segment_id != seg_id) return id;
    id.first_segment = h.segment_id;
    id.created_ns = h.created_unix_ns;
    return id;
}

// Identity of the lowest-id segment (a new store, or a rebuild with no entries).
inline VLogIdentity readVLogIdentity(const std::string& vlog_dir) {
    std::vector<uint32_t> segs = listVLogSegments(vlog_dir);
    return segs.empty() ? VLogIdentity{} : readSegmentIdentity(vlog_dir, segs[0]);
}

// The anchor a superblock written now should record: the segment holding the replay point, or
// the lowest segment while nothing has been applied.
inline VLogIdentity anchorIdentity(const std::string& vlog_dir, uint32_t replay_segment) {
    return replay_segment != 0 ? readSegmentIdentity(vlog_dir, replay_segment)
                               : readVLogIdentity(vlog_dir);
}

// Thrown when the committed index reflects vLog entries that the vLog no longer has
// (acked writes missing from the vLog). Index files are left untouched, so every open fails the
// same way.
class VLogBehindError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown by open when a Delta entry does not fit the key's history (its base_seq is not the
// key's current seq, the key is absent, or its prefix is unreadable). The store's files are left
// as they were before the open.
class VLogCorruptError : public std::runtime_error {
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
    std::vector<bool> reachable; // indexed by page id
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
        if (id == kNoPage || id >= page_count)
            return fail(id, "page id outside [1, " + std::to_string(page_count) + ")");
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
            w.why = why; // already starts with "page <id>: "
            return w;
        }
        PageHeader h = loadHeader(buf.data());
        if (h.page_type == static_cast<uint8_t>(PageType::Meta))
            return fail(id, "header page inside the tree");
        if (expect >= 0 && h.level != expect)
            return fail(id, "level " + std::to_string(h.level) + ", expected " +
                                std::to_string(expect));
        if (expect < 0) w.height = h.level + 1u;
        Node n(buf.data());
        const uint32_t vsize = n.valueSize();
        for (uint32_t i = 0; i < h.slot_count; ++i) {
            uint32_t off = n.slot(i);
            if (off < h.cell_start || off + 2 > kPageSize) return fail(id, "slot out of range");
            uint32_t klen = n.keyLen(i);
            if (!validKeyLen(klen) || off + 2 + klen + vsize > kPageSize)
                return fail(id, "cell out of range");
        }
        if (n.isLeaf()) {
            w.keys += h.slot_count;
        } else {
            if (h.slot_count == 0 || h.leftmost_child == kNoPage)
                return fail(id, "internal page without children");
            for (uint32_t j = 0; j <= h.slot_count; ++j)
                stack.emplace_back(n.childAt(j), h.level - 1);
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
    uint64_t reachable_pages = 0; // pages of the current tree
};

// Read-only checks of index.db + superblock.db. vlog_dir == nullptr skips the vLog identity
// check; otherwise the anchor segment the superblock names must exist with the recorded
// created_unix_ns (*vid_out gets it).
inline BootCheck checkCommittedIndex(const std::string& dir, SuperblockManager& sbm,
                                     const std::string* vlog_dir, VLogIdentity* vid_out = nullptr) {
    BootCheck bc;
    const std::string path = dir + "/index.db";
    OpenStatus hs = Pager::readHeader(path, &bc.header);
    bc.sb = sbm.load();
    if (hs != OpenStatus::Ok) {
        bc.status = hs;
        return bc;
    }
    if (bc.sb.status == SuperblockManager::LoadResult::Status::Missing) {
        bc.status = OpenStatus::NoSuperblock;
        return bc;
    }
    if (bc.sb.status == SuperblockManager::LoadResult::Status::Corrupt) {
        bc.status = OpenStatus::SuperblockCorrupt;
        bc.why = std::string("slot 0 ") + toString(bc.sb.slot[0]) + ", slot 1 " +
                 toString(bc.sb.slot[1]);
        return bc;
    }
    const SuperblockRecord& cur = bc.sb.current;
    if (cur.ext.index_nonce != bc.header.index_nonce) {
        bc.status = OpenStatus::IndexMismatch;
        return bc;
    }
    if (vlog_dir) {
        const VLogIdentity anchor = readSegmentIdentity(*vlog_dir, cur.ext.vlog_first_segment);
        if (anchor.first_segment == 0 || anchor.first_segment != cur.ext.vlog_first_segment ||
            anchor.created_ns != cur.ext.vlog_created_ns) {
            bc.status = OpenStatus::VLogMismatch;
            return bc;
        }
        if (vid_out) *vid_out = anchor;
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
        bc.why = "file size " + std::to_string(st.st_size) + ", superblock page count " +
                 std::to_string(page_count);
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
        TreeWalk ow =
            walkTree(fd, static_cast<page_id_t>(bc.sb.older.sb.index_root_page),
                     static_cast<uint32_t>(bc.sb.older.sb.index_page_count), /*read_leaves=*/false);
        if (ow.ok) older_reach = std::move(ow.reachable);
        bc.older_tree_ok = ow.ok;
    }
    if (::close(fd) != 0) fatal("close failed for " + path);
    for (page_id_t id = 1; id < page_count; ++id) {
        if (w.reachable[id]) continue;
        if (id < older_reach.size() && older_reach[id])
            bc.committed.free_older.push_back(id);
        else
            bc.committed.free_now.push_back(id);
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

// base_seq of a Delta entry; VLogCorruptError if the payload cannot hold a DeltaPrefix.
inline uint64_t deltaBaseSeq(const vlog::RecoveredEntry& e) {
    vlog::DeltaPrefix p;
    if (!readDeltaPrefix(e.payload, &p))
        throw VLogCorruptError("Delta entry too short for its prefix at segment " +
                               std::to_string(e.vptr.segment_id) + " offset " +
                               std::to_string(e.vptr.offset));
    return p.base_seq;
}

inline std::string deltaMismatchMessage(const vlog::RecoveredEntry& e, uint64_t base_seq,
                                        bool has_key, uint64_t index_seq) {
    const uint64_t seq = e.header.seq;
    return "Delta entry for key \"" + e.key + "\" (seq " + std::to_string(seq) + ", segment " +
           std::to_string(e.vptr.segment_id) + " offset " + std::to_string(e.vptr.offset) +
           ") has base seq " + std::to_string(base_seq) + ", but the key is " +
           (has_key ? "at seq " + std::to_string(index_seq) : std::string("absent")) +
           ". Nothing was modified.";
}

// Read-only pass over the same entries replayVLogFrom would apply: tracks the key -> seq changes
// in memory on top of the tree and throws VLogCorruptError at the first Delta whose base does
// not match. Writes nothing (the tree is only read).
inline void checkDeltaBases(const std::string& vlog_dir, BPTree& tree, uint32_t from_segment,
                            uint32_t from_offset, uint64_t after_seq) {
    std::map<std::string, std::optional<uint64_t>> overlay; // nullopt: key absent
    std::vector<uint32_t> segs = listVLogSegments(vlog_dir);
    vlog::VLogReader reader(vlog_dir);
    for (size_t i = 0; i < segs.size(); ++i) {
        if (segs[i] < from_segment) continue;
        uint32_t start = segs[i] == from_segment ? from_offset : vlog::kBlockSize;
        vlog::ScanResult sr = reader.scanSegment(segs[i], i + 1 == segs.size(), start);
        for (const vlog::RecoveredEntry& e : sr.entries) {
            const uint64_t seq = e.header.seq;
            if (seq <= after_seq) continue;
            std::optional<uint64_t> cur;
            auto it = overlay.find(e.key);
            if (it != overlay.end()) {
                cur = it->second;
            } else if (std::optional<vlog::IndexValueRef> r = tree.get(e.key)) {
                cur = r->seq;
            }
            const auto type = static_cast<vlog::EntryType>(e.header.type);
            if (type == vlog::EntryType::Delta) {
                const uint64_t base_seq = deltaBaseSeq(e);
                if (cur && *cur >= seq) continue;
                if (!cur || *cur != base_seq)
                    throw VLogCorruptError(
                        deltaMismatchMessage(e, base_seq, cur.has_value(), cur ? *cur : 0));
                overlay[e.key] = seq;
            } else if (vlog::should_apply(cur.has_value(), cur ? *cur : 0, seq, e.header.flags)) {
                overlay[e.key] = type == vlog::EntryType::Tombstone ? std::nullopt
                                                                    : std::optional<uint64_t>(seq);
            }
        }
    }
}

// Applies vLog entries at or after (from_segment, from_offset) with seq > after_seq, in file order.
// from_segment == 0: from the first segment. When expect_next is true, the first entry found must
// have seq after_seq + 1 (the replay point is the position right after entry after_seq).
inline void replayVLogFrom(const std::string& vlog_dir, BPTree& tree, uint32_t from_segment,
                           uint32_t from_offset, uint64_t after_seq, bool expect_next,
                           const RecoveryHooks& hooks, OpenIndexResult* res) {
    std::vector<uint32_t> segs = listVLogSegments(vlog_dir);
    vlog::VLogReader reader(vlog_dir);
    uint64_t prev_seq = after_seq;
    bool first = true;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (segs[i] < from_segment) continue;
        uint32_t start = segs[i] == from_segment ? from_offset : vlog::kBlockSize;
        vlog::ScanResult sr =
            reader.scanSegment(segs[i], /*is_last_segment=*/i + 1 == segs.size(), start);
        for (const vlog::RecoveredEntry& e : sr.entries) {
            res->vlog_entries_scanned++;
            const uint64_t seq = e.header.seq;
            const uint8_t flags = e.header.flags;
            res->last_segment = e.vptr.segment_id;
            res->last_end = e.vptr.offset + e.vptr.length;
            if (flags & vlog::kFlagRelocated) {
                // A GC copy keeps its original seq, so it is exempt from the order checks. It
                // applies only if the key is still at that seq (relocation rule).
                const auto rtype = static_cast<vlog::EntryType>(e.header.type);
                if (rtype == vlog::EntryType::Delta)
                    throw VLogCorruptError("Delta entry with kFlagRelocated at segment " +
                                           std::to_string(e.vptr.segment_id) + " offset " +
                                           std::to_string(e.vptr.offset) +
                                           " (GC never relocates a Delta). Nothing was modified.");
                BPTree::ApplyResult ar = tree.apply(e.key, rtype, seq, flags, e.vptr);
                if (ar == BPTree::ApplyResult::InvalidKey) {
                    errno = 0;
                    fatal("vLog holds a key the index cannot store (length " +
                          std::to_string(e.key.size()) + ")");
                }
                if (ar == BPTree::ApplyResult::Applied) res->vlog_entries_applied++;
                if (hooks.on_rebuild_entry) hooks.on_rebuild_entry(res->vlog_entries_applied);
                continue;
            }
            if (first && expect_next && seq != after_seq + 1) {
                errno = 0;
                fatal("vLog entry after the replay point has seq " + std::to_string(seq) +
                      ", expected " + std::to_string(after_seq + 1));
            }
            if (!first && seq <= prev_seq) {
                errno = 0;
                fatal("vLog seq not increasing during replay: " + std::to_string(seq) + " after " +
                      std::to_string(prev_seq));
            }
            first = false;
            prev_seq = seq;
            res->last_seq = seq;
            if (seq <= after_seq) continue;
            const auto type = static_cast<vlog::EntryType>(e.header.type);
            const uint64_t base_seq = type == vlog::EntryType::Delta ? deltaBaseSeq(e) : 0;
            BPTree::ApplyResult ar =
                tree.apply(e.key, type, seq, e.header.flags, e.vptr, nullptr, base_seq);
            if (ar == BPTree::ApplyResult::DeltaBaseMismatch) {
                std::optional<vlog::IndexValueRef> cur = tree.get(e.key);
                throw VLogCorruptError(
                    deltaMismatchMessage(e, base_seq, cur.has_value(), cur ? cur->seq : 0));
            }
            if (ar == BPTree::ApplyResult::InvalidKey) {
                errno = 0;
                fatal("vLog holds a key the index cannot store (length " +
                      std::to_string(e.key.size()) + ")");
            }
            if (ar == BPTree::ApplyResult::Applied) res->vlog_entries_applied++;
            if (hooks.on_rebuild_entry) hooks.on_rebuild_entry(res->vlog_entries_applied);
        }
    }
}

// Tie-break between entries of one key with the same seq (a version and its GC copy):
// consolidated Full 3 > relocated Full 2 > original Full 1 > Delta 0. Tombstones: relocated 2,
// original 1 (a tombstone never shares its seq with a document entry).
inline int rebuildWinnerRank(uint8_t type, uint8_t flags) {
    switch (static_cast<vlog::EntryType>(type)) {
    case vlog::EntryType::Full:
        if (flags & vlog::kFlagConsolidated) return 3;
        return (flags & vlog::kFlagRelocated) ? 2 : 1;
    case vlog::EntryType::Delta:
        return 0;
    case vlog::EntryType::Tombstone:
        return (flags & vlog::kFlagRelocated) ? 2 : 1;
    }
    return -1;
}

// Walks the chain of a key whose winning entry is the Delta at ref, back to its Full base: each
// hop must be readable (CRC), carry this key and the expected seq, and lower the depth by one.
// Throws VLogCorruptError naming the key and the base seq otherwise.
inline void checkWinningChain(vlog::VLogReader& reader, const std::string& key,
                              const vlog::IndexValueRef& ref) {
    uint64_t want_seq = ref.seq;
    vlog::VPtr at = ref.vptr;
    int want_depth = -1;
    uint64_t head_seq = ref.seq, from_seq = ref.seq;
    vlog::VPtr from = ref.vptr;
    auto fail = [&](const std::string& what) {
        throw VLogCorruptError("Delta chain of key \"" + key + "\" (head seq " +
                               std::to_string(head_seq) + "): the Delta with seq " +
                               std::to_string(from_seq) + " at segment " +
                               std::to_string(from.segment_id) + " offset " +
                               std::to_string(from.offset) + " has base seq " +
                               std::to_string(want_seq) + " at segment " +
                               std::to_string(at.segment_id) + " offset " +
                               std::to_string(at.offset) + ", but " + what +
                               ". Nothing was modified.");
    };
    while (true) {
        vlog::EntryHeader h;
        std::string k;
        std::vector<uint8_t> payload;
        if (!reader.readEntry(at, h, k, payload))
            fail("that entry is unreadable (missing segment, bad bounds, or CRC mismatch)");
        const uint64_t seq = h.seq;
        if (k != key || seq != want_seq)
            fail("the entry there is key \"" + k + "\" seq " + std::to_string(seq));
        if (h.type == static_cast<uint8_t>(vlog::EntryType::Full)) {
            if (want_depth > 0) fail("the chain reaches a Full entry early");
            return;
        }
        if (h.type != static_cast<uint8_t>(vlog::EntryType::Delta))
            fail("the entry there is a tombstone");
        vlog::DeltaPrefix p;
        if (!readDeltaPrefix(payload, &p)) fail("the Delta there is too short for its prefix");
        const int depth = p.chain_depth;
        if (depth < 1 || depth > vlog::kMaxDeltaChain ||
            (want_depth >= 0 && depth != want_depth) || p.base_seq >= seq)
            fail("the Delta there has a bad chain depth or base");
        from_seq = seq;
        from = at;
        want_seq = p.base_seq;
        at = p.base;
        want_depth = depth - 1;
    }
}

// Full rebuild into tree (see the header comment, step 2). Independent of file order.
inline void rebuildFromVLog(const std::string& vlog_dir, BPTree& tree, Pager& pager,
                            const RecoveryHooks& hooks, OpenIndexResult* res) {
    std::vector<uint32_t> segs = listVLogSegments(vlog_dir);
    vlog::VLogReader reader(vlog_dir);
    std::map<std::string, uint64_t> erased;  // key -> seq of its winning tombstone
    std::map<std::string, bool> delta_heads; // keys whose current winner is a Delta
    uint64_t max_seq = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        vlog::ScanResult sr = reader.scanSegment(segs[i], i + 1 == segs.size(), vlog::kBlockSize);
        for (const vlog::RecoveredEntry& e : sr.entries) {
            res->vlog_entries_scanned++;
            const uint64_t seq = e.header.seq;
            const uint8_t type = e.header.type, flags = e.header.flags;
            res->last_segment = e.vptr.segment_id;
            res->last_end = e.vptr.offset + e.vptr.length;
            const int rank = rebuildWinnerRank(type, flags);
            if (rank < 0 || (type == static_cast<uint8_t>(vlog::EntryType::Delta) &&
                             (flags & vlog::kFlagRelocated)))
                throw VLogCorruptError("vLog entry of unknown type or a relocated Delta at segment " +
                                       std::to_string(e.vptr.segment_id) + " offset " +
                                       std::to_string(e.vptr.offset) + ". Nothing was modified.");
            if (seq > max_seq) {
                max_seq = seq;
                if (seq > pager.maxAppliedSeq()) pager.setMaxAppliedSeq(seq);
            }
            if (!validKeyLen(e.key.size())) {
                errno = 0;
                fatal("vLog holds a key the index cannot store (length " +
                      std::to_string(e.key.size()) + ")");
            }
            bool wins;
            std::optional<vlog::IndexValueRef> cur = tree.get(e.key);
            if (cur) {
                if (seq != cur->seq) {
                    wins = seq > cur->seq;
                } else {
                    vlog::EntryHeader ch;
                    std::string ck;
                    std::vector<uint8_t> cp;
                    if (!reader.readEntry(cur->vptr, ch, ck, cp)) {
                        errno = 0;
                        fatal("rebuild: cannot re-read an entry it scanned at segment " +
                              std::to_string(cur->vptr.segment_id));
                    }
                    wins = rank > rebuildWinnerRank(ch.type, ch.flags);
                }
            } else {
                auto it = erased.find(e.key);
                wins = it == erased.end() || seq > it->second;
            }
            if (!wins) continue;
            if (type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) {
                if (cur) tree.erase(e.key);
                erased[e.key] = seq;
                delta_heads.erase(e.key);
            } else {
                vlog::IndexValueRef r{};
                r.seq = seq;
                r.kind = 1;
                r.vptr = e.vptr;
                if (tree.put(e.key, r) == BPTree::PutStatus::InvalidKey) {
                    errno = 0;
                    fatal("rebuild: put failed for a valid key");
                }
                erased.erase(e.key);
                if (type == static_cast<uint8_t>(vlog::EntryType::Delta))
                    delta_heads[e.key] = true;
                else
                    delta_heads.erase(e.key);
            }
            res->vlog_entries_applied++;
            if (hooks.on_rebuild_entry) hooks.on_rebuild_entry(res->vlog_entries_applied);
        }
    }
    res->last_seq = max_seq;
    for (const auto& kv : delta_heads) {
        std::optional<vlog::IndexValueRef> ref = tree.get(kv.first);
        if (!ref) {
            errno = 0;
            fatal("rebuild: a Delta winner is missing from the tree");
        }
        checkWinningChain(reader, kv.first, *ref);
    }
}

struct IndexOpenPlan {
    BootCheck bc;
    VLogIdentity vid;
    std::unique_ptr<SuperblockManager> sbm;
    bool from_checkpoint = false; // false: full rebuild
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
inline IndexOpenPlan planIndexOpen(const std::string& dir, const std::string& vlog_dir,
                                   const Pager::Options& opts) {
    IndexOpenPlan plan;
    plan.sbm.reset(new SuperblockManager(dir + "/superblock.db", sbOpts(opts)));
    plan.bc = checkCommittedIndex(dir, *plan.sbm, &vlog_dir, &plan.vid);
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
            throw VLogBehindError(
                "committed index is ahead of the vLog: the superblock's replay point (segment " +
                std::to_string(plan.replay_segment) + ", offset " +
                std::to_string(plan.replay_offset) + ", checkpoint seq " +
                std::to_string(plan.checkpoint_seq) + ") is " +
                (ok ? "past the end of the segment (" + std::to_string(st.st_size) + " bytes)"
                    : std::string("in a segment that does not exist")) +
                ". Acked writes are missing from the vLog. Nothing was modified; restore the "
                "missing "
                "segments, or delete index.db to rebuild from the vLog as it is.");
        }
    }
    return plan;
}

// Call after the vLog writer opened (with plan.applyToWriterOptions applied).
inline OpenIndexResult openIndexFromPlan(IndexOpenPlan plan, const std::string& dir,
                                         const std::string& vlog_dir, const Pager::Options& opts,
                                         const RecoveryHooks& hooks = {}) {
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
        res.pager =
            Pager::openCommitted(path, opts, plan.bc.committed); // ftruncates to index_page_count
        BPTree tree(*res.pager);
        checkDeltaBases(vlog_dir, tree, plan.replay_segment, plan.replay_offset,
                        plan.checkpoint_seq);
        replayVLogFrom(vlog_dir, tree, plan.replay_segment, plan.replay_offset, plan.checkpoint_seq,
                       /*expect_next=*/true, hooks, &res);
        if (res.last_segment != plan.replay_segment)
            res.vid = anchorIdentity(vlog_dir, res.last_segment);
        return res;
    }

    auto t0 = std::chrono::steady_clock::now();
    res.rebuilt = true;
    if (::unlink(tmp.c_str()) != 0 && errno != ENOENT)
        fatal("unlink of leftover " + tmp + " failed");
    std::unique_ptr<Pager> np = Pager::create(tmp, opts);
    try {
        BPTree tree(*np);
        rebuildFromVLog(vlog_dir, tree, *np, hooks, &res);
    } catch (const VLogCorruptError&) {
        np.reset();
        if (::unlink(tmp.c_str()) != 0) fatal("unlink of " + tmp + " failed");
        if (vlog::sync_directory(dir) != 0) fatal("fsync of directory " + dir + " failed");
        throw;
    }
    // After the writer opened (it may have removed a ghost segment).
    res.vid = anchorIdentity(vlog_dir, res.last_segment);
    {
        std::vector<uint32_t> present = listVLogSegments(vlog_dir);
        res.oldest_segment = present.empty() ? 0 : present.front();
    }
    np->flushAndSync();
    res.sbm->remove(); // after the rebuilt index is durable; see the header comment
    if (::rename(tmp.c_str(), path.c_str()) != 0)
        fatal("rename " + tmp + " -> " + path + " failed");
    if (vlog::sync_directory(dir) != 0) fatal("fsync of directory " + dir + " failed after rename");
    np->setPath(path);
    np->checkpoint(*res.sbm, res.commitInfo());
    res.pager = std::move(np);
    res.rebuild_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    return res;
}

// Plan + open for callers that opened the vLog writer themselves (with a full tail scan).
inline OpenIndexResult openOrRebuildIndex(const std::string& dir, const std::string& vlog_dir,
                                          const Pager::Options& opts,
                                          const RecoveryHooks& hooks = {}) {
    return openIndexFromPlan(planIndexOpen(dir, vlog_dir, opts), dir, vlog_dir, opts, hooks);
}

} // namespace bpt
