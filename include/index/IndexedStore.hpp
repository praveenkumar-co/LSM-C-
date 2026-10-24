#pragma once
// IndexedStore.hpp - document store: vLog for values + B+Tree index + checkpoints.
//
//   put(key, doc): validate -> vLog append (fsynced, group commit) -> apply to the index in seq
//   order -> ack erase(key):    validate -> vLog append Tombstone                -> apply in seq
//   order             -> ack get(key):      index lookup -> vLog read of the referenced entry (CRC
//   checked)
//
// Sequencer: live index updates must be applied in seq order. Otherwise a put with seq 9 that
// applies after a tombstone with seq 10 would bring a deleted key back. After append() returns
// seq s, the thread waits until next_apply_seq == s, applies, then advances next_apply_seq.
// Every assigned seq is either acked or the process aborts, so the sequence has no holes; for
// that reason any exception from append() aborts the process.
//
// Checkpoint (checkpoint()): with the sequencer lock held (no index update can start) and the
// tree's exclusive lock held (no reader or writer is in the tree):
//   checkpoint_seq = seq of the last entry applied to the index
//   replay point   = vLog position right after that entry (never the writer's tail: entries
//                    appended but not yet applied have higher seqs and are replayed after a crash)
//   write back dirty pages -> F_FULLFSYNC index.db -> superblock epoch+1 -> F_FULLFSYNC
// Triggers: a background thread checkpoints after checkpoint_bytes of vLog writes (default
// 4 MiB) or checkpoint_interval_ms (default 30 s), and close() checkpoints.
//
// Directory layout: <dir>/vlog/vlog_%08u.log, <dir>/index.db, <dir>/superblock.db.

#include "index/BPTree.hpp"
#include "index/IndexRecovery.hpp"
#include "index/Pager.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/stat.h>

namespace bpt {

// get() could not read a valid entry for a key the index has (missing segment, CRC failure, or an
// entry whose key/seq/type does not match the index). No bytes are returned in that case.
class VLogReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class CheckpointTrigger { Bytes, Timer, Close, Manual };

inline const char* toString(CheckpointTrigger t) {
    switch (t) {
    case CheckpointTrigger::Bytes:
        return "bytes";
    case CheckpointTrigger::Timer:
        return "timer";
    case CheckpointTrigger::Close:
        return "close";
    case CheckpointTrigger::Manual:
        return "manual";
    }
    return "?";
}

struct CheckpointReport {
    CheckpointTrigger trigger = CheckpointTrigger::Manual;
    bool skipped = false; // nothing applied since the last checkpoint (timer/bytes only)
    uint64_t epoch = 0;
    uint64_t checkpoint_seq = 0;
    uint32_t replay_segment = 0;
    uint32_t replay_offset = 0;
    uint64_t pages_written = 0;
    uint64_t bytes_written = 0; // index pages + one 4096-byte superblock slot
    uint32_t page_count = 0;
    double lock_wait_ms = 0; // time to get the sequencer + tree locks
    double stall_ms = 0;     // time the locks were held: index updates and readers wait this long
};

class IndexedStore {
public:
    struct Options {
        size_t cache_frames = 1024;
        vlog::VLogWriter::Options vlog_opts{};
        std::function<void(uint64_t)>
            on_index_write;  // per index.db page / superblock write (tests count bytes)
        RecoveryHooks hooks; // crash-test hooks (rebuild/replay, close checkpoint)
        std::function<bool(Pager::IoOp, page_id_t)>
            inject_index_failure; // test-only, see Pager::Options
        std::function<void(CheckpointTrigger)>
            on_checkpoint_start; // test-only: locks held, before the flush
        std::function<void(CheckpointTrigger)>
            on_checkpoint_after_index_fsync; // test-only, every checkpoint
        bool auto_checkpoint = true;
        uint64_t checkpoint_bytes = 4ULL * 1024 * 1024; // vLog bytes between automatic checkpoints
        uint32_t checkpoint_interval_ms = 30000; // longest time between automatic checkpoints
    };

