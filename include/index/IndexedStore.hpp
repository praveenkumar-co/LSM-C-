#pragma once
// IndexedStore.hpp - document store: vLog for values + B+Tree index. See docs/PHASE2_PLAN.md section 4.
//
//   put(key, doc): validate -> vLog append (fsynced, group commit) -> apply to the index in seq order -> ack
//   erase(key):    validate -> vLog append Tombstone                -> apply in seq order             -> ack
//   get(key):      index lookup -> vLog read of the referenced entry (CRC checked)
//
// Sequencer: live index updates must be applied in seq order. Otherwise a put with seq 9 that
// applies after a tombstone with seq 10 would bring a deleted key back. After append() returns
// seq s, the thread waits until next_apply_seq == s, applies, then advances next_apply_seq.
// Every assigned seq is either acked or the process aborts, so the sequence has no holes; for
// that reason any exception from append() aborts the process.
//
// Directory layout: <dir>/vlog/vlog_%08u.log, <dir>/index.db, <dir>/superblock.db.
// close() writes back the index and commits a superblock. Without close() there is no superblock
// (Step 1 rule, see IndexRecovery.hpp) and the next open rebuilds the index from the vLog.

#include "index/BPTree.hpp"
#include "index/IndexRecovery.hpp"
#include "index/Pager.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sys/stat.h>

namespace bpt {

// get() could not read a valid entry for a key the index has (missing segment, CRC failure, or an
// entry whose key/seq/type does not match the index). No bytes are returned in that case.
class VLogReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class IndexedStore {
public:
    struct Options {
        size_t cache_frames = 1024;
        vlog::VLogWriter::Options vlog_opts{};
        std::function<void(uint64_t)> on_index_write;  // per index.db page write (tests count bytes)
        RecoveryHooks hooks;                           // crash-test hooks (rebuild, clean close)
        std::function<bool(Pager::IoOp, page_id_t)> inject_index_failure;  // test-only, see Pager::Options
    };

    struct EraseResult {
        uint64_t seq;
        bool existed;  // key was in the index when the tombstone was applied
    };

    static std::unique_ptr<IndexedStore> open(const std::string& dir, Options opts) {
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) fatal("mkdir failed for " + dir);
        std::unique_ptr<IndexedStore> s(new IndexedStore(dir, opts));
        // vLog first: its recovery truncates a torn tail before the index is opened or rebuilt.
        s->writer_.reset(new vlog::VLogWriter(s->vlog_dir_, opts.vlog_opts));
        if (vlog::sync_directory(dir) != 0) fatal("fsync of directory " + dir + " failed");
        Pager::Options po;
        po.num_frames = opts.cache_frames;
        po.on_write = opts.on_index_write;
        po.inject_failure = opts.inject_index_failure;
        s->open_info_ = openOrRebuildIndex(dir, s->vlog_dir_, po, opts.hooks);
        s->pager_ = std::move(s->open_info_.pager);
        s->sbm_ = std::move(s->open_info_.sbm);
        s->tree_.reset(new BPTree(*s->pager_));
        s->next_apply_seq_ = s->writer_->getCurrentSeq() + 1;
        s->last_applied_seq_ = s->open_info_.last_seq;
        s->last_applied_segment_ = s->open_info_.last_segment;
        s->last_applied_end_ = s->open_info_.last_end;
        return s;
    }

    IndexedStore(const IndexedStore&) = delete;
    IndexedStore& operator=(const IndexedStore&) = delete;
    ~IndexedStore() = default;  // without close(): vLog is fsynced and closed, no superblock is written

    // Returns the seq of the acked write. Throws std::invalid_argument for a bad key or an
    // oversize document (nothing is written in that case).
    uint64_t put(std::string_view key, std::string_view doc) {
        checkKey(key);
        // Exact on-disk entry size, computed in 64 bits: header + padded key + padded doc.
        const uint64_t entry_bytes = sizeof(vlog::EntryHeader) + vlog::align_up(static_cast<uint32_t>(key.size()), vlog::kEntryAlign) +
                                     ((static_cast<uint64_t>(doc.size()) + vlog::kEntryAlign - 1) & ~uint64_t(vlog::kEntryAlign - 1));
        if (entry_bytes > vlog::kMaxEntryLen) {
            throw std::invalid_argument("document of " + std::to_string(doc.size()) + " bytes exceeds the " +
                                        std::to_string(vlog::kMaxEntryLen) + "-byte vLog entry limit");
        }
        vlog::AppendResult ar = appendOrDie(vlog::EntryType::Full, key, doc.data(), static_cast<uint32_t>(doc.size()));
        applyInOrder(key, vlog::EntryType::Full, ar, nullptr);
        return ar.seq;
    }

    EraseResult erase(std::string_view key) {
        checkKey(key);
        vlog::AppendResult ar = appendOrDie(vlog::EntryType::Tombstone, key, nullptr, 0);
        bool had = false;
        applyInOrder(key, vlog::EntryType::Tombstone, ar, &had);
        return EraseResult{ar.seq, had};
    }

