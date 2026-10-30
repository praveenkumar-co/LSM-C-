#pragma once
// GcTypes.hpp - vLog garbage collection: per-segment counters and lifecycle states, GC reports,
// and the reader registry that keeps a segment on disk while a reader may still use it
// (docs/PHASE5_PLAN.md sections 1, 2, 4).

#include "index/IndexCommon.hpp"
#include "vlog/ScanProfile.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bpt {

// Segment lifecycle (plan section 4). Condemned: listed in a committed superblock's condemned
// list (or about to be, see SegStats::condemned_epoch); Unlinked segments leave the map.
enum class SegmentState : uint8_t { Active, Sealed, Migrating, WaitingForCheckpoints, Condemned };

inline const char* toString(SegmentState s) {
    switch (s) {
    case SegmentState::Active:
        return "Active";
    case SegmentState::Sealed:
        return "Sealed";
    case SegmentState::Migrating:
        return "Migrating";
    case SegmentState::WaitingForCheckpoints:
        return "WaitingForCheckpoints";
    case SegmentState::Condemned:
        return "Condemned";
    }
    return "?";
}

struct SegStats {
    uint64_t total_bytes = 0; // sum of total_len of every entry (the 4 KiB header excluded)
    uint64_t live_bytes = 0;  // sum of total_len of live entries (index heads and their chains)
    // Tombstones GC relocated into this segment because they were still needed. Not liveness
    // (erased keys are not in the index): victim selection counts them as occupied, so a
    // segment of kept tombstones is not picked again and rewritten in a loop. In memory only.
    uint64_t kept_tombstone_bytes = 0;
    SegmentState state = SegmentState::Sealed;
    uint64_t ready_at_epoch = 0;  // WaitingForCheckpoints: committed epoch needed (E + 2)
    uint64_t retire_epoch = 0;    // WaitingForCheckpoints: reader epoch at retirement
    uint64_t condemned_epoch = 0; // Condemned: epoch of the superblock that lists it (0: not yet)
};

// One segment in GcStats.
struct SegmentInfo {
    uint32_t id = 0;
    uint64_t total_bytes = 0;
    uint64_t live_bytes = 0;
    SegmentState state = SegmentState::Sealed;
    uint64_t kept_tombstone_bytes = 0;
};

struct GcStats {
    std::vector<SegmentInfo> segments; // ascending id
    uint64_t total_bytes = 0;          // sum of SegStats::total_bytes
    uint64_t live_bytes = 0;           // sum of SegStats::live_bytes
    uint64_t disk_bytes = 0;           // total_bytes + one 4 KiB header per segment
    double space_amplification = 0;    // disk_bytes / live_bytes (0 if nothing is live)
    uint64_t counter_mismatches = 0;   // verification scans that found a live entry in a 0-live segment
    uint64_t passes = 0;               // GC passes run
    uint64_t segments_unlinked = 0;
    uint64_t relocated_entries = 0; // Full copies, consolidated chains and kept tombstones written
    uint64_t relocated_bytes = 0;   // vLog bytes written by GC
    uint64_t tombstones_dropped = 0;
    uint64_t verify_scans = 0;      // fast-path verification scans
    uint64_t verify_scan_bytes = 0; // segment bytes those scans read
};

// What open did to rebuild the counters (plan section 1).
struct GcOpenStats {
    uint64_t keys_walked = 0;
    uint64_t head_reads = 0;  // small preads of an index head (header + key + DeltaPrefix)
    uint64_t chain_reads = 0; // small preads of Delta bases
    double ms = 0;
    uint32_t segments_removed = 0; // condemned segments found again and unlinked by open
};

// What one GC pass did.
struct GcReport {
    enum class Action { None, FastPath, Relocated } action = Action::None;
    uint32_t victim = 0;
    double victim_live_ratio = 0;
    bool counter_mismatch = false; // the verification scan found a live entry
    uint64_t entries_scanned = 0;
    uint64_t bytes_scanned = 0;
    uint64_t keys_relocated = 0;      // Full copies + consolidated chains
    uint64_t chains_consolidated = 0; // of keys_relocated
    uint64_t tombstones_kept = 0;
    uint64_t tombstones_dropped = 0;
    uint64_t bytes_written = 0;
    uint64_t batches = 0;
    uint32_t condemned = 0; // segments added to the condemned list by this pass
    uint32_t unlinked = 0;  // segments unlinked by this pass
    double ms = 0;
};

