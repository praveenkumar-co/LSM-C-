#pragma once
// IndexedStore.hpp - document store: vLog for values + B+Tree index + checkpoints.
//
//   put(key, doc): validate -> vLog append (fsynced, group commit) -> apply to the index in seq
//   order -> ack erase(key):    validate -> vLog append Tombstone                -> apply in seq
//   order             -> ack get(key):      index lookup -> vLog read of the referenced entry (CRC
//   checked)
//
// update(key, ops): stripe lock -> read the current version -> apply ops in memory -> vLog append
//   of a Delta (base = the current version) or of a consolidated Full entry -> apply in seq order
//   -> release. get() follows a Delta chain back to its Full base and applies the ops oldest
//   first; a Full entry is returned byte for byte.
//
// Stripe locks (1024, by key hash): put, erase and update hold their key's stripe from reading
// the current version until their index update is applied, so a Delta's base is always the key's
// current version. Each operation holds one stripe and never asks for another; checkpoint(),
// get() and scan() take none. A thread waiting in the sequencer for seq s holds a stripe, but
// every thread with a lower seq needs only the sequencer and the tree lock, so the lowest
// unapplied seq always proceeds: no deadlock. Writes to one key run one group commit at a time.
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
// GC counters (docs/PHASE5_PLAN.md section 1): per segment, total_bytes (every entry) and
// live_bytes (index heads and their Delta chains), guarded by gc_mu_ (a leaf lock). Every write
// updates them under its stripe lock after its index update: a Full entry or a tombstone kills the
// key's whole previous chain, a Delta kills nothing. chains_ holds the chain VPtrs of keys whose
// head is a Delta, so put() and erase() find the chain to kill without reading the vLog. Open
// rebuilds both from the index (one small read per key, plus one per Delta hop).
//
// Segment lifecycle (plan section 4): a segment whose live data is gone (relocated, or dead) is
// retired: WaitingForCheckpoints with ready_at_epoch = E + 2 (E = committed epoch read under
// seq_mu_ after its last relocation; checkpoint() holds seq_mu_ for its whole run, so checkpoint
// E + 1 contains every relocation, and after E + 2 neither superblock slot can name it). It is
// condemned once that epoch committed, no reader pinned before its retirement is still reading,
// and both slots' replay points are past it, while the condemned list has room (32). The next
// checkpoint writes it into the superblock's condemned list; then it is unlinked and the
// directory fsynced; the checkpoint after that drops it from the list. Open unlinks any segment
// the current slot lists (or below oldest_live_segment) that a crash brought back.
// Readers: get() pins (ReaderRegistry) before it reads the index; pin() lets scan() callers
// keep VPtrs readable.
//
// Directory layout: <dir>/vlog/vlog_%08u.log, <dir>/index.db, <dir>/superblock.db.

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/BPTree.hpp"
#include "index/DeltaCodec.hpp"
#include "index/FdCache.hpp"
#include "index/GcTypes.hpp"
#include "index/IndexRecovery.hpp"
#include "index/Pager.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

static_assert(doc::kMaxDocBytes ==
                  vlog::kMaxEntryLen - sizeof(vlog::EntryHeader) - vlog::kEntryAlign,
              "doc::kMaxDocBytes must be the largest vLog payload (key of 1-8 bytes)");

// What one get() cost.
struct ReadStats {
    uint32_t vlog_reads = 0;  // entries read from the vLog (chain_depth + 1)
    uint16_t chain_depth = 0; // Delta entries between the index and the Full base
};

struct UpdateResult {
    uint64_t seq = 0;     // seq of the written entry, or the current seq if nothing was written
    bool written = false; // false: the ops left the document unchanged; nothing was appended
    bool consolidated = false; // a Full entry with kFlagConsolidated was written instead of a Delta
    uint16_t chain_depth = 0;  // the key's chain depth after this call (0 = Full entry)
};