    struct EraseResult {
        uint64_t seq;
        bool existed; // key was in the index when the tombstone was applied
    };

    static std::unique_ptr<IndexedStore> open(const std::string& dir, Options opts) {
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) fatal("mkdir failed for " + dir);
        std::unique_ptr<IndexedStore> s(new IndexedStore(dir, opts));
        Pager::Options po;
        po.num_frames = opts.cache_frames;
        po.on_write = opts.on_index_write;
        po.inject_failure = opts.inject_index_failure;
        // 1. Read-only checks of superblock + index (may throw VLogBehindError; nothing modified).
        IndexOpenPlan plan = planIndexOpen(dir, s->vlog_dir_, po);
        // 2. vLog writer: its tail scan starts at the superblock's replay point (a rebuild scans
        //    everything). It truncates a torn tail before the index replays the vLog.
        vlog::VLogWriter::Options wo = opts.vlog_opts;
        plan.applyToWriterOptions(&wo);
        s->writer_.reset(new vlog::VLogWriter(s->vlog_dir_, wo));
        if (vlog::sync_directory(dir) != 0) fatal("fsync of directory " + dir + " failed");
        // 3. Open the index at the superblock and replay from the replay point, or rebuild.
        s->open_info_ = openIndexFromPlan(std::move(plan), dir, s->vlog_dir_, po, opts.hooks);
        s->pager_ = std::move(s->open_info_.pager);
        s->sbm_ = std::move(s->open_info_.sbm);
        s->tree_.reset(new BPTree(*s->pager_));
        s->next_apply_seq_ = s->writer_->getCurrentSeq() + 1;
        s->last_applied_seq_ = s->open_info_.last_seq;
        s->last_applied_segment_ = s->open_info_.last_segment;
        s->last_applied_end_ = s->open_info_.last_end;
        s->last_cp_seq_ =
            s->open_info_.rebuilt ? s->open_info_.last_seq : s->open_info_.checkpoint_seq;
        s->last_cp_time_ = std::chrono::steady_clock::now();
        s->vlog_bytes_at_cp_ = 0;
        if (opts.auto_checkpoint)
            s->checkpointer_ = std::thread([p = s.get()] { p->checkpointerLoop(); });
        return s;
    }

    IndexedStore(const IndexedStore&) = delete;
    IndexedStore& operator=(const IndexedStore&) = delete;

    // Without close(): the checkpointer stops without a final checkpoint; the vLog is fsynced and
    // closed. The next open recovers from the last superblock plus the vLog.
    ~IndexedStore() { stopCheckpointer(); }

    // Returns the seq of the acked write. Throws std::invalid_argument for a bad key or an
    // oversize document (nothing is written in that case).
    uint64_t put(std::string_view key, std::string_view doc) {
        checkKey(key);
        // Exact on-disk entry size, computed in 64 bits: header + padded key + padded doc.
        const uint64_t entry_bytes =
            sizeof(vlog::EntryHeader) +
            vlog::align_up(static_cast<uint32_t>(key.size()), vlog::kEntryAlign) +
            ((static_cast<uint64_t>(doc.size()) + vlog::kEntryAlign - 1) &
             ~uint64_t(vlog::kEntryAlign - 1));
        if (entry_bytes > vlog::kMaxEntryLen) {
            throw std::invalid_argument("document of " + std::to_string(doc.size()) +
                                        " bytes exceeds the " + std::to_string(vlog::kMaxEntryLen) +
                                        "-byte vLog entry limit");
        }
        vlog::AppendResult ar =
            appendOrDie(vlog::EntryType::Full, key, doc.data(), static_cast<uint32_t>(doc.size()));
        applyInOrder(key, vlog::EntryType::Full, ar, nullptr);
        noteVLogBytes();
        return ar.seq;
    }