// Keeps segments on disk while a reader may still use them (plan section 4). Built from
// mutexes (no lock-free code): 64 shards, each a mutex and a count of readers per epoch, and one
// mutex for the epoch counter.
//   pin():    a reader records the current epoch BEFORE it reads the index.
//   retire(): GC, after the last relocation of a segment is applied, moves the epoch on and
//             keeps the new value; a reader that pins from then on sees only the new index.
//   pinnedBefore(e): some reader that pinned before retire() returned e is still reading, so it
//             may hold a VPtr into the retired segment; the segment must stay.
// pin() reads the epoch and records the pin while holding its shard lock, and pinnedBefore()
// takes every shard lock in turn. So a reader that read an epoch below e is either visible to
// pinnedBefore(e), or still holds its shard lock and pinnedBefore() waits for it. No thread holds
// more than two of these locks at once (shard, then epoch); retire() takes only the epoch lock.
// (It used to take all 64 shard locks, which exceeds ThreadSanitizer's limit of 64 locks held
// by one thread on Linux.)
class ReaderRegistry {
public:
    static constexpr size_t kShards = 64;
    struct Pin {
        size_t shard = 0;
        uint64_t epoch = 0;
    };

    Pin pin() {
        const size_t sh = std::hash<std::thread::id>{}(std::this_thread::get_id()) % kShards;
        SCAN_PROF_T0(prof_lock);
        std::lock_guard<std::mutex> lk(shards_[sh].mu);
        SCAN_PROF_ADD_NS(lock_registry_ns, prof_lock);
        uint64_t e;
        {
            SCAN_PROF_T0(prof_lock2);
            std::lock_guard<std::mutex> el(epoch_mu_);
            SCAN_PROF_ADD_NS(lock_registry_ns, prof_lock2);
            e = epoch_;
        }
        shards_[sh].pins[e]++; // recorded before the shard lock is released
        return Pin{sh, e};
    }

    void unpin(const Pin& p) {
        SCAN_PROF_T0(prof_lock);
        std::lock_guard<std::mutex> lk(shards_[p.shard].mu);
        SCAN_PROF_ADD_NS(lock_registry_ns, prof_lock);
        auto it = shards_[p.shard].pins.find(p.epoch);
        if (it == shards_[p.shard].pins.end()) {
            errno = 0;
            fatal("ReaderRegistry: unpin without a pin");
        }
        if (--it->second == 0) shards_[p.shard].pins.erase(it);
    }

    uint64_t retire() {
        std::lock_guard<std::mutex> el(epoch_mu_);
        return ++epoch_;
    }

    bool pinnedBefore(uint64_t e) const {
        for (const Shard& s : shards_) {
            std::lock_guard<std::mutex> lk(s.mu);
            if (!s.pins.empty() && s.pins.begin()->first < e) return true;
        }
        return false;
    }

    uint64_t activePins() const {
        uint64_t n = 0;
        for (const Shard& s : shards_) {
            std::lock_guard<std::mutex> lk(s.mu);
            for (const auto& kv : s.pins) n += kv.second;
        }
        return n;
    }

private:
    struct Shard {
        mutable std::mutex mu;
        std::map<uint64_t, uint32_t> pins; // epoch -> readers
    };
    std::array<Shard, kShards> shards_;
    std::mutex epoch_mu_; // guards epoch_; taken after a shard lock, never before
    uint64_t epoch_ = 1;
};

// RAII pin (IndexedStore::pin()). VPtrs read from the index while it is held stay readable.
class ReadGuard {
public:
    ReadGuard() = default;
    explicit ReadGuard(ReaderRegistry* r) : r_(r), p_(r->pin()) {}
    ReadGuard(ReadGuard&& o) noexcept : r_(o.r_), p_(o.p_) { o.r_ = nullptr; }
    ReadGuard& operator=(ReadGuard&& o) noexcept {
        if (this != &o) {
            release();
            r_ = o.r_;
            p_ = o.p_;
            o.r_ = nullptr;
        }
        return *this;
    }
    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;
    ~ReadGuard() { release(); }
    void release() {
        if (r_) r_->unpin(p_);
        r_ = nullptr;
    }

private:
    ReaderRegistry* r_ = nullptr;
    ReaderRegistry::Pin p_;
};

} // namespace bpt