    // Returns the document, or nullopt if the key is absent. *seq_out gets the entry's seq.
    // Throws VLogReadError if the index references an entry that cannot be read or does not match.
    std::optional<std::string> get(std::string_view key, uint64_t* seq_out = nullptr) const {
        checkKey(key);
        std::optional<vlog::IndexValueRef> ref = tree_->get(key);
        if (!ref) return std::nullopt;
        vlog::VLogReader reader(vlog_dir_);
        vlog::EntryHeader h;
        std::string k;
        std::vector<uint8_t> payload;
        const std::string where = "segment " + std::to_string(ref->vptr.segment_id) + " offset " +
                                  std::to_string(ref->vptr.offset) + " (index seq " + std::to_string(ref->seq) + ")";
        if (!reader.readEntry(ref->vptr, h, k, payload)) {
            throw VLogReadError("vLog entry unreadable (missing segment, bad bounds, or CRC mismatch) at " + where);
        }
        const uint64_t entry_seq = h.seq;
        if (entry_seq != ref->seq || k != key || h.type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) {
            throw VLogReadError("vLog entry does not match the index (seq " + std::to_string(entry_seq) + ") at " + where);
        }
        if (seq_out) *seq_out = entry_seq;
        return std::string(payload.begin(), payload.end());
    }

    BPTree::ScanResult scan(std::string_view start, std::string_view end, size_t limit) const {
        return tree_->scan(start, end, limit);
    }

    // Clean close: callers must have stopped all other operations.
    void close() {
        if (closed_) fatal("IndexedStore::close called twice");
        {
            std::lock_guard<std::mutex> lk(seq_mu_);
            Pager::CommitInfo ci;
            ci.checkpoint_seq = last_applied_seq_;
            ci.replay_segment = last_applied_segment_;
            ci.replay_offset = last_applied_end_;
            ci.vlog_first_segment = open_info_.vid.first_segment;
            ci.vlog_created_ns = open_info_.vid.created_ns;
            pager_->checkpoint(*sbm_, ci, opts_.hooks.on_close_after_flush);
        }
        closed_ = true;
        writer_.reset();  // fsync + close of the current vLog segment
    }

    const OpenIndexResult& openInfo() const { return open_info_; }
    BPTree& tree() { return *tree_; }
    Pager& pager() { return *pager_; }
    uint64_t vlogBytesWritten() const { return writer_ ? writer_->getTotalBytesWritten() : 0; }
    const std::string& vlogDir() const { return vlog_dir_; }

private:
    IndexedStore(const std::string& dir, const Options& opts) : dir_(dir), vlog_dir_(dir + "/vlog"), opts_(opts) {}

    static void checkKey(std::string_view key) {
        if (!validKeyLen(key.size())) {
            throw std::invalid_argument("key length " + std::to_string(key.size()) + " outside [1, 256]");
        }
    }

    vlog::AppendResult appendOrDie(vlog::EntryType type, std::string_view key, const void* data, uint32_t len) {
        if (closed_) fatal("IndexedStore used after close");
        try {
            return writer_->append(type, std::string(key), data, len);
        } catch (const std::exception& e) {
            errno = 0;
            fatal(std::string("vLog append failed: ") + e.what());
        }
    }

    void applyInOrder(std::string_view key, vlog::EntryType type, const vlog::AppendResult& ar, bool* had) {
        std::unique_lock<std::mutex> lk(seq_mu_);
        if (ar.seq < next_apply_seq_) fatal("sequencer: seq " + std::to_string(ar.seq) + " already applied");
        seq_cv_.wait(lk, [&] { return next_apply_seq_ == ar.seq; });
        BPTree::ApplyResult r = tree_->apply(key, type, ar.seq, 0, ar.vptr, had);
        if (r != BPTree::ApplyResult::Applied) {
            errno = 0;
            fatal("live apply was not applied (seq " + std::to_string(ar.seq) + ")");
        }
        next_apply_seq_ = ar.seq + 1;
        last_applied_seq_ = ar.seq;
        last_applied_segment_ = ar.vptr.segment_id;
        last_applied_end_ = ar.vptr.offset + ar.vptr.length;
        lk.unlock();
        seq_cv_.notify_all();
    }

    std::string dir_;
    std::string vlog_dir_;
    Options opts_;
    std::unique_ptr<vlog::VLogWriter> writer_;
    OpenIndexResult open_info_;
    std::unique_ptr<Pager> pager_;
    std::unique_ptr<SuperblockManager> sbm_;
    std::unique_ptr<BPTree> tree_;
    bool closed_ = false;

    std::mutex seq_mu_;
    std::condition_variable seq_cv_;
    uint64_t next_apply_seq_ = 1;
    // Last entry applied to the index and the vLog position right after it (guarded by seq_mu_).
    uint64_t last_applied_seq_ = 0;
    uint32_t last_applied_segment_ = 0;
    uint32_t last_applied_end_ = vlog::kBlockSize;
};

} // namespace bpt