    EraseResult erase(std::string_view key) {
        checkKey(key);
        vlog::AppendResult ar = appendOrDie(vlog::EntryType::Tombstone, key, nullptr, 0);
        bool had = false;
        applyInOrder(key, vlog::EntryType::Tombstone, ar, &had);
        noteVLogBytes();
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
                                  std::to_string(ref->vptr.offset) + " (index seq " +
                                  std::to_string(ref->seq) + ")";
        if (!reader.readEntry(ref->vptr, h, k, payload)) {
            throw VLogReadError(
                "vLog entry unreadable (missing segment, bad bounds, or CRC mismatch) at " + where);
        }
        const uint64_t entry_seq = h.seq;
        if (entry_seq != ref->seq || k != key ||
            h.type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) {
            throw VLogReadError("vLog entry does not match the index (seq " +
                                std::to_string(entry_seq) + ") at " + where);
        }
        if (seq_out) *seq_out = entry_seq;
        return std::string(payload.begin(), payload.end());
    }

    BPTree::ScanResult scan(std::string_view start, std::string_view end, size_t limit) const {
        return tree_->scan(start, end, limit);
    }

    // Makes the index durable up to the last applied entry. Safe to call while other threads use
    // the store. Timer/bytes triggers skip the write if nothing was applied since the last one.
    CheckpointReport checkpoint(CheckpointTrigger trigger = CheckpointTrigger::Manual) {
        if (closed_) fatal("IndexedStore::checkpoint after close");
        std::lock_guard<std::mutex> run_lk(cp_run_mu_); // one checkpoint at a time
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> seq_lk(seq_mu_); // no index update can start
        CheckpointReport rep = tree_->withExclusiveLock([&] {
            const auto t1 = std::chrono::steady_clock::now();
            CheckpointReport r;
            r.trigger = trigger;
            r.lock_wait_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            r.checkpoint_seq = last_applied_seq_;
            r.replay_segment = last_applied_segment_;
            r.replay_offset = last_applied_end_;
            const bool auto_trigger =
                trigger == CheckpointTrigger::Bytes || trigger == CheckpointTrigger::Timer;
            if (auto_trigger && last_applied_seq_ == last_cp_seq_) {
                r.skipped = true;
                r.epoch = pager_->committedEpoch();
                return r;
            }
            if (opts_.on_checkpoint_start) opts_.on_checkpoint_start(trigger);
            Pager::CommitInfo ci;
            ci.checkpoint_seq = last_applied_seq_;
            ci.replay_segment = last_applied_segment_;
            ci.replay_offset = last_applied_end_;
            ci.vlog_first_segment = open_info_.vid.first_segment;
            ci.vlog_created_ns = open_info_.vid.created_ns;
            std::function<void()> hook;
            if (trigger == CheckpointTrigger::Close && opts_.hooks.on_close_after_flush) {
                hook = opts_.hooks.on_close_after_flush;
            } else if (opts_.on_checkpoint_after_index_fsync) {
                hook = [this, trigger] { opts_.on_checkpoint_after_index_fsync(trigger); };
            }
            Pager::CheckpointStats cs = pager_->checkpoint(*sbm_, ci, hook);
            last_cp_seq_ = last_applied_seq_;
            r.epoch = cs.epoch;
            r.pages_written = cs.pages_written;
            r.bytes_written = cs.bytes_written;
            r.page_count = cs.page_count;
            r.stall_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1)
                    .count();
            return r;
        });
        seq_lk.unlock();
        {
            std::lock_guard<std::mutex> lk(cp_mu_);
            last_cp_time_ = std::chrono::steady_clock::now();
            vlog_bytes_at_cp_ = writer_->getTotalBytesWritten();
            cp_bytes_due_ = false;
            if (!rep.skipped) reports_.push_back(rep);
        }
        return rep;
    }

    // Clean close: callers must have stopped all other operations.
    void close() {
        if (closed_) fatal("IndexedStore::close called twice");
        stopCheckpointer();
        checkpoint(CheckpointTrigger::Close);
        closed_ = true;
        writer_.reset(); // fsync + close of the current vLog segment
    }

    std::vector<CheckpointReport> checkpointReports() const {
        std::lock_guard<std::mutex> lk(cp_mu_);
        return reports_;
    }

    const OpenIndexResult& openInfo() const { return open_info_; }
    BPTree& tree() { return *tree_; }
    Pager& pager() { return *pager_; }
    uint64_t vlogBytesWritten() const { return writer_ ? writer_->getTotalBytesWritten() : 0; }
    uint64_t vlogLastSeq() const {
        return writer_->getCurrentSeq();
    } // last seq assigned by the vLog writer
    uint64_t writerRecoveryEntriesScanned() const {
        return writer_ ? writer_->getRecoveryEntriesScanned() : 0;
    }
    const std::string& vlogDir() const { return vlog_dir_; }
    uint64_t lastAppliedSeq() {
        std::lock_guard<std::mutex> lk(seq_mu_);
        return last_applied_seq_;
    }

