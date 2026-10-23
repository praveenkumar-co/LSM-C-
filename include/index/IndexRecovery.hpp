#pragma once
// IndexRecovery.hpp - open index.db, or rebuild it from the vLog. See docs/PHASE2_PLAN.md section 1.7.
//
// Open:    index.db passes meta checks and clean_shutdown == 1 -> Pager::open (clears the flag and
//          fsyncs), then apply vLog entries with seq > max_applied_seq (normally none).
// Rebuild: anything else (missing, bad meta, clean_shutdown == 0, wrong size):
//          1. unlink a leftover index.db.rebuild, create it (O_CREAT | O_EXCL)
//          2. apply every vLog entry, in file order, through BPTree::apply (should_apply)
//          3. clean-close it (pages fsynced, then meta with clean_shutdown = 1, fsynced)
//          4. rename index.db.rebuild -> index.db, fsync the directory
//          5. open index.db normally
// A crash at any point leaves either the old (dirty) index.db or none, so the next open rebuilds.
//
// The caller must have opened the vLog first (VLogWriter's constructor truncates a torn tail).

#include "index/BPTree.hpp"
#include "index/Pager.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"  // vlog::sync_directory

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

namespace bpt {

struct RecoveryHooks {
    std::function<void(uint64_t entries_applied)> on_rebuild_entry;  // after each applied entry (crash tests)
    std::function<void()> on_close_after_flush;                       // passed to Pager::closeClean
};

struct OpenIndexResult {
    std::unique_ptr<Pager> pager;
    OpenStatus initial_status = OpenStatus::Ok;  // what the first open attempt found
    bool rebuilt = false;
    uint64_t vlog_entries_scanned = 0;
    uint64_t vlog_entries_applied = 0;  // BPTree::apply returned Applied
    uint64_t rebuild_ms = 0;
};

// Sorted ids of vlog_%08u.log files in dir. Every syscall is checked.
inline std::vector<uint32_t> listVLogSegments(const std::string& vlog_dir) {
    DIR* d = opendir(vlog_dir.c_str());
    if (!d) fatal("opendir failed on " + vlog_dir);
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

// Applies every vLog entry with seq > after_seq to the tree, in file order.
// Entry seqs must be strictly increasing (true while there is no GC / relocation).
inline void replayVLog(const std::string& vlog_dir, BPTree& tree, uint64_t after_seq, const RecoveryHooks& hooks,
                       OpenIndexResult* res) {
    std::vector<uint32_t> segs = listVLogSegments(vlog_dir);
    vlog::VLogReader reader(vlog_dir);
    uint64_t prev_seq = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        vlog::ScanResult sr = reader.scanSegment(segs[i], /*is_last_segment=*/i + 1 == segs.size());
        for (const vlog::RecoveredEntry& e : sr.entries) {
            res->vlog_entries_scanned++;
            if (e.header.seq <= prev_seq) {
                errno = 0;
                fatal("vLog seq not increasing during replay: " + std::to_string(e.header.seq) + " after " +
                      std::to_string(prev_seq));
            }
            prev_seq = e.header.seq;
            if (e.header.seq <= after_seq) continue;
            BPTree::ApplyResult ar = tree.apply(e.key, static_cast<vlog::EntryType>(e.header.type), e.header.seq,
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

inline OpenIndexResult openOrRebuildIndex(const std::string& dir, const std::string& vlog_dir,
                                          const Pager::Options& opts, const RecoveryHooks& hooks = {}) {
    const std::string path = dir + "/index.db";
    const std::string tmp = dir + "/index.db.rebuild";
    OpenIndexResult res;
    OpenStatus st;
    res.pager = Pager::open(path, opts, &st);
    res.initial_status = st;
    if (st == OpenStatus::Ok) {
        BPTree tree(*res.pager);
        replayVLog(vlog_dir, tree, res.pager->maxAppliedSeq(), RecoveryHooks{}, &res);
        return res;
    }

    auto t0 = std::chrono::steady_clock::now();
    res.rebuilt = true;
    if (::unlink(tmp.c_str()) != 0 && errno != ENOENT) fatal("unlink of leftover " + tmp + " failed");
    {
        std::unique_ptr<Pager> np = Pager::create(tmp, opts);
        BPTree tree(*np);
        replayVLog(vlog_dir, tree, 0, hooks, &res);
        np->closeClean();
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) fatal("rename " + tmp + " -> " + path + " failed");
    if (vlog::sync_directory(dir) != 0) fatal("fsync of directory " + dir + " failed after rename");
    res.pager = Pager::open(path, opts, &st);
    if (st != OpenStatus::Ok) {
        errno = 0;
        fatal(std::string("rebuilt index.db does not open cleanly: ") + toString(st));
    }
    res.rebuild_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    return res;
}

} // namespace bpt