class IndexedStore {
public:
    static constexpr size_t kStripes = 1024;

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
        // Delta entries allowed in a chain before update() writes a Full entry instead (0: every
        // update writes a Full entry). At most vlog::kMaxDeltaChain.
        uint16_t delta_chain_cap = vlog::kMaxDeltaChain;
        // Largest document put() and update() accept; 0 = the largest payload the vLog accepts.
        uint32_t max_document_bytes = 0;
        // GC (plan sections 2, 3, 6). runGcPass() runs one pass on the caller's thread; with
        // gc_enabled a background thread runs passes every gc_interval_ms until nothing is
        // left to do, and requests the checkpoints that move retired segments on.
        bool gc_enabled = false;
        uint32_t gc_interval_ms = 1000;
        double gc_threshold = 0.5;              // victims: live/total below this, in (0, 1)
        uint64_t gc_batch_bytes = 1024 * 1024;  // relocations per appendRelocated (one fsync)
        uint32_t gc_batch_max_stripes = 32;     // max distinct stripes held by one batch
        uint32_t gc_rate_mb_s = 32;             // GC vLog writes per second (0: no limit)
        // Read path: open vLog segment descriptors kept for get() / update() (LRU).
        size_t fd_cache_size = 64;
        // Test-only hooks.
        std::function<void(uint32_t victim, size_t entries)> on_gc_after_append;
        std::function<void(uint32_t victim)> on_gc_after_apply;
        std::function<void(std::string_view key)> on_get_after_index_read;
        std::function<void(uint32_t segment)> on_gc_before_unlink;
        std::function<void(uint32_t segment)> on_gc_after_unlink_before_dirsync;
        std::function<void(uint32_t victim, size_t stripes)> on_gc_batch_stripes;
    };

    struct EraseResult {
        uint64_t seq;
        bool existed; // key was in the index when the tombstone was applied
    };

    static std::unique_ptr<IndexedStore> open(const std::string& dir, Options opts) {
        if (opts.delta_chain_cap > vlog::kMaxDeltaChain)
            throw std::invalid_argument("delta_chain_cap above " +
                                        std::to_string(vlog::kMaxDeltaChain));
        if (opts.max_document_bytes > doc::kMaxDocBytes)
            throw std::invalid_argument("max_document_bytes above " +
                                        std::to_string(doc::kMaxDocBytes));
        if (!(opts.gc_threshold > 0.0 && opts.gc_threshold < 1.0))
            throw std::invalid_argument("gc_threshold must be in (0, 1)");
        if (opts.gc_batch_bytes == 0) throw std::invalid_argument("gc_batch_bytes must be > 0");
        if (opts.fd_cache_size == 0) throw std::invalid_argument("fd_cache_size must be > 0");
        if (opts.gc_batch_max_stripes == 0)
            throw std::invalid_argument("gc_batch_max_stripes must be > 0");
        if (opts.gc_enabled && opts.gc_interval_ms == 0)
            throw std::invalid_argument("gc_interval_ms must be > 0 with gc_enabled");
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
        const bool from_cp = plan.from_checkpoint;
        const SuperblockRecord cur_slot = plan.bc.sb.current;
        const bool has_older = from_cp && plan.bc.sb.has_older;
        const uint32_t older_replay = has_older ? plan.bc.sb.older.sb.replay_segment_id : 0;
        // Condemned segments a crash brought back (unlinked before the directory fsync, or
        // condemned but not yet unlinked). Only with a valid superblock.
        if (from_cp) s->gc_open_removed_ = s->removeCondemnedAtOpen(cur_slot);
        const std::vector<uint32_t> segments_before = listVLogSegments(s->vlog_dir_);
        s->writer_.reset(new vlog::VLogWriter(s->vlog_dir_, wo));
        if (vlog::sync_directory(dir) != 0) fatal("fsync of directory " + dir + " failed");
        // 3. Open the index at the superblock and replay from the replay point, or rebuild.
        try {
            s->open_info_ = openIndexFromPlan(std::move(plan), dir, s->vlog_dir_, po, opts.hooks);
        } catch (const VLogCorruptError&) {
            // Leave the files as they were: drop the empty segment the writer created on open.
            s->writer_.reset();
            s->removeSegmentsCreatedByOpen(segments_before);
            throw;
        }
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
        s->anchor_ = s->open_info_.vid;
        s->rebuildCounters();
        s->gc_open_.segments_removed = s->gc_open_removed_;
        s->cp_replay_cur_ = from_cp ? cur_slot.sb.replay_segment_id : s->open_info_.last_segment;
        s->cp_has_older_ = has_older;
        s->cp_replay_older_ = older_replay;
        if (opts.auto_checkpoint)
            s->checkpointer_ = std::thread([p = s.get()] { p->checkpointerLoop(); });
        if (opts.gc_enabled) s->gc_thread_ = std::thread([p = s.get()] { p->gcLoop(); });
        return s;
    }

    IndexedStore(const IndexedStore&) = delete;
    IndexedStore& operator=(const IndexedStore&) = delete;

    // Without close(): the checkpointer stops without a final checkpoint; the vLog is fsynced and
    // closed. The next open recovers from the last superblock plus the vLog.
    ~IndexedStore() {
        stopGc();
        stopCheckpointer();
    }

    // Returns the seq of the acked write. Throws std::invalid_argument for a bad key or an
    // oversize document (nothing is written in that case).
    uint64_t put(std::string_view key, std::string_view doc) {
        checkKey(key);
        if (doc.size() > maxDocBytes(key)) {
            throw std::invalid_argument("document of " + std::to_string(doc.size()) +
                                        " bytes exceeds the " + std::to_string(maxDocBytes(key)) +
                                        "-byte limit");
        }
        std::lock_guard<std::mutex> stripe(stripeFor(key));
        vlog::AppendResult ar =
            appendOrDie(vlog::EntryType::Full, key, doc.data(), static_cast<uint32_t>(doc.size()));
        // After the append: reading the index waits while a checkpoint holds the tree, and the
        // append must not wait for that. The stripe keeps the key's chain unchanged until here.
        const std::vector<vlog::VPtr> old = currentChain(key);
        applyInOrder(key, vlog::EntryType::Full, ar, nullptr);
        accountReplace(key, old, &ar.vptr);
        noteVLogBytes();
        return ar.seq;
    }

    EraseResult erase(std::string_view key) {
        checkKey(key);
        std::lock_guard<std::mutex> stripe(stripeFor(key));
        vlog::AppendResult ar = appendOrDie(vlog::EntryType::Tombstone, key, nullptr, 0);
        const std::vector<vlog::VPtr> old = currentChain(key); // after the append, as in put()
        bool had = false;
        applyInOrder(key, vlog::EntryType::Tombstone, ar, &had);
        accountReplace(key, old, nullptr);
        noteVLogBytes();
        return EraseResult{ar.seq, had};
    }

    // Applies ops to the key's document and appends the change: a Delta entry, or a Full entry
    // with kFlagConsolidated when the chain would exceed delta_chain_cap or the Delta payload
    // would be more than half the new document. Writes nothing if the document does not change.
    // Throws (writing nothing): std::invalid_argument (bad key, no ops), doc::NotFoundError,
    // doc::TypeError (stored value is not a JSON object, or a type error in an op),
    // doc::ConflictError, doc::RangeError, doc::PathError, VLogReadError (stored chain unreadable).
    UpdateResult update(std::string_view key, const std::vector<doc::UpdateOp>& ops) {
        checkKey(key);
        if (ops.empty()) throw std::invalid_argument("update() with no ops");
        if (ops.size() > UINT16_MAX) throw doc::RangeError("more than 65535 ops in one update");
        (void)doc::validateOps(ops); // path and conflict errors before taking the lock
        std::lock_guard<std::mutex> stripe(stripeFor(key));
        ReadGuard guard(&readers_);
        std::optional<vlog::IndexValueRef> ref = tree_->get(key);
        if (!ref) throw doc::NotFoundError("update(): key not found");
        Version v = readVersion(key, *ref);
        doc::Value cur;
        if (v.deltas.empty()) {
            try {
                cur = doc::parse(std::string_view(
                    reinterpret_cast<const char*>(v.full_payload.data()), v.full_payload.size()));
            } catch (const doc::ParseError& e) {
                throw doc::TypeError(std::string("stored value is not JSON: ") + e.what());
            }
            if (!cur.isObject()) throw doc::TypeError("stored value is not a JSON object");
        } else {
            cur = materialize(v, key, *ref);
        }
        doc::Value next = cur;
        doc::applyOps(next, ops);
        std::string next_bytes = doc::serialize(next);
        if (next_bytes == doc::serialize(cur)) return UpdateResult{ref->seq, false, false, v.depth};
        if (next_bytes.size() > maxDocBytes(key)) {
            throw doc::RangeError("updated document of " + std::to_string(next_bytes.size()) +
                                  " bytes exceeds the " + std::to_string(maxDocBytes(key)) +
                                  "-byte limit");
        }
        const uint16_t depth = static_cast<uint16_t>(v.depth + 1);
        const std::string enc = encodeOps(ops);
        const uint64_t delta_payload = sizeof(vlog::DeltaPrefix) + enc.size();
        UpdateResult r;
        if (depth > opts_.delta_chain_cap || delta_payload * 2 > next_bytes.size()) {
            vlog::AppendResult ar =
                appendOrDie(vlog::EntryType::Full, key, next_bytes.data(),
                            static_cast<uint32_t>(next_bytes.size()), vlog::kFlagConsolidated);
            applyInOrder(key, vlog::EntryType::Full, ar, nullptr);
            accountReplace(key, v.chain, &ar.vptr);
            r = UpdateResult{ar.seq, true, true, 0};
        } else {
            vlog::DeltaPrefix pre{};
            pre.base_seq = ref->seq;
            pre.base = ref->vptr;
            pre.chain_depth = depth;
            pre.op_count = static_cast<uint16_t>(ops.size());
            vlog::AppendResult ar = appendOrDie(vlog::EntryType::Delta, key, enc.data(),
                                                static_cast<uint32_t>(enc.size()), 0, &pre);
            applyInOrder(key, vlog::EntryType::Delta, ar, nullptr, ref->seq);
            accountDelta(key, v.chain, ar.vptr);
            r = UpdateResult{ar.seq, true, false, depth};
        }
        noteVLogBytes();
        return r;
    }

    // Returns the document, or nullopt if the key is absent. *seq_out gets the index seq, *stats
    // the read cost. A Full entry is returned byte for byte; a Delta chain is applied to its Full
    // base and returned as canonical JSON. Throws VLogReadError if any entry of the chain cannot
    // be read, does not match, or does not apply (no bytes are returned in that case).
    std::optional<std::string> get(std::string_view key, uint64_t* seq_out = nullptr,
                                   ReadStats* stats = nullptr) const {
        checkKey(key);
        ReadGuard guard(&readers_); // before the index read (plan section 4)
        std::optional<vlog::IndexValueRef> ref = tree_->get(key);
        if (!ref) return std::nullopt;
        if (opts_.on_get_after_index_read) opts_.on_get_after_index_read(key);
        Version v = readVersion(key, *ref);
        if (stats) *stats = ReadStats{v.reads, v.depth};
        if (seq_out) *seq_out = ref->seq;
        if (v.deltas.empty()) return std::string(v.full_payload.begin(), v.full_payload.end());
        return doc::serialize(materialize(v, key, *ref));
    }

    // The VPtrs returned stay readable only while the caller holds a pin() taken before the scan.
    BPTree::ScanResult scan(std::string_view start, std::string_view end, size_t limit) const {
        return tree_->scan(start, end, limit);
    }

    // Keeps every segment the index references now on disk until the guard is released.
    ReadGuard pin() const { return ReadGuard(&readers_); }

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
            // vLog identity anchor: the replay segment (GC never unlinks it).
            if (last_applied_segment_ != 0 && anchor_.first_segment != last_applied_segment_) {
                VLogIdentity a = readSegmentIdentity(vlog_dir_, last_applied_segment_);
                if (a.first_segment == 0) {
                    errno = 0;
                    fatal("checkpoint: replay segment " + std::to_string(last_applied_segment_) +
                          " has no readable header");
                }
                anchor_ = a;
            }
            ci.vlog_first_segment = anchor_.first_segment;
            ci.vlog_created_ns = anchor_.created_ns;
            ci.oldest_live_segment = oldestSegment();
            {
                std::lock_guard<std::mutex> gl(gc_mu_);
                ci.condemned = condemned_;
            }
            std::function<void()> hook;
            if (trigger == CheckpointTrigger::Close && opts_.hooks.on_close_after_flush) {
                hook = opts_.hooks.on_close_after_flush;
            } else if (opts_.on_checkpoint_after_index_fsync) {
                hook = [this, trigger] { opts_.on_checkpoint_after_index_fsync(trigger); };
            }
            Pager::CheckpointStats cs = pager_->checkpoint(*sbm_, ci, hook);
            last_cp_seq_ = last_applied_seq_;
            {
                std::lock_guard<std::mutex> gl(gc_mu_);
                for (uint32_t id : ci.condemned) {
                    auto it = segs_.find(id);
                    if (it != segs_.end() && it->second.condemned_epoch == 0)
                        it->second.condemned_epoch = cs.epoch;
                }
                cp_has_older_ = true;
                cp_replay_older_ = cp_replay_cur_;
                cp_replay_cur_ = ci.replay_segment;
            }
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
        stopGc();
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

    // GC counters and totals (plan section 1). The segment the writer appends to is Active.
    GcStats gcStats() const {
        const uint32_t active = writer_ ? writer_->getCurrentSegmentId() : 0;
        std::lock_guard<std::mutex> lk(gc_mu_);
        GcStats g = gc_totals_;
        g.segments.clear();
        g.total_bytes = g.live_bytes = g.disk_bytes = 0;
        for (const auto& [id, st] : segs_) {
            SegmentInfo si{id, st.total_bytes, st.live_bytes, st.state, st.kept_tombstone_bytes};
            if (id == active) si.state = SegmentState::Active;
            g.segments.push_back(si);
            g.total_bytes += st.total_bytes;
            g.live_bytes += st.live_bytes;
            g.disk_bytes += st.total_bytes + vlog::kBlockSize;
        }
        g.space_amplification = g.live_bytes ? double(g.disk_bytes) / double(g.live_bytes) : 0.0;
        return g;
    }
    const GcOpenStats& gcOpenStats() const { return gc_open_; }

    // One GC pass (plan sections 2-4): advance segment lifecycles (unlink what a committed
    // superblock condemned, condemn what is ready), then pick at most one victim and either retire
    // it without copying (fast path, after a verification scan) or relocate its live data in
    // batches and retire it. Safe while other threads use the store; one pass at a time.
    GcReport runGcPass() {
        std::lock_guard<std::mutex> run(gc_run_mu_);
        const auto t0 = std::chrono::steady_clock::now();
        GcReport rep;
        advanceLifecycle(&rep);
        bool fast = false;
        const uint32_t victim = pickVictim(&fast, &rep.victim_live_ratio);
        if (victim != 0) {
            rep.victim = victim;
            collectVictim(victim, fast, &rep);
        }
        rep.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                     .count();
        std::lock_guard<std::mutex> lk(gc_mu_);
        gc_totals_.passes++;
        return rep;
    }

    // Stops the background GC thread (no-op without gc_enabled). close() calls it. A pass in
    // progress finishes first; its rate-limit sleep is cut short.
    void stopGc() {
        {
            std::lock_guard<std::mutex> lk(gc_thread_mu_);
            gc_stop_ = true;
        }
        gc_cv_.notify_all();
        if (gc_thread_.joinable()) gc_thread_.join();
    }

    // True while some segment waits for checkpoints, or is condemned but not yet unlinked.
    bool gcPending() const {
        std::lock_guard<std::mutex> lk(gc_mu_);
        for (const auto& [id, st] : segs_)
            if (st.state == SegmentState::WaitingForCheckpoints ||
                st.state == SegmentState::Condemned || st.state == SegmentState::Migrating)
                return true;
        return false;
    }

    // Test-only: overwrite a segment's live_bytes (to show the verification scan catches it).
    void setLiveBytesForTest(uint32_t id, uint64_t live) {
        std::lock_guard<std::mutex> lk(gc_mu_);
        segs_[id].live_bytes = live;
    }

    // Test-only: retires a Sealed segment that holds nothing live (as GC does after relocating
    // it). Throws std::logic_error if it is not eligible.
    void retireSegmentForTest(uint32_t id) {
        {
            std::lock_guard<std::mutex> lk(gc_mu_);
            auto it = segs_.find(id);
            if (it == segs_.end() || it->second.state != SegmentState::Sealed ||
                it->second.live_bytes != 0)
                throw std::logic_error("segment " + std::to_string(id) + " cannot be retired");
        }
        if (!sealedAndApplied(id))
            throw std::logic_error("segment " + std::to_string(id) + " is active or unapplied");
        retireSegment(id);
    }
    // Test-only: one lifecycle step (unlink what a committed superblock condemned, condemn what is
    // ready).
    GcReport advanceLifecycleForTest() {
        GcReport r;
        advanceLifecycle(&r);
        return r;
    }
    SegStats segStatsForTest(uint32_t id) const {
        std::lock_guard<std::mutex> lk(gc_mu_);
        auto it = segs_.find(id);
        return it == segs_.end() ? SegStats{} : it->second;
    }
    std::vector<uint32_t> condemnedForTest() const {
        std::lock_guard<std::mutex> lk(gc_mu_);
        return condemned_;
    }
    uint64_t activeReadPinsForTest() const { return readers_.activePins(); }
    const SegmentFdCache& fdCacheForTest() const { return fds_; }

    // Test-only: recomputes the counters from the index and the segment files and compares them
    // with the in-memory ones. "" if equal, else the first difference. Callers must have stopped
    // all writes.
    std::string verifyCountersForTest() const {
        CounterScan cs = scanCounters();
        std::lock_guard<std::mutex> lk(gc_mu_);
        for (const auto& [id, st] : segs_) {
            const uint64_t file_total = segmentEntryBytes(id);
            if (st.total_bytes != file_total)
                return "segment " + std::to_string(id) + " total_bytes " +
                       std::to_string(st.total_bytes) + ", file has " + std::to_string(file_total);
            auto it = cs.live.find(id);
            const uint64_t want = it == cs.live.end() ? 0 : it->second;
            if (st.live_bytes != want)
                return "segment " + std::to_string(id) + " live_bytes " +
                       std::to_string(st.live_bytes) + ", recount " + std::to_string(want);
        }
        for (const auto& [id, live] : cs.live)
            if (!segs_.count(id)) return "live bytes in untracked segment " + std::to_string(id);
        for (uint32_t id : listVLogSegments(vlog_dir_))
            if (!segs_.count(id)) return "segment " + std::to_string(id) + " is not tracked";
        if (cs.chains.size() != chains_.size())
            return "chains: " + std::to_string(chains_.size()) + " tracked, recount " +
                   std::to_string(cs.chains.size());
        for (const auto& [k, c] : cs.chains) {
            auto it = chains_.find(k);
            if (it == chains_.end() || it->second.size() != c.size()) return "chain of " + k;
            for (size_t i = 0; i < c.size(); ++i)
                if (std::memcmp(&c[i], &it->second[i], sizeof(vlog::VPtr)) != 0)
                    return "chain of " + k;
        }
        return "";
    }
    BPTree& tree() { return *tree_; }
    Pager& pager() { return *pager_; }
    uint64_t vlogBytesWritten() const { return writer_ ? writer_->getTotalBytesWritten() : 0; }
    // Group commit: entries per write + F_FULLFSYNC batch, and the number of those fsyncs.
    double vlogAverageBatch() const { return writer_ ? writer_->getAverageBatchSize() : 0.0; }
    uint64_t vlogFsyncs() const { return writer_ ? writer_->getTotalFsyncs() : 0; }
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

    // The entry the index points at and, for a Delta, the chain back to its Full base.
    struct Version {
        uint16_t depth = 0;                // Delta entries in the chain
        uint32_t reads = 0;                // vLog entries read
        std::vector<uint8_t> full_payload; // the Full base entry's payload
        std::vector<DecodedDelta> deltas;  // newest first
        std::vector<vlog::VPtr> chain;     // every entry read, head first (the Full base last)
    };

    // ---- Segment lifecycle (plan section 4) ----

    // True if id is below the writer's segment and below the segment of the last applied entry
    // (normal entries apply in seq order = file order, so every entry in id is applied).
    bool sealedAndApplied(uint32_t id) {
        const uint32_t active = writer_->getCurrentSegmentId();
        uint32_t applied_seg;
        {
            std::lock_guard<std::mutex> lk(seq_mu_);
            applied_seg = last_applied_segment_;
        }
        return id < active && id < applied_seg;
    }

    // Call after the segment's last relocation is applied (or when it held nothing live).
    void retireSegment(uint32_t id) {
        uint64_t e;
        {
            std::lock_guard<std::mutex> lk(seq_mu_); // checkpoint() holds it for its whole run
            e = pager_->committedEpoch();
        }
        const uint64_t retire_epoch = readers_.retire();
        std::lock_guard<std::mutex> lk(gc_mu_);
        SegStats& st = segs_[id];
        st.state = SegmentState::WaitingForCheckpoints;
        st.ready_at_epoch = e + 2;
        st.retire_epoch = retire_epoch;
    }

    void advanceLifecycle(GcReport* rep) {
        const uint64_t committed = pager_->committedEpoch();
        // 1. Unlink segments a committed superblock lists as condemned.
        std::vector<uint32_t> to_unlink;
        {
            std::lock_guard<std::mutex> lk(gc_mu_);
            for (const auto& [id, st] : segs_)
                if (st.state == SegmentState::Condemned && st.condemned_epoch != 0 &&
                    st.condemned_epoch <= committed)
                    to_unlink.push_back(id);
        }
        for (uint32_t id : to_unlink) {
            if (opts_.on_gc_before_unlink) opts_.on_gc_before_unlink(id);
            fds_.invalidate(id); // closes once no reader holds it
            const std::string path = segmentPath(id);
            if (::unlink(path.c_str()) != 0 && errno != ENOENT) fatal("unlink failed for " + path);
            if (opts_.on_gc_after_unlink_before_dirsync) opts_.on_gc_after_unlink_before_dirsync(id);
            if (vlog::sync_directory(vlog_dir_) != 0)
                fatal("fsync of directory " + vlog_dir_ + " failed after unlinking " + path);
            std::lock_guard<std::mutex> lk(gc_mu_);
            segs_.erase(id); // gone from now on (the oldest-segment rule may move past it)
            condemned_.erase(std::remove(condemned_.begin(), condemned_.end(), id),
                             condemned_.end());
            gc_totals_.segments_unlinked++;
            rep->unlinked++;
        }
        // 2. Condemn retired segments that are ready, while the list has room.
        std::vector<std::pair<uint32_t, uint64_t>> waiting; // id, retire_epoch
        {
            std::lock_guard<std::mutex> lk(gc_mu_);
            for (const auto& [id, st] : segs_)
                if (st.state == SegmentState::WaitingForCheckpoints &&
                    committed >= st.ready_at_epoch && cp_replay_cur_ > id &&
                    (!cp_has_older_ || cp_replay_older_ > id))
                    waiting.emplace_back(id, st.retire_epoch);
        }
        for (const auto& [id, retire_epoch] : waiting) {
            if (readers_.pinnedBefore(retire_epoch)) continue; // a reader may hold a VPtr into it
            std::lock_guard<std::mutex> lk(gc_mu_);
            if (condemned_.size() >= kMaxCondemned) break;
            SegStats& st = segs_[id];
            st.state = SegmentState::Condemned;
            st.condemned_epoch = 0; // set by the checkpoint that writes the list
            condemned_.push_back(id);
            rep->condemned++;
        }
    }

    // ---- GC pass (plan sections 2, 3) ----

    // Eligible: Sealed, below the writer's segment and below the segment of the last applied entry
    // (so every entry in it is applied). Fast path: the lowest such id with live_bytes == 0 and
    // no kept tombstones. Otherwise the lowest ratio (live + kept tombstone bytes) / total below
    // gc_threshold; ties: the lowest id. 0: none. In the oldest segment kept tombstones count as
    // free: they are dropped there.
    uint32_t pickVictim(bool* fast, double* ratio) {
        const uint32_t active = writer_->getCurrentSegmentId();
        uint32_t applied_seg;
        {
            std::lock_guard<std::mutex> lk(seq_mu_);
            applied_seg = last_applied_segment_;
        }
        std::lock_guard<std::mutex> lk(gc_mu_);
        uint32_t best = 0;
        double best_ratio = 2.0;
        const uint32_t oldest = segs_.empty() ? 0 : segs_.begin()->first;
        for (const auto& [id, st] : segs_) {
            if (id >= active || id >= applied_seg) break; // ascending: the rest are newer
            if (st.state != SegmentState::Sealed) continue;
            const uint64_t kept = id == oldest ? 0 : st.kept_tombstone_bytes;
            if (st.live_bytes == 0 && kept == 0) {
                *fast = true;
                *ratio = 0;
                return id;
            }
            const double r = double(st.live_bytes + kept) / double(st.total_bytes);
            if (r < opts_.gc_threshold && r < best_ratio) {
                best = id;
                best_ratio = r;
            }
        }
        *fast = false;
        *ratio = best ? best_ratio : 0;
        return best;
    }

    // Is the entry at p (of key) the key's head or in its live chain? Caller holds the stripe.
    bool entryIsLive(const std::string& key, const vlog::VPtr& p) const {
        for (const vlog::VPtr& c : currentChain(key))
            if (std::memcmp(&c, &p, sizeof(p)) == 0) return true;
        return false;
    }

    bool isOldestSegment(uint32_t id) const {
        std::lock_guard<std::mutex> lk(gc_mu_);
        return !segs_.empty() && segs_.begin()->first == id;
    }

    struct VictimKey {
        std::vector<size_t> entries;         // indexes into the scan, in file order
        bool has_tombstone = false;
        size_t tombstone = 0;                // the tombstone with the highest seq
        uint64_t bytes = 0;
    };

    void collectVictim(uint32_t victim, bool fast, GcReport* rep) {
        {
            std::lock_guard<std::mutex> lk(gc_mu_);
            segs_[victim].state = SegmentState::Migrating;
        }
        vlog::VLogReader reader(vlog_dir_);
        vlog::ScanResult sr = reader.scanSegment(victim, /*is_last_segment=*/false);
        rep->entries_scanned = sr.entries.size();
        rep->bytes_scanned = segmentEntryBytes(victim);
        const bool oldest = isOldestSegment(victim);
        std::vector<std::string> order; // keys in file order of first appearance
        std::unordered_map<std::string, VictimKey> keys;
        for (size_t i = 0; i < sr.entries.size(); ++i) {
            const vlog::RecoveredEntry& e = sr.entries[i];
            auto [it, fresh] = keys.try_emplace(e.key);
            if (fresh) order.push_back(e.key);
            it->second.entries.push_back(i);
            it->second.bytes += e.vptr.length;
            if (e.header.type == static_cast<uint8_t>(vlog::EntryType::Tombstone)) {
                const uint64_t seq = e.header.seq;
                if (!it->second.has_tombstone || seq > sr.entries[it->second.tombstone].header.seq)
                    it->second.tombstone = i;
                it->second.has_tombstone = true;
            }
        }
        if (fast) {
            // Verification scan: every entry dead by the index, and no tombstone to keep.
            bool live = false, keep_tombstone = false;
            for (const std::string& k : order) {
                std::lock_guard<std::mutex> stripe(stripeFor(k));
                const bool present = tree_->get(k).has_value();
                for (size_t i : keys[k].entries) {
                    const vlog::RecoveredEntry& e = sr.entries[i];
                    if (e.header.type == static_cast<uint8_t>(vlog::EntryType::Tombstone))
                        keep_tombstone = keep_tombstone || (!present && !oldest);
                    else if (present && entryIsLive(k, e.vptr))
                        live = true;
                }
            }
            {
                std::lock_guard<std::mutex> lk(gc_mu_);
                gc_totals_.verify_scans++;
                gc_totals_.verify_scan_bytes += rep->bytes_scanned;
            }
            if (!live && !keep_tombstone) {
                for (const std::string& k : order)
                    if (keys[k].has_tombstone) rep->tombstones_dropped++;
                {
                    std::lock_guard<std::mutex> lk(gc_mu_);
                    gc_totals_.tombstones_dropped += rep->tombstones_dropped;
                }
                retireSegment(victim);
                rep->action = GcReport::Action::FastPath;
                return;
            }
            if (live) {
                // live_bytes said 0 but the index references an entry here: a counter bug.
                rep->counter_mismatch = true;
                recountLive();
                std::lock_guard<std::mutex> lk(gc_mu_);
                gc_totals_.counter_mismatches++;
            }
        }
        // Relocation in batches of about gc_batch_bytes, bounded by gc_batch_max_stripes.
        std::vector<std::string> batch;
        uint64_t batch_bytes = 0;
        std::unordered_set<size_t> batch_stripes;
        for (const std::string& k : order) {
            batch.push_back(k);
            batch_bytes += keys[k].bytes;
            batch_stripes.insert(stripeIndex(k));
            if (batch_bytes >= opts_.gc_batch_bytes ||
                batch_stripes.size() >= opts_.gc_batch_max_stripes) {
                relocateBatch(victim, oldest, batch, keys, sr, rep);
                batch.clear();
                batch_bytes = 0;
                batch_stripes.clear();
            }
        }
        if (!batch.empty()) relocateBatch(victim, oldest, batch, keys, sr, rep);
        {
            std::lock_guard<std::mutex> lk(gc_mu_);
            gc_totals_.tombstones_dropped += rep->tombstones_dropped;
        }
        retireSegment(victim);
        rep->action = GcReport::Action::Relocated;
    }

    // Holds the batch's stripes (ascending) from the re-check through the vLog append and the
    // index applies, so no write to these keys can land between them (plan section 3).
    void relocateBatch(uint32_t victim, bool oldest, const std::vector<std::string>& batch,
                       std::unordered_map<std::string, VictimKey>& keys,
                       const vlog::ScanResult& sr, GcReport* rep) {
        std::vector<size_t> stripes;
        for (const std::string& k : batch) stripes.push_back(stripeIndex(k));
        std::sort(stripes.begin(), stripes.end());
        stripes.erase(std::unique(stripes.begin(), stripes.end()), stripes.end());
        if (opts_.on_gc_batch_stripes) opts_.on_gc_batch_stripes(victim, stripes.size());
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(stripes.size());
        for (size_t i : stripes) locks.emplace_back(stripes_[i]);

        struct Work {
            std::string key;
            std::vector<vlog::VPtr> old_chain; // empty for a tombstone
            bool consolidated = false;
        };
        std::vector<Work> work;
        std::vector<vlog::VLogWriter::RelocatedEntry> entries;
        for (const std::string& k : batch) {
            const VictimKey& vk = keys[k];
            std::optional<vlog::IndexValueRef> ref = tree_->get(k);
            if (!ref) {
                if (!vk.has_tombstone) continue;
                if (oldest) {
                    rep->tombstones_dropped++;
                    continue;
                }
                const vlog::RecoveredEntry& t = sr.entries[vk.tombstone];
                vlog::VLogWriter::RelocatedEntry r;
                r.type = vlog::EntryType::Tombstone;
                r.key = k;
                r.seq = t.header.seq;
                r.flags = static_cast<uint8_t>(t.header.flags | vlog::kFlagRelocated);
                r.raw_len = 0;
                entries.push_back(std::move(r));
                work.push_back(Work{k, {}, false});
                continue;
            }
            if (vk.has_tombstone) rep->tombstones_dropped++; // the key has a newer version
            const std::vector<vlog::VPtr> chain = currentChain(k);
            bool touches = false;
            for (const vlog::VPtr& p : chain) touches = touches || p.segment_id == victim;
            if (!touches) continue; // every version of k in the victim is dead
            vlog::VLogWriter::RelocatedEntry r;
            r.type = vlog::EntryType::Full;
            r.key = k;
            r.seq = ref->seq;
            const vlog::RecoveredEntry* e = nullptr; // the head, if it is in the victim
            for (size_t i : vk.entries)
                if (std::memcmp(&sr.entries[i].vptr, &ref->vptr, sizeof(vlog::VPtr)) == 0)
                    e = &sr.entries[i];
            if (e != nullptr && e->header.seq != ref->seq) {
                errno = 0;
                fatal("GC: the index head of \"" + k + "\" does not match the victim's entry");
            }
            if (chain.size() == 1 && e != nullptr &&
                e->header.type == static_cast<uint8_t>(vlog::EntryType::Full)) {
                // A Full head with no deltas: copy it byte for byte, keeping its seq.
                r.flags = static_cast<uint8_t>(e->header.flags | vlog::kFlagRelocated);
                r.raw_len = e->header.raw_len;
                r.payload = e->payload;
                work.push_back(Work{k, chain, false});
            } else {
                // A live chain touches the victim: one consolidated Full with the head's seq.
                Version v = readVersion(k, *ref);
                const std::string bytes = doc::serialize(materialize(v, k, *ref));
                r.flags = vlog::kFlagRelocated | vlog::kFlagConsolidated;
                r.raw_len = 0;
                r.payload.assign(bytes.begin(), bytes.end());
                work.push_back(Work{k, v.chain, true});
            }
            entries.push_back(std::move(r));
        }
        if (entries.empty()) return;
        std::vector<vlog::VPtr> at;
        try {
            at = writer_->appendRelocated(entries);
        } catch (const std::exception& e) {
            errno = 0;
            fatal(std::string("GC: vLog append failed: ") + e.what());
        }
        uint64_t written = 0;
        for (const vlog::VPtr& p : at) {
            noteAppended(p);
            written += p.length;
        }
        if (opts_.on_gc_after_append) opts_.on_gc_after_append(victim, entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            const vlog::VLogWriter::RelocatedEntry& r = entries[i];
            BPTree::ApplyResult ar;
            {
                std::lock_guard<std::mutex> lk(seq_mu_); // never during a checkpoint
                ar = tree_->apply(r.key, r.type, r.seq, r.flags, at[i]);
            }
            if (r.type == vlog::EntryType::Tombstone) {
                if (ar != BPTree::ApplyResult::Skipped) {
                    errno = 0;
                    fatal("GC: a relocated tombstone was applied");
                }
                {
                    std::lock_guard<std::mutex> lk(gc_mu_);
                    segs_[at[i].segment_id].kept_tombstone_bytes += at[i].length;
                }
                rep->tombstones_kept++;
                continue;
            }
            if (ar != BPTree::ApplyResult::Applied) {
                errno = 0; // the stripe was held since the re-check: the key cannot have moved
                fatal("GC: relocation of \"" + r.key + "\" (seq " + std::to_string(r.seq) +
                      ") was not applied");
            }
            accountReplace(r.key, work[i].old_chain, &at[i]);
            rep->keys_relocated++;
            if (work[i].consolidated) rep->chains_consolidated++;
        }
        if (opts_.on_gc_after_apply) opts_.on_gc_after_apply(victim);
        rep->bytes_written += written;
        rep->batches++;
        {
            std::lock_guard<std::mutex> lk(gc_mu_);
            gc_totals_.relocated_entries += entries.size();
            gc_totals_.relocated_bytes += written;
        }
        locks.clear(); // release the stripes before sleeping
        if (opts_.gc_rate_mb_s > 0) {
            // Token bucket of one batch: sleep as long as the batch's bytes take at the rate.
            const double secs = double(written) / (double(opts_.gc_rate_mb_s) * 1024 * 1024);
            std::unique_lock<std::mutex> lk(gc_thread_mu_);
            gc_cv_.wait_for(lk, std::chrono::duration<double>(secs), [&] { return gc_stop_; });
        }
    }

    // Background GC (plan section 6): every gc_interval_ms, passes until there is no victim and
    // nothing waits; a manual checkpoint after a pass that left segments waiting moves their
    // epochs on. Foreground operations wait at most on one stripe that a batch holds.
    void gcLoop() {
        std::unique_lock<std::mutex> lk(gc_thread_mu_);
        while (!gc_stop_) {
            gc_cv_.wait_for(lk, std::chrono::milliseconds(opts_.gc_interval_ms),
                            [&] { return gc_stop_; });
            if (gc_stop_) break;
            lk.unlock();
            for (int i = 0; i < 64; ++i) {
                GcReport r = runGcPass();
                const bool pending = gcPending();
                if (r.action == GcReport::Action::None && !pending) break;
                if (pending) checkpoint(CheckpointTrigger::Manual);
                std::lock_guard<std::mutex> g(gc_thread_mu_);
                if (gc_stop_) break;
            }
            lk.lock();
        }
    }

    // After a counter mismatch: recompute live_bytes and chains_ from the index (states kept).
    void recountLive() {
        CounterScan cs = scanCounters();
        std::lock_guard<std::mutex> lk(gc_mu_);
        for (auto& [id, st] : segs_) {
            auto it = cs.live.find(id);
            st.live_bytes = it == cs.live.end() ? 0 : it->second;
        }
        chains_ = std::move(cs.chains);
    }

    // Open (valid superblock only): unlinks segments the current slot lists as condemned, or below
    // its oldest_live_segment, that exist again. Never the replay or anchor segment or above.
    uint32_t removeCondemnedAtOpen(const SuperblockRecord& cur) {
        uint32_t removed = 0;
        for (uint32_t id : listVLogSegments(vlog_dir_)) {
            bool listed = false;
            for (uint32_t i = 0; i < cur.gc.count; ++i) listed = listed || cur.gc.ids[i] == id;
            if (!listed && id >= cur.sb.oldest_live_segment) continue;
            if (id >= cur.sb.replay_segment_id || id == cur.ext.vlog_first_segment) continue;
            const std::string path = segmentPath(id);
            if (::unlink(path.c_str()) != 0) fatal("unlink failed for " + path);
            removed++;
        }
        if (removed && vlog::sync_directory(vlog_dir_) != 0)
            fatal("fsync of directory " + vlog_dir_ + " failed");
        return removed;
    }

    // Lowest segment id present (segments leave segs_ only after their unlink is durable).
    uint32_t oldestSegment() const {
        std::lock_guard<std::mutex> lk(gc_mu_);
        return segs_.empty() ? 0 : segs_.begin()->first;
    }

    static size_t stripeIndex(std::string_view key) {
        return std::hash<std::string_view>{}(key) % kStripes;
    }
    std::mutex& stripeFor(std::string_view key) { return stripes_[stripeIndex(key)]; }

    uint64_t maxDocBytes(std::string_view key) const {
        const uint64_t vlog_max =
            vlog::kMaxEntryLen - sizeof(vlog::EntryHeader) -
            vlog::align_up(static_cast<uint32_t>(key.size()), vlog::kEntryAlign);
        const uint64_t opt =
            opts_.max_document_bytes ? opts_.max_document_bytes : uint64_t(doc::kMaxDocBytes);
        return std::min(vlog_max, opt);
    }

    // Reads the chain starting at ref. Each entry must pass its CRC, carry this key and the
    // expected seq (the index seq, then each Delta's base_seq), and the depth must fall by one per
    // hop until a Full entry. Throws VLogReadError otherwise.
    Version readVersion(std::string_view key, const vlog::IndexValueRef& ref) const {
        Version v;
        uint64_t want_seq = ref.seq;
        vlog::VPtr at = ref.vptr;
        int want_depth = -1; // depth of the next entry; -1 = whatever the first entry says
        while (true) {
            vlog::EntryHeader h;
            std::string k;
            std::vector<uint8_t> payload;
            const std::string where = "segment " + std::to_string(at.segment_id) + " offset " +
                                      std::to_string(at.offset) + " (expected seq " +
                                      std::to_string(want_seq) + ")";
            SegmentFdCache::Lease lease = fds_.acquire(at.segment_id);
            if (!lease || !vlog::VLogReader::readEntryAt(lease.fd(), at, h, k, payload)) {
                throw VLogReadError(
                    "vLog entry unreadable (missing segment, bad bounds, or CRC mismatch) at " +
                    where);
            }
            v.reads++;
            v.chain.push_back(at);
            const uint64_t seq = h.seq;
            if (seq != want_seq || k != key)
                throw VLogReadError("vLog entry does not match (seq " + std::to_string(seq) +
                                    ") at " + where);
            if (h.type == static_cast<uint8_t>(vlog::EntryType::Full)) {
                if (want_depth > 0)
                    throw VLogReadError("Delta chain reached a Full entry early at " + where);
                v.full_payload = std::move(payload);
                return v;
            }
            if (h.type != static_cast<uint8_t>(vlog::EntryType::Delta))
                throw VLogReadError("index or Delta points at a non-document entry at " + where);
            DecodedDelta d;
            try {
                d = decodeDelta(payload);
            } catch (const DeltaFormatError& e) {
                throw VLogReadError(std::string("malformed Delta entry at ") + where + ": " +
                                    e.what());
            }
            const int depth = d.prefix.chain_depth;
            if (depth < 1 || depth > vlog::kMaxDeltaChain ||
                (want_depth >= 0 && depth != want_depth) || d.prefix.base_seq >= seq)
                throw VLogReadError("Delta entry with a bad chain depth or base at " + where);
            if (want_depth < 0) v.depth = static_cast<uint16_t>(depth);
            want_seq = d.prefix.base_seq;
            at = d.prefix.base;
            want_depth = depth - 1;
            v.deltas.push_back(std::move(d));
        }
    }

    // Applies the chain's ops, oldest first, to its Full base. Any failure is corruption (ops were
    // checked against this base when they were written): VLogReadError.
    doc::Value materialize(const Version& v, std::string_view key,
                           const vlog::IndexValueRef& ref) const {
        try {
            doc::Value d = doc::parse(std::string_view(
                reinterpret_cast<const char*>(v.full_payload.data()), v.full_payload.size()));
            for (auto it = v.deltas.rbegin(); it != v.deltas.rend(); ++it)
                doc::applyOps(d, it->ops);
            return d;
        } catch (const std::exception& e) {
            throw VLogReadError("Delta chain of key \"" + std::string(key) + "\" (seq " +
                                std::to_string(ref.seq) +
                                ") does not apply to its base: " + e.what());
        }
    }

    // Unlinks segments that did not exist before open and hold only their 4 KiB header.
    void removeSegmentsCreatedByOpen(const std::vector<uint32_t>& before) {
        bool removed = false;
        for (uint32_t id : listVLogSegments(vlog_dir_)) {
            if (std::find(before.begin(), before.end(), id) != before.end()) continue;
            char name[32];
            std::snprintf(name, sizeof(name), "/vlog_%08u.log", id);
            const std::string path = vlog_dir_ + name;
            struct stat st;
            if (::stat(path.c_str(), &st) != 0) fatal("stat failed for " + path);
            if (static_cast<uint64_t>(st.st_size) != vlog::kBlockSize) continue;
            if (::unlink(path.c_str()) != 0) fatal("unlink failed for " + path);
            removed = true;
        }
        if (removed && vlog::sync_directory(vlog_dir_) != 0)
            fatal("fsync of directory " + vlog_dir_ + " failed");
    }

    static void checkKey(std::string_view key) {
        if (!validKeyLen(key.size())) {
            throw std::invalid_argument("key length " + std::to_string(key.size()) +
                                        " outside [1, 256]");
        }
    }

    vlog::AppendResult appendOrDie(vlog::EntryType type, std::string_view key, const void* data,
                                   uint32_t len, uint8_t flags = 0,
                                   const vlog::DeltaPrefix* prefix = nullptr) {
        if (closed_) fatal("IndexedStore used after close");
        vlog::AppendResult ar;
        try {
            ar = writer_->append(type, std::string(key), data, len, flags, 0, prefix);
        } catch (const std::exception& e) {
            errno = 0;
            fatal(std::string("vLog append failed: ") + e.what());
        }
        noteAppended(ar.vptr);
        return ar;
    }

    // ---- GC counters (gc_mu_ is a leaf lock: nothing else is taken while it is held) ----

    void noteAppended(const vlog::VPtr& p) {
        std::lock_guard<std::mutex> lk(gc_mu_);
        segs_[p.segment_id].total_bytes += p.length;
    }

    void killLocked(const std::vector<vlog::VPtr>& chain) {
        for (const vlog::VPtr& p : chain) {
            auto it = segs_.find(p.segment_id);
            if (it == segs_.end()) continue;
            it->second.live_bytes -= std::min<uint64_t>(it->second.live_bytes, p.length);
        }
    }

    // The key's current chain (head first), from the index and chains_. Caller holds the stripe.
    std::vector<vlog::VPtr> currentChain(std::string_view key) const {
        std::optional<vlog::IndexValueRef> ref = tree_->get(key);
        if (!ref) return {};
        std::lock_guard<std::mutex> lk(gc_mu_);
        auto it = chains_.find(std::string(key));
        if (it != chains_.end() && !it->second.empty() &&
            std::memcmp(&it->second.front(), &ref->vptr, sizeof(vlog::VPtr)) == 0)
            return it->second;
        return {ref->vptr};
    }

    // A Full entry (head = nullptr: a tombstone) replaced the key's chain old.
    void accountReplace(std::string_view key, const std::vector<vlog::VPtr>& old,
                        const vlog::VPtr* head) {
        std::lock_guard<std::mutex> lk(gc_mu_);
        killLocked(old);
        if (head) segs_[head->segment_id].live_bytes += head->length;
        chains_.erase(std::string(key));
    }

    // A Delta entry was added on top of the chain old: nothing dies.
    void accountDelta(std::string_view key, const std::vector<vlog::VPtr>& old,
                      const vlog::VPtr& head) {
        std::lock_guard<std::mutex> lk(gc_mu_);
        segs_[head.segment_id].live_bytes += head.length;
        std::vector<vlog::VPtr> c;
        c.reserve(old.size() + 1);
        c.push_back(head);
        c.insert(c.end(), old.begin(), old.end());
        chains_[std::string(key)] = std::move(c);
    }

    // Entry bytes in a segment file (its size minus the 4 KiB header); 0 if it is missing.
    uint64_t segmentEntryBytes(uint32_t id) const {
        struct stat st;
        const std::string path = segmentPath(id);
        if (::stat(path.c_str(), &st) != 0) {
            if (errno == ENOENT) return 0;
            fatal("stat failed for " + path);
        }
        const uint64_t size = static_cast<uint64_t>(st.st_size);
        return size > vlog::kBlockSize ? size - vlog::kBlockSize : 0;
    }

    std::string segmentPath(uint32_t id) const {
        char name[32];
        std::snprintf(name, sizeof(name), "/vlog_%08u.log", id);
        return vlog_dir_ + name;
    }

    // Small preads of entry heads (header + key + DeltaPrefix), one fd per segment.
    class HeadReader {
    public:
        explicit HeadReader(const IndexedStore& st) : st_(st) {}
        ~HeadReader() {
            for (auto& [id, fd] : fds_)
                if (fd >= 0 && ::close(fd) != 0) fatal("close failed for a vLog segment");
        }
        // False if the head cannot be read or does not hold this key and seq.
        bool read(const vlog::VPtr& p, std::string_view key, uint64_t seq, uint8_t* type,
                  vlog::DeltaPrefix* prefix) {
            auto it = fds_.find(p.segment_id);
            if (it == fds_.end()) {
                int f = ::open(st_.segmentPath(p.segment_id).c_str(), O_RDONLY);
                if (f < 0 && errno != ENOENT) fatal("open failed for a vLog segment");
                it = fds_.emplace(p.segment_id, f).first;
            }
            const int fd = it->second;
            if (fd < 0) return false;
            uint8_t buf[sizeof(vlog::EntryHeader) + kMaxKeyLen + 8 + sizeof(vlog::DeltaPrefix)];
            const size_t want = std::min<size_t>(sizeof(buf), p.length);
            const ssize_t r = ::pread(fd, buf, want, p.offset);
            if (r < 0) fatal("pread failed for a vLog segment");
            if (static_cast<size_t>(r) < sizeof(vlog::EntryHeader)) return false;
            vlog::EntryHeader h;
            std::memcpy(&h, buf, sizeof(h));
            const uint32_t poff = vlog::payload_offset(h.key_len);
            if (h.total_len != p.length || h.seq != seq || h.key_len != key.size() ||
                sizeof(vlog::EntryHeader) + h.key_len > static_cast<size_t>(r) ||
                std::memcmp(buf + sizeof(vlog::EntryHeader), key.data(), key.size()) != 0)
                return false;
            *type = h.type;
            if (h.type == static_cast<uint8_t>(vlog::EntryType::Delta)) {
                if (poff + sizeof(vlog::DeltaPrefix) > static_cast<size_t>(r)) return false;
                std::memcpy(prefix, buf + poff, sizeof(*prefix));
            }
            return true;
        }

    private:
        const IndexedStore& st_;
        std::map<uint32_t, int> fds_;
    };

    struct CounterScan {
        std::map<uint32_t, uint64_t> live;
        std::unordered_map<std::string, std::vector<vlog::VPtr>> chains;
        GcOpenStats stats;
    };

    // Walks every key of the index and its Delta chain (plan section 1, open-time rebuild).
    CounterScan scanCounters() const {
        const auto t0 = std::chrono::steady_clock::now();
        CounterScan cs;
        HeadReader hr(*this);
        std::string from;
        while (true) {
            BPTree::ScanResult batch = tree_->scanFrom(from, 4096);
            for (const auto& [key, ref] : batch) {
                cs.stats.keys_walked++;
                std::vector<vlog::VPtr> chain{ref.vptr};
                uint8_t type = 0;
                vlog::DeltaPrefix pre{};
                cs.stats.head_reads++;
                if (hr.read(ref.vptr, key, ref.seq, &type, &pre)) {
                    while (type == static_cast<uint8_t>(vlog::EntryType::Delta) &&
                           chain.size() <= vlog::kMaxDeltaChain) {
                        const vlog::VPtr base = pre.base;
                        cs.stats.chain_reads++;
                        if (!hr.read(base, key, pre.base_seq, &type, &pre)) break;
                        chain.push_back(base);
                    }
                }
                for (const vlog::VPtr& p : chain) cs.live[p.segment_id] += p.length;
                if (chain.size() > 1) cs.chains[key] = std::move(chain);
            }
            if (batch.size() < 4096) break;
            from = batch.back().first;
            from.push_back('\0'); // the smallest key after the last one
        }
        cs.stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                t0)
                          .count();
        return cs;
    }

    // Open: total_bytes from the segment sizes, live_bytes and chains_ from the index.
    void rebuildCounters() {
        CounterScan cs = scanCounters();
        std::lock_guard<std::mutex> lk(gc_mu_);
        segs_.clear();
        for (uint32_t id : listVLogSegments(vlog_dir_)) segs_[id].total_bytes = segmentEntryBytes(id);
        for (const auto& [id, live] : cs.live) segs_[id].live_bytes = live;
        chains_ = std::move(cs.chains);
        gc_open_ = cs.stats;
    }

    // delta_base_seq: the Delta's base (Delta entries only). A live Delta whose base is not the
    // key's current version is a broken invariant (the stripe lock rules it out): abort.
    void applyInOrder(std::string_view key, vlog::EntryType type, const vlog::AppendResult& ar,
                      bool* had, uint64_t delta_base_seq = 0) {
        std::unique_lock<std::mutex> lk(seq_mu_);
        if (ar.seq < next_apply_seq_)
            fatal("sequencer: seq " + std::to_string(ar.seq) + " already applied");
        seq_cv_.wait(lk, [&] { return next_apply_seq_ == ar.seq; });
        BPTree::ApplyResult r = tree_->apply(key, type, ar.seq, 0, ar.vptr, had, delta_base_seq);
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

    std::array<std::mutex, kStripes> stripes_;
    std::mutex seq_mu_;
    std::condition_variable seq_cv_;
    uint64_t next_apply_seq_ = 1;
    // Last entry applied to the index and the vLog position right after it (guarded by seq_mu_).
    uint64_t last_applied_seq_ = 0;
    uint32_t last_applied_segment_ = 0;
    uint32_t last_applied_end_ = vlog::kBlockSize;
    uint64_t last_cp_seq_ = 0; // checkpoint_seq of the last superblock (guarded by seq_mu_)
    VLogIdentity anchor_;      // identity the next superblock records (guarded by seq_mu_)

    mutable std::mutex gc_mu_; // leaf lock: guards the GC fields below
    std::map<uint32_t, SegStats> segs_;
    std::unordered_map<std::string, std::vector<vlog::VPtr>> chains_; // keys with a Delta head
    GcStats gc_totals_;                                               // scalar totals only
    GcOpenStats gc_open_;
    uint32_t gc_open_removed_ = 0;
    std::vector<uint32_t> condemned_; // written into the next superblock (<= kMaxCondemned)
    // replay_segment_id of the current and the older superblock slot (no older slot after a
    // rebuild until the next checkpoint).
    uint32_t cp_replay_cur_ = 0;
    uint32_t cp_replay_older_ = 0;
    bool cp_has_older_ = false;
    mutable ReaderRegistry readers_;
    mutable SegmentFdCache fds_{vlog_dir_, opts_.fd_cache_size};
    std::mutex gc_run_mu_; // one GC pass at a time
    std::mutex gc_thread_mu_; // guards gc_stop_
    std::condition_variable gc_cv_;
    bool gc_stop_ = false;
    std::thread gc_thread_;

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
