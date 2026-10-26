# Phase 5: vLog Garbage Collection & Compaction Execution Plan

This document outlines the architectural blueprint, design invariants, and testing strategy for the vLog Garbage Collection (GC) subsystem. The core goal is to reclaim disk space from dead vLog entries safely, concurrently, and with deterministic crash resilience, without modifying the existing layout format.

---

## 1. Liveness & In-Memory Accounting

### Liveness Invariants
An entry in a vLog segment is defined as **live** if and only if it satisfies one of the following structural criteria. Everything else is dead junk:
*   **Direct Index Pointing:** The active memory-resident or index-resident pointer maps directly to the entry's `VPtr`.
*   **Delta Chain Membership:** The entry is a structural node in a currently active delta chain. Liveness is tracked by tracing backwards from the active index head through the `base_vptr` links of successive deltas until a `Full` document entry is reached.

### In-Memory Space Counters
*   **Segment Tracking Structures:** Maintain an in-memory map of `SegmentId -> SegmentMetadata`. Each structure tracks `total_bytes`, `live_bytes`, and `dead_bytes`.
*   **Runtime Mutation Updates:**
    *   `Put` / `Update`: Subtract the overwritten entry’s size from its segment's `live_bytes` (adding it to `dead_bytes`), and initialize the new active segment's tracking counters with the size of the incoming record.
    *   `Erase`: Transition the overwritten entry's bytes from live to dead.
    *   `Consolidation`: Atomically adjust the counters for all target segments when a multi-segment delta chain is collapsed into a single `Full` document.
*   **Warm Boot Initialization:** At database open, the engine populates these counters from scratch by executing a single-pass traversal over all active index keys, walking their respective live delta chains, and aggregating byte sizes grouped by `SegmentId`.

---

## 2. Victim Selection Strategy

The GC core operates on a space vs. write amplification trade-off to ensure efficient disk reclamation:

*   **Zero-Live Reclamation:** Any segment whose in-memory counter indicates exactly `0` live bytes is earmarked for instant physical cleanup. It is deleted immediately without spawning any relocation writes.
*   **Threshold-Driven Selection:** For segments containing mixed data, the engine selects the candidate segment exhibiting the absolute lowest live-to-total byte ratio, constrained strictly by:
    $$\text{Live Ratio} = \frac{\text{live\_bytes}}{\text{total\_bytes}} < \text{gc\_threshold} \quad (\text{Default: } 0.5)$$
*   **Immutability Boundary:** The active (newest) segment currently receiving foreground client writes is explicitly excluded from victim selection, regardless of its liveness ratio.
*   **Metrics Tracking:** The subsystem continuously calculates and logs the system's global space amplification factor:
    $$\text{Space Amplification} = \frac{\text{Total vLog Disk Bytes}}{\sum \text{Segment live\_bytes}}$$

---

## 3. Relocation Mechanics & Race Resolution

When a mixed-live segment is selected as a victim, its live entries must be migrated forward to the active tail before the file can be decommissioned.

### Isolation and Relocation Execution
For each live key residing inside the victim segment, the GC worker thread executes the following sequence:
1.  Acquire the global key-space stripe lock matching the target key to isolate mutations.
2.  Re-verify the active index: Confirm that the index's current `VPtr` or active delta chain still flows through the victim's record. If a foreground write bypassed it while the GC thread was processing a prior key, drop the migration immediately.
3.  **Migration Rules:**
    *   *Standalone Full Head:* If the entry is a complete `Full` head document with zero dependent deltas, append a precise copy of the record to the active tail. Retain the **ORIGINAL sequence number (`seq`)** of the record and append the `kFlagRelocated` flag.
    *   *Intertwined Delta Chains:* If any part of a live delta chain intersects the victim segment (whether it touches an intermediate delta node or the root base record), **never move a base alone**. The GC collapses the entire chain into a single consolidated `Full` document. This record is written to the active tail with flags `(kFlagRelocated | kFlagConsolidated)` and tagged with the **latest chain head's `seq`**.

### Foreground Conflict Resolution (The Relocation Rule)
The relocated records are piped back into the storage engine through the primary sequencer pipeline. 
*   **The Invariant:** The sequencer applies a relocation write to the active index *if and only if* the current index `seq` matches the incoming relocated entry's `seq`. 
*   **Race Win Condition:** If a foreground user write wins a race and advances the key's sequence number while the GC thread is migrating old data, the sequence numbers will mismatch. The sequencer will reject the index update, rendering the relocated copy dead junk on the active tail and ensuring user writes always take precedence.