private:
    IndexedStore(const std::string& dir, const Options& opts)
        : dir_(dir), vlog_dir_(dir + "/vlog"), opts_(opts) {}

    static void checkKey(std::string_view key) {
        if (!validKeyLen(key.size())) {
            throw std::invalid_argument("key length " + std::to_string(key.size()) +
                                        " outside [1, 256]");
        }
    }

    vlog::AppendResult appendOrDie(vlog::EntryType type, std::string_view key, const void* data,
                                   uint32_t len) {
        if (closed_) fatal("IndexedStore used after close");
        try {
            return writer_->append(type, std::string(key), data, len);
        } catch (const std::exception& e) {
            errno = 0;
            fatal(std::string("vLog append failed: ") + e.what());
        }
    }

    void applyInOrder(std::string_view key, vlog::EntryType type, const vlog::AppendResult& ar,
                      bool* had) {
        std::unique_lock<std::mutex> lk(seq_mu_);
        if (ar.seq < next_apply_seq_)
            fatal("sequencer: seq " + std::to_string(ar.seq) + " already applied");
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

    // Wakes the checkpointer once checkpoint_bytes of vLog have been written since the last
    // checkpoint.
    void noteVLogBytes() {
        if (!opts_.auto_checkpoint) return;
        const uint64_t b = writer_->getTotalBytesWritten();
        std::lock_guard<std::mutex> lk(cp_mu_);
        if (!cp_bytes_due_ && b - vlog_bytes_at_cp_ >= opts_.checkpoint_bytes) {
            cp_bytes_due_ = true;
            cp_cv_.notify_all();
        }
    }

    void checkpointerLoop() {
        std::unique_lock<std::mutex> lk(cp_mu_);
        while (!cp_stop_) {
            const auto deadline =
                last_cp_time_ + std::chrono::milliseconds(opts_.checkpoint_interval_ms);
            cp_cv_.wait_until(lk, deadline, [&] { return cp_stop_ || cp_bytes_due_; });
            if (cp_stop_) break;
            CheckpointTrigger t;
            if (cp_bytes_due_) {
                t = CheckpointTrigger::Bytes;
            } else if (std::chrono::steady_clock::now() >=
                       last_cp_time_ + std::chrono::milliseconds(opts_.checkpoint_interval_ms)) {
                t = CheckpointTrigger::Timer;
            } else {
                continue;
            }
            lk.unlock();
            checkpoint(t); // updates last_cp_time_ / vlog_bytes_at_cp_ / cp_bytes_due_
            lk.lock();
        }
    }

    void stopCheckpointer() {
        {
            std::lock_guard<std::mutex> lk(cp_mu_);
            cp_stop_ = true;
        }
        cp_cv_.notify_all();
        if (checkpointer_.joinable()) checkpointer_.join();
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
    uint64_t last_cp_seq_ = 0; // checkpoint_seq of the last superblock (guarded by seq_mu_)

    std::mutex cp_run_mu_;     // serializes checkpoint()
    mutable std::mutex cp_mu_; // guards the fields below
    std::condition_variable cp_cv_;
    bool cp_stop_ = false;
    bool cp_bytes_due_ = false;
    std::chrono::steady_clock::time_point last_cp_time_;
    uint64_t vlog_bytes_at_cp_ = 0;
    std::vector<CheckpointReport> reports_;
    std::thread checkpointer_;
};

} // namespace bpt