### Tombstone Lifecycle Protection
*   Tombstones encountered in a victim segment can be completely dropped *only* if the victim segment represents the absolute oldest segment file on disk.
*   If older segments exist below the victim, the tombstone must be copied forward and relocated. 
*   *Justification:* If a tombstone is dropped while older segments remain on disk, a subsequent full cold rebuild would expose legacy versions of the key in those older files, leading to silent data resurrection.

---

## 4. Safe Deletion & Structural Invariants

A victim segment file can be physically unlinked from the host operating system file system only when it satisfies a 4-tiered safety barrier:

Use code with caution.
[GC Migration Completed]
│
▼
[Invariant A: All Relocations Sequenced & Indexed]
│
▼
[Invariant B: Two Subsequent Superblock Checkpoints Committed]
│
▼
[Invariant C: Active Reader Reference / Epoch Count Drops to 0]
│
▼
[Invariant D: System Replay Point Advances Past Victim Boundaries]
│
▼
[Safe to Unlink File] ──► [fsync Directory] ──► [Update oldest_live_segment]

*   **Invariant A (Application):** Every live entry identified in the victim must have its relocation attempt processed, acknowledged, and committed to the index via the sequencer.
*   **Invariant B (Two-Checkpoint Delay):** Because the engine utilizes a double-buffered master `superblock.db` split into `Slot 0` and `Slot 1` that alternates updates, a segment cannot be deleted after a single checkpoint. It must survive until **two subsequent checkpoints** have successfully committed. This guarantees that neither superblock tree version references the victim segment, eliminating `File Not Found` errors if recovery rolls back to the alternate slot.
*   **Invariant C (Reader Guard):** Active foreground `Get` requests utilize thread-safe reference tracking (via segment-level atomic reference counts or hazard epochs). A segment cannot be deleted if a concurrent reader holds an active `VPtr` addressing its contents.
*   **Invariant D (WAL Boundary):** The system's global WAL recovery/replay point must have advanced completely past the maximum sequence numbers contained within the victim segment.

### Deletion Finalization
Once all four criteria are fulfilled, the engine executes a physical `unlink()` or `delete` on the file, forces a synchronous flush of the parent directory metadata via `fsync()` to ensure durability on macOS/POSIX systems, and atomically advances the `oldest_live_segment` identifier in the active superblock.

---

## 5. Recovery, Replay, and Full Rebuilds

### Crash Recovery Replay
During standard WAL replay from the last recorded checkpoint, the engine adheres to standard ingestion semantics augmented by the **GC Relocation Rule**: a relocated log entry (`kFlagRelocated`) updates the active index only if the log entry's `seq` matches the current sequence number stored in the index node.

### Full Cold Rebuilds
When both superblock slots are lost or invalidated, the database reconstructs its complete state by scanning raw vLog segments sequentially. To guarantee convergence without relying on arbitrary file discovery order, the engine enforces these exact resolution parameters:
*   **Highest Sequence Dominance:** For any given key, the entry carrying the absolute highest sequence number (`seq`) wins the state determination.
*   **Tie-Breaking Priority:** If two records for the same key share an identical `seq`, a `Full` entry bearing the `kFlagRelocated` or `kFlagConsolidated` flags takes precedence and wins over a raw `Delta` record.
*   **Tombstone Finality:** If the winning record is a tombstone, the key is handled as absent/deleted.
*   **Corruption Demarcation vs. Expected Orphans:** Because the GC collapses delta chains, a cold rebuild will encounter orphan deltas in newer segments whose historical base records were previously garbage collected and deleted. 
    *   *Expected Behavior:* If an intermediate delta's base `VPtr` points back to a deleted segment *but* a superior, consolidated `kFlagConsolidated` record exists with an equal or higher sequence number elsewhere in the log, the orphan delta must be silently ignored.
    *   *Real Corruption:* A base mismatch is flagged as true structural corruption *only* if a live delta chain points to a missing or unreadable segment offset and no superior consolidated record or higher sequence number exists to resolve the key's state.

---

## 6. Background Engine Subsystem

*   **Concurrency & Rate Control:** The GC subsystem runs on a dedicated background thread, governed by a configurable boolean flag (`gc_enabled`) and an I/O throttling option (`gc_write_rate_limit_mb_s`). The writer utilizes a token-bucket algorithm to smoothly pace disk writes, preventing I/O starvation of foreground transactions.
*   **Non-Blocking Operations:** Foreground `Put`, `Get`, and `Update` operations communicate concurrently using fine-grained key-space stripe locks. Client threads never block on background GC routines, pausing briefly only if they attempt to modify the specific individual key currently undergoing active migration.
*   **Index Compaction Interaction:** Index space optimization is explicitly scoped out of the core vLog garbage collector. The engine will not truncate trailing free pages of `index.db` inside this pass; space reclamation within the index layout is deferred entirely to independent B-Tree page smoothing phases.

---

## 7. Subsystem Measurement & Benchmarking Plan

To quantify operational performance and write amplification metrics, the system will execute a standardized benchmarking test harness:

*   **Workload Footprint:** Initialize the dataset with 10,000 distinct keys mapping to 1 KB values. Run 10 continuous rounds of randomized overwrites, high-frequency updates (producing long delta chains), and random key erases.
*   **Reporting Specifications:** The benchmark routine must output a structured payload containing:
    *   `vlog_size_before_bytes`: Total vLog size prior to triggering the GC run.
    *   `vlog_size_after_bytes`: Total vLog size post-GC cycle execution.
    *   `space_amplification_ratio`: Calculated space amplification metrics before and after cleanup.
    *   `gc_bytes_written`: Total bytes written to disk specifically by relocation and consolidation operations.
    *   `foreground_latency_p50_on_vs_off`: Comparison of median client latency with background GC enabled vs. disabled.
    *   `foreground_latency_p99_on_vs_off`: Comparison of tail client latency with background GC enabled vs. disabled.

---

## 8. Verification & Integration Test Suite (P5 Series)

All automated tests within this verification tier must use fixed operation counts when running under constrained ("quick") evaluation modes to ensure deterministic runtimes. To accommodate the extensive I/O scaffolding required to safely test background compaction without risking engine timeout failures, **the maximum memory budget for test files is raised to 24 MiB**. Heavy multi-pass segment cycles will be restricted to deep integration validation execution modes.

*   **P5.Liveness.Accounting:** Validate in-memory counter precision. Assert that overwriting keys, executing erases, appending delta chain elements, and completing structural chain consolidations shift `live_bytes` and `dead_bytes` counts across segments with byte-perfect accuracy.
*   **P5.Victim.Selection:** Verify that a segment registering zero live bytes is instantly reclaimed via file deletion without generating relocation writes. Ensure that mixed-live segments are skipped if their ratio exceeds `gc_threshold`, and validate that the active tail segment is never selected.
*   **P5.Race.ForegroundWins:** Simulate a race condition. The GC thread grabs a key from a victim segment and attempts relocation; simultaneously, a foreground client thread executes a high-sequence `Put` or `Erase` on the same key. Assert that the sequencer flags the sequence mismatch, drops the GC relocation write, and preserves the user's update.
*   **P5.Delta.CrossSegment:** Construct a live delta chain where individual delta nodes span across victim segments and non-victim segments. Execute a GC cycle, and assert that subsequent foreground `Get` requests parse the chain and return correct values.
*   **P5.Tombstone.Preservation:** Write a tombstone to a non-oldest segment file, trigger compaction on that segment, and assert that the tombstone is safely relocated forward. Perform a cold database rebuild from raw log files and confirm no legacy records are resurrected.
*   **P5.Crash.Relocation Phase:** Force an immediate process termination (`kill -9`) mid-way through the GC relocation write pipeline. Restart the database and verify that all acknowledged user data is completely intact.
*   **P5.Crash.PreCheckpoint:** Terminate the process after GC relocation writes have completed but before a new superblock checkpoint is flushed. Assert upon reboot that the database recovers safely to its pre-GC state without corruption.
*   **P5.Crash.InterCheckpoint:** Terminate the process after exactly one superblock slot has committed the GC changes, leaving the alternate slot pointing to the old layout. Verify that recovery operates smoothly and that the victim segment remains untouched.
*   **P5.Crash.PostUnlink:** Halt the engine immediately following the physical `unlink()` call but before the parent directory metadata is synced. Verify on reboot that the system handles the disk state cleanly.
*   **P5.Superblock.Corruption:** Manually corrupt the newest superblock slot following a successful GC pass. Assert that the engine falls back to the older slot, reads the tree state successfully, and that no files referenced by that older slot were prematurely deleted from disk.
*   **P5.Concurrent.ReadGuard:** Spin up a long-running foreground `Get` reader that pins a `VPtr` inside a victim segment. Trigger background GC deletion against that segment. Assert that the reference count/epoch guard blocks the physical file deletion until the reader thread safely releases its pointer.
*   **P5.Rebuild.Equivalence:** Execute multiple successive background GC cycles across diverse workloads. Wipe both superblock slots to force a full cold rebuild from raw vLog disk segments. Assert that the final reconstructed state exactly matches the active live index state.
*   **P5.Concurrency.ThreadSanitizer:** Execute a high-contention integration test running the background GC thread concurrently alongside 8 active foreground worker threads performing updates under ThreadSanitizer (TSAN) instrumentation to guarantee data-race freedom.
