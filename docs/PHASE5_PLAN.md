# Phase 5 plan: vLog garbage collection

Plan only; no code yet. The vLog entry format (EntryHeader, DeltaPrefix, DeltaOpHeader, VPtr,
SegmentHeader, Superblock in VLogFormat.hpp) does not change. GC uses what the format already
has: `kFlagRelocated`, `kFlagConsolidated`, `Superblock::oldest_live_segment` and the relocation
branch of `should_apply()`.

## 0. Decisions needed before Step 1

These come from reading the current code. Each one changes what the spec asks for, or touches
a file earlier phases kept fixed.

**D1. VLogWriter needs one new method (include/vlog/VLogWriter.hpp, no format change).**
`VLogWriter::append()` always assigns the next seq (`current_seq_++`). It cannot write an entry
that keeps its original seq. It also takes one entry per call, so a single GC thread would get
one fsync per entry, not one per batch. The spec needs both, so the plan adds:

```cpp
// Appends entries that keep the caller's seq (each must carry kFlagRelocated). Does not change
// current_seq_. One write + one F_FULLFSYNC for the whole batch (joins the group commit).
// Returns one VPtr per entry. Same abort rules as append().
std::vector<VPtr> appendRelocated(const std::vector<RelocatedEntry>& batch);
```

The writer comment "file order == seq order" becomes "file order == seq order for entries
without kFlagRelocated". Nothing else in include/vlog changes.

*Alternative, if include/vlog must stay untouched:* GC rewrites each live version as an
ordinary put with a new seq while it holds the key's stripe lock. Replay and rebuild then need
no changes, because seqs keep rising in file order. The costs: `get(key, &seq)` reports a new
seq after GC with no user write, `kFlagRelocated` goes unused, and there is still one fsync per
key unless GC runs several appending threads. **The rest of this plan assumes D1 is approved.**

**D2. The vLog identity check must not depend on the lowest segment.** Today
`readVLogIdentity()` reads the header of the lowest-id segment, and the superblock stores that
segment's id and created_ns (`SuperblockExt`). Once GC unlinks segment 1, every open would fail
the identity check and run a full rebuild. Plan: store the identity of the **replay segment**
instead, using the same two ext fields under a new meaning. The replay segment always exists
(`planIndexOpen` already requires it), and GC never unlinks it (§4). Existing stores still open
unchanged: until GC runs, their recorded segment (1) still exists and matches. The next
checkpoint then records the new anchor.

**D3. A durable record of segments condemned for deletion (superblock.db only).** Victims are
chosen by live ratio, not by age, so deleted segments are not always a prefix of the id range.
`oldest_live_segment`, a low-water mark, cannot say "segment 4 is condemned but 3 is not." §4
adds a small condemned-id list to the superblock slot, in the unused bytes [96, 4096). This is
an index-side file. The ext magic becomes `SUPBEXT2`, and EXT1 slots are still read. This is how
open identifies a segment that reappears after a crash (spec §4, last bullet).

**D4. Two Phase 4 behaviours change. P4 tests stay green.**
- *Replay* still applies the Phase 4 delta rule. It must now accept relocated entries whose seq
  is below the entries around them (§5.1).
- *Full rebuild* can no longer apply entries in file order with the Phase 4 delta rule. With
  relocated entries in the log, file order is not seq order. Rebuild becomes the
  order-independent rule in §5.2. A bad Delta is still caught, by checking the winning chain hop
  by hop. P4.G6 variant 2 (wrong base_seq, rebuild path) still gets `VLogCorruptError` with
  "base seq" in the message.

**D5. One more victim rule the spec does not list:** a victim must contain only **applied**
entries (§2). Otherwise GC would see an appended-but-unapplied entry as dead and drop an acked
write.

### 0.1 Approved (with conditions)
D1–D4 and D5 are approved, with these conditions:
- **D1.** `appendRelocated(batch)` is additive and VLogFormat.hpp is unchanged. Relocated entries
  do not consume new seqs and are applied under the key's stripe lock with the relocation rule.
  Older tests that assert "seqs strictly increasing in file order" may exempt only entries with
  `kFlagRelocated`. Nothing else in older tests is weakened.
- **D2.** Stores written before Phase 5 open without a rebuild. Test P5.G5 rewrites the
  superblock ext the way Phase 4 wrote it, with the anchor at the lowest segment. A manual check
  also opens a store built by the Phase 4 binary.
- **D3.** The condemned list holds at most **32** segment ids (`kMaxCondemned = 32`). While it is
  full, GC condemns nothing more; it keeps relocating and unlinking until there is room.
  - The list is stored as an optional block at offset 96 of each superblock slot:
    `SuperblockGcExt`, with magic "SUPBGC01", a count, 32 ids and a CRC32C, 144 bytes in all.
  - A slot with an empty list keeps those bytes zero, so its image is byte-identical to a
    Phase 4 slot and `SUPBEXT1` stays as it is. This replaces the "EXT2 magic" idea above with
    the same effect and no version bump.
  - A non-zero block with a bad CRC or count > 32 makes the slot invalid (BadCrc/BadFields).
- **D4.** All Phase 1–4 tests keep passing unchanged, except for the D1 exemption.

**Two details settled while reading the code:**
- `total_bytes` at open is the file size minus the 4 KiB header. Entries are contiguous after
  tail truncation, so open does not scan the vLog for it. `SegStats` drops the `tombstones`
  field; tombstones are found when a victim is scanned.
- `put()` and `erase()` find the chain to kill without reading the vLog. A per-key in-memory map
  holds the chain VPtrs of keys whose head is a Delta, guarded by `gc_mu_`, built at open and
  kept up to date by every write. Keys whose head is a Full need only the index VPtr.
  - Open still does one small header read per key (32 bytes + key + prefix) to learn which heads
    are Deltas. This cost is reported in `openInfo()`.

**Found during Step 4: kept tombstones.** A segment that holds only tombstones GC kept would
count as 0 live, because tombstones are not counted as live. GC would then pick it again, copy
the tombstones into a new segment that is again 0 live, and repeat for as long as an older
segment needs them. G3's mixed workload hit this. The fix:
- GC records, per segment, the bytes of tombstones it relocated there (`kept_tombstone_bytes`).
- Victim selection treats those bytes as occupied: the fast path needs live == 0 and no kept
  tombstones, and the ratio is (live + kept) / total.
- Tombstone liveness is still not an index counter. This is GC's own record of what it chose to
  keep. A kept tombstone is copied again only when other data in its segment dies.
- The record is in memory only, so after a restart such a segment may be copied once more.
  Test: P5.D5.

### Changes from the PHASE5_PLAN.md committed in 9cbeb58
- A 0-live segment is no longer deleted on the counter alone. A verification scan must confirm
  it (§2), and needed tombstones block the fast path.
- `dead_bytes` is dropped (it is always total − live).
- "Replay point past the victim's max seq" becomes `replay_segment_id > victim id`.
- The checkpoint delay is pinned to the epoch rule in §4.
- Rebuild tie-break ranks are spelled out.
- Added: the race argument for holding the lock across the append, reader pinning before the
  index read, the condemned-id list, gap handling, and the steps, tests and budget.

---

## 1. Liveness and counters

**Live entry.** An entry is live if the index points at it, or if it is in its key's live
delta chain: walk `DeltaPrefix::base` from the index head down to the Full base. Everything else
is dead, except tombstones, which are decided at scan time (below).

**Counters.** Kept per segment in memory:
`SegStats { uint64_t total_bytes; uint64_t live_bytes; uint32_t tombstones; }`.
- They live in `std::map<uint32_t, SegStats>`, guarded by `gc_mu_`. This is a leaf lock: it is
  held only to change or read counters, and nothing else is taken under it.
- `total_bytes` is the sum of `total_len` of every entry in the segment. The 4 KiB segment
  header is excluded.
- `live_bytes` is the sum of `total_len` of live entries.

**Updates.** Each one is made after the index apply succeeds, still under the stripe lock, so
the chain being killed cannot change.

| Operation | Adds live | Kills |
|---|---|---|
| put (Full) | new entry | the key's whole previous chain (head plus every base, in every segment it spans) |
| update writing a Delta | new Delta | nothing: its base is still needed |
| update writing a consolidated Full | new entry | the whole previous chain |
| erase | nothing (tombstone bytes count in total only) | the whole previous chain |
| GC relocation (§3) | relocated entry | the chain it replaces |

All appends add to `total_bytes`, including no-op relocations whose apply was skipped.

**Getting the chain to kill.** For a Full head, the chain is just the index VPtr; no read is
needed. For a Delta head, `update()` already has the chain from `readVersion()`, which will also
record each hop's VPtr. `put()` and `erase()` over a Delta head read each hop's header and
prefix: depth reads, at most 4 small preads. That cost shows up in the bench put p50/p99.

**Rebuilt at open.**
- `total_bytes` comes from scanning headers, one sequential read per segment.
- `live_bytes` comes from walking every key in the index and its chain. A Full head needs no
  read; a Delta head of depth d needs d prefix reads.
- Open records `gc_counter_ms`, `keys_walked` and `chain_reads` in `openInfo()`, and the bench
  reports them.
- The header scan reads every segment once. On a large vLog that is the main open-time cost of
  GC. It is reported, not hidden.

**Tombstones are not counted as live.** Erased keys are not in the index, so nothing points at
a tombstone. When a victim is scanned:
- key present in the index: the tombstone is dead. The present version has a higher seq,
  because a re-put after an erase always gets a newer seq.
- key absent, and the victim is the **oldest segment present on disk**: drop the tombstone.
- key absent otherwise: **keep it.** Relocate it with `kFlagRelocated` and its original seq.

"Present on disk" includes segments waiting for deletion. A segment counts as gone only after
the directory fsync of its unlink returns (§4).

## 2. Victim selection

**Never a victim:**
- the active segment;
- any segment with id ≥ the segment of the last entry applied to the index (D5). Entries below
  that segment are all applied, because normal entries apply in seq order and seq order is file
  order;
- a segment already in Migrating or a later state.

**Fast path (unlink without copying).** Every condition must hold:
1. `live_bytes == 0`;
2. a scan of the victim confirms that every entry is dead according to the index;
3. it holds no tombstone that must be kept (§1).

The scan exists to catch counter bugs. If it finds a live entry:
- `gc_stats().counter_mismatches` goes up by one;
- the segment takes the normal relocation path;
- all counters are rebuilt from the index, as at open.

The scan's cost is reported per pass: segments scanned, bytes read, index lookups and ms. It is
one sequential read of the segment plus one index lookup per entry.

**Otherwise.** Pick the eligible segment with the lowest `live_bytes / total_bytes` below
`gc_threshold` (default 0.5). On a tie, take the lowest id, which is the oldest. With no such
segment, the pass does nothing.

**Space amplification** = total vLog bytes on disk / sum of live_bytes. It is reported by
`gc_stats()` and by the bench, before and after.

## 3. Relocation

GC scans the victim and groups its entries by key. Each key is handled once, even if several of
its chain entries sit in the victim.

**Per key.**
1. Take the key's stripe lock, and hold it through steps 2 to 5.
2. Pin the victim against unlink (§4), and read the index ref.
3. Re-check: is the version in the victim still the key's current version (`ref.seq` unchanged
   since the scan) or part of its live chain? If not, there is nothing to move.
4. Build the relocated entry.
   - Head is a Full with no deltas: copy the entry byte for byte (payload, LZ4 flag, raw_len),
     with `flags |= kFlagRelocated` and the **original seq**.
   - Any live chain that touches the victim (head Delta, middle Delta or Full base): write **one**
     Full entry holding the materialized document (`serialize(materialize(...))`), with
     `kFlagRelocated | kFlagConsolidated` and **the chain head's seq**. A base is never moved
     alone: other Deltas name it by VPtr, and moving it would break them.
   - Tombstone that must be kept (§1): copy it with `kFlagRelocated` and its original seq.
5. Append the entry (`appendRelocated`, D1), then apply it under the sequencer lock (`seq_mu_`)
   and the tree lock. The apply happens only if the index seq still equals the entry's seq
   (`should_apply` with `kFlagRelocated`). With the stripe lock held, a mismatch means a broken
   invariant, so the process aborts, the same rule as a live Delta in Phase 4. A relocated
   tombstone never applies, because the key is absent; it exists only for rebuild.
6. Update the counters (§1), then release the stripe.

**Relocated applies skip the seq wait.** They take `seq_mu_` and apply, but they do not wait for
`next_apply_seq_` and do not move it. Their seq was applied long ago.

**What readers see after GC.** `get()` returns the same bytes and the same seq. A copied Full is
byte-identical. A consolidated Full holds exactly the canonical JSON that `get()` used to build
from the chain.

**Why the lock must cover the append.** Suppose GC re-checked K at seq 5 and then released the
stripe before appending. An `erase(K)` can then append tombstone T (seq 10) in segment 8, and
GC's copy C (seq 5, relocated) lands after it, in segment 9. The relocated apply is skipped
because K is absent, so live state is fine. On disk, though, C now sits in a newer segment than
T. Later, segment 8 becomes the oldest and GC drops T as allowed. A full rebuild then sees only
C, with seq 5, and brings K back. Holding the stripe from the re-check through the append and
the apply means any erase of K appends after C. So T is always in a segment at or after C's,
and T can only be dropped once C's segment is gone.

**Batching.**
- GC collects up to `gc_batch_bytes` (default 1 MiB) of relocations.
- It locks their stripes in ascending stripe order (deduplicated), then re-checks every key.
- It sends one `appendRelocated` (one fsync), applies each entry, updates counters, and unlocks.
- No deadlock: foreground ops hold at most one stripe and never wait for a second one. A
  foreground thread waits in the sequencer only after its append, never while waiting for a
  stripe. GC holding stripes waits only for the writer's fsync and briefly for `seq_mu_`.
- Lock order: stripes (ascending) → reader registry → writer → sequencer → tree → `gc_mu_`.

**Rate limit.** A token bucket over GC's appended bytes (`gc_rate_mb_s`, default 32). GC sleeps
between batches with no stripe held.

## 4. Segment lifecycle and safe deletion

```
Active --roll over--> Sealed --picked--> Migrating --all relocations applied-->
WaitingForCheckpoints(ready_at_epoch = E + 2) --conditions--> Condemned --unlink+dir fsync--> Unlinked
```

The states are kept in memory, in the `SegStats` map.

**ready_at_epoch.**
- E is the committed superblock epoch, read under `seq_mu_` right after the victim's last
  relocation applied.
- `checkpoint()` holds `seq_mu_` and the tree's exclusive lock for its whole snapshot, flush and
  commit. So the next checkpoint (E+1) contains every relocation.
- After E+2 commits, slot E has been overwritten (ping-pong). Both slots then describe trees
  with no reference into the victim. After E+1 alone, slot E is still the fallback and may point
  into the victim.

**Condemn only when all hold:**
1. every relocation for the segment is applied;
2. committed epoch ≥ `ready_at_epoch`;
3. no reader or GC pin covers it (below);
4. `replay_segment_id` of **both** valid slots > the segment id. Condition 2 already implies
   this, because both slots were committed after the relocations, which sit in newer segments.
   It is still checked.

**Deleting.**
1. The next checkpoint writes the id into the slot's condemned list (D3, at most 32 ids). GC
   requests a checkpoint if none is due within `gc_interval_ms`.
2. After that checkpoint commits: unlink the segment, then fsync the vLog directory. Only now
   does the segment count as gone for the oldest-segment rule (§1).
3. The following checkpoint removes the id from the list and sets `oldest_live_segment` to the
   lowest segment id present.

**Reader safety.** Pins use epochs, built with mutexes, not lock-free code (standing rule).
- The `ReaderRegistry` has 64 shards. Each shard holds a mutex and a map of reader epoch to
  count.
- `get()` and `scan()` take a pin, holding the current global epoch, **before** reading the
  index. They release it after the last vLog read. `put`, `erase` and `update` pin too, because
  they read the old chain.
- When a segment enters WaitingForCheckpoints, GC records `retire_epoch = ++global_epoch`. The
  segment may be condemned only when no pin with epoch < `retire_epoch` remains.
- **Why a guard taken after the index read is not enough.** The reader reads ref → (seg 3,
  off X) and is descheduled. GC relocates the key, two checkpoints pass, no pin exists, and
  segment 3 is unlinked. The reader then pins, which is too late because nothing protects
  seg 3 any more, opens seg 3 and fails. Pinning first means any segment the reader could have
  seen in the index was retired after its pin, so it stays.
- The alternative, pin then re-read the index and retry if the ref changed, also works. It
  costs a second index lookup on every get, so it is not used.
- `scan()` returns VPtrs to the caller. Those stay readable only while the caller holds a pin,
  so the plan adds a public `IndexedStore::ReadGuard pin()`. This limit goes in the README.

**At open.** Each step runs only when a valid superblock exists, before the writer's tail scan.
1. Unlink every present segment whose id is in the current slot's condemned list, or below its
   `oldest_live_segment`. Then fsync the directory.
2. This is safe because an id enters the list only after every unlink condition held at that
   checkpoint. Deleting it at open does exactly what GC had already decided.
3. If the newest slot is corrupt and the older slot is used, that slot's list is older, so open
   deletes less, never more.
4. With no valid superblock (full rebuild), nothing is deleted. A reappeared segment is harmless
   to rebuild:
   - its live data has relocated copies with the same seq and a higher rank (§5.2);
   - its dead versions lose on seq;
   - its tombstones are still inside it;
   - no newer segment dropped a tombstone because of it, since its unlink was never fsynced.
   GC later removes it through the fast path.

## 5. Recovery and full rebuild

### 5.1 Replay from the replay point
- These rules are unchanged:
  - `should_apply()`;
  - the delta rule (skip if index_seq ≥ delta.seq; corrupt if the key is absent or
    index_seq ≠ base_seq);
  - the read-only `checkDeltaBases` pre-check.
- The relocation rule applies: an entry with `kFlagRelocated` applies only if the key is present
  and index_seq == entry seq. Replaying one twice is harmless, because it points the key at
  identical content.
- `replayVLogFrom` changes:
  - entries with `kFlagRelocated` are left out of the "seq increasing" and "first entry is
    checkpoint_seq + 1" checks;
  - `last_seq` becomes a max, not the last seen;
  - `last_segment` and `last_end` still track file position.
- A relocated entry appended but not applied when the checkpoint ran may sit before the replay
  point. It is then never applied, and the index keeps pointing at the original, which is safe:
  the victim was not condemned yet (its relocations were incomplete), and GC state is lost on a
  crash, so the victim is picked again later.

### 5.2 Full rebuild (order-independent)
**Pass 1, streaming.** Scan every present segment and apply each entry into the temp tree under
the rule below.
- Per key, the highest seq wins.
- On an equal seq, rank decides: consolidated Full (3) > relocated Full (2) > original Full
  (1) > Delta (0). A tie can only happen between a version and its relocated copy. On a tie,
  rebuild reads the header of the entry the tree currently points at to get its rank, one read
  per tie.
- A winning tombstone means the key is absent. A small side map, erased key → tombstone seq,
  remembers it, so an older entry met later in file order (for example a relocated copy) cannot
  re-insert the key. The map is O(erased keys) in memory and is freed after the rebuild.
- Deltas are accepted on seq and rank alone in this pass. Orphan deltas lose to an
  equal-or-higher Full and are ignored; their bases may be gone.

**Pass 2.** For every key whose winner is a Delta, walk its chain hop by hop (same checks as
`readVersion`: key, seq == base_seq, depth falls by one, ends at a Full). A failure is the only
real corruption: `VLogCorruptError` with the key, seq and "base seq" in the message. The temp
file is removed and nothing is modified, as in Phase 4.

**Result.** The same as replaying all writes in seq order. `rebuild_ms` and the side map's peak
size are reported.

### 5.3 Segment-id gaps
After GC the ids can look like 1, 2, 4, 7. Status of each part:
- `listVLogSegments`, `VLogWriter::recoverAndInitLocked` (max id + 1) and `scanSegment`'s
  "last segment" flag (position in the sorted list) already work from the sorted directory
  listing, not from id + 1.
- replay, rebuild and checkDeltaBases iterate that list.

Test P5.G1 covers all four paths. Any code that steps id → id + 1 must be found and fixed in
Step 1.

## 6. Background operation

**Options** (validated in `open`):

| Option | Default | Notes |
|---|---|---|
| `gc_enabled` | false | Existing tests are unaffected |
| `gc_threshold` | 0.5 | Range (0, 1) |
| `gc_rate_mb_s` | 32 | 0 means no limit |
| `gc_batch_bytes` | 1 MiB | |
| `gc_interval_ms` | 1000 | |

**One GC thread.** Each pass: pick a victim → relocate in batches → advance lifecycle states →
condemn/unlink what is ready. `runGcPass()` runs one pass on the caller's thread and returns a
`GcReport`; tests use it so GC runs deterministically. `close()` stops the thread before the
final checkpoint.

**Foreground cost.**
- Foreground ops never wait for a whole pass. At most they wait on one stripe lock (GC holds a
  batch's stripes for one fsync plus the applies), plus the usual sequencer and tree locks.
- `get()` adds one pin, which is a shard mutex.
- Test hooks: `on_gc_after_append`, `on_gc_after_apply`, `on_gc_before_unlink`,
  `on_gc_after_unlink_before_dirsync`.

**Out of scope.**
- Truncating or compacting index.db: freed index pages are reused, but the file does not
  shrink.
- More than one GC thread, and GC of index.db.
- Changing the vLog entry format.

## 7. Files

| File | Change |
|---|---|
| include/vlog/VLogWriter.hpp | `appendRelocated` (D1); comment on seq order. **Needs approval.** |
| include/index/IndexRecovery.hpp | relocation in replay checks, order-independent rebuild + chain pass, identity anchor (D2), open-time condemned cleanup |
| include/index/SuperblockManager.hpp | EXT2 with condemned list (D3), still reads EXT1 |
| include/index/Pager.hpp | CommitInfo carries oldest_live_segment + condemned list |
| include/index/IndexedStore.hpp | counters, stripe-held relocation, ReaderRegistry, lifecycle, GC thread, options, `runGcPass`, `gc_stats`, `pin` |
| include/index/GcTypes.hpp (new) | SegStats, SegmentState, GcReport, ReaderRegistry |
| tests/test_gc.cpp (new) | P5 tests |
| bench/bench_gc.cpp (new), Makefile | `make bench` runs bench_delta and bench_gc; LIMIT_quick 20 MiB → 24 MiB |
| README.md | GC section, measured numbers only |

## 8. Measurement (`make bench`, not part of `make test`)

**Planned bytes.** bench_gc prints its planned bytes first. Budget 256 MiB; files are deleted
after each run.

**Workload.**
- 10,000 keys × 1 KB documents.
- 1 MiB segments.
- 10 rounds. Each round runs 3,000 overwrites (put), 3,000 one-field updates, and 500 erases
  plus 500 re-puts of erased keys. A fixed seed picks the keys.

**Runs.**
- **GC off.** Run the rounds, then run `runGcPass()` until nothing is eligible.
  - Report vLog bytes and space amplification before and after.
  - Report GC bytes written and segments unlinked.
  - Report fast-path scan bytes and the counter rebuild ms at the next open.
- **GC on vs off.** The same rounds, with the background thread on and off.
  - Foreground put and get p50/p99, plus update p50/p99.
  - Final space amplification.

**Expected bytes.** Roughly 10.7 MB of initial load plus about 4.4 MB per round, about 55 MB per
run. With relocation and both runs, the total is about 130 MB.

The hardware line is printed as in bench_delta.

## 9. Tests (prefix P5., file tests/test_gc.cpp)

All tests use a temp dir, clean up and fixed op counts in quick mode. They use small segments
(8–64 KiB), so a few KB of data spans several segments. Every test compares the store against
an independent oracle (`replayWinners`): the §5.2 rule run on raw vLog entries, plus chain
checks. Heavy multi-pass tests run in full mode only and are marked [full].

**A. Counters**
- **A1 overwrite:** after `put(k)` twice, the old entry's bytes leave live_bytes of their
  segment.
- **A2 erase:** the whole chain dies, and tombstone bytes count in total only.
- **A3 delta append kills nothing:** the counters only gain the delta.
- **A4 consolidation across segments:** a chain spanning 3 segments dies in all 3 when the
  consolidated Full is written. The counters equal the open-time rebuild (close/reopen) after
  every step.

**B. Fast path**
- **B1:** a 0-live segment holding a needed tombstone, with older segments below it, is not
  deleted without copying. The tombstone is relocated first; a rebuild after the unlink does
  not resurrect the key.
- **B2:** a test hook sets a live segment's live_bytes to 0. The verification scan catches it,
  `counter_mismatches == 1`, the segment is relocated normally, and no data is lost.
- **B3:** a truly dead segment is unlinked with zero relocation bytes. The scan cost is reported.

**C. Selection**
- **C1:** threshold respected. Segments at ratio 0.6 are not picked; at 0.3 they are.
- **C2:** the active segment is never picked, even at 0 live, and neither is a segment holding
  unapplied entries.
- **C3:** with equal ratios, the lowest id is picked.

**D. Relocation, chains, tombstones**
- **D1:** a Full is relocated with its original seq and `kFlagRelocated`; get() returns the
  same bytes and seq.
- **D2:** a chain spanning victim and non-victim segments becomes one relocated|consolidated
  Full with the head seq. get() returns the same bytes before and after; update() continues
  from it.
- **D3:** a tombstone from a non-oldest victim is relocated. A full rebuild never resurrects the
  key, and checks every key against the oracle.
- **D4:** a tombstone in the oldest segment is dropped; a rebuild still matches.

**E. Races**
- **E1:** user put, update or erase while GC relocates the same key, staged with
  `on_gc_after_append` (the user thread blocks on the stripe). The user write is never lost.
  This runs each op type, both in quick mode.
- **E2 erase vs relocation:** across many staged interleavings, no relocated copy of K lands in a
  later file position than K's tombstone. After GC drops the tombstone (oldest segment), a full
  rebuild does not resurrect K.

**F. Readers**
- **F1:** a reader takes the index ref and pauses (test hook between the index read and the
  vLog read). GC relocates the key and 2 checkpoints pass. The segment is not unlinked while
  pinned. The reader gets the right document; the segment is unlinked after it unpins.
- **F2:** `scan()` + `pin()` keeps VPtrs readable across a GC pass.

**G. Gaps and rebuild**
- **G1:** files 1, 2, 4, 7 after GC. Reopen (replay), full rebuild (superblock.db deleted), the
  writer's next segment (8) and get() all work.
- **G2:** the newest superblock slot is corrupted after GC. Open falls back to the older slot,
  its tree reads correctly, and every segment it references exists.
- **G3 [full]:** after 20 GC passes over a mixed workload, a full rebuild equals the live state
  and the oracle.
- **G4:** D4's P4.G6 rebuild case still gives `VLogCorruptError` with "base seq", and files
  are unchanged.

**H. Crashes (fork + kill -9; quick mode runs one kill per point)**
- **H1:** mid-relocation (after `appendRelocated`, before the applies).
- **H2:** after relocation, before a checkpoint.
- **H3:** between the 2 checkpoints (after E+1, before E+2).
- **H4:** after unlink, before the dir fsync.
- **H5:** after the condemning checkpoint, before the unlink. Open removes the segment.

For each point:
- every acked write is intact;
- no erased key returns;
- every segment referenced by either superblock slot exists;
- open's cleanup removes only condemned ids;
- a full rebuild equals the oracle.

**I. Threads**
- **I1:** the GC thread plus 8 foreground threads (put, update, erase, get) on shared keys.
  The final state equals the oracle and the counters equal the open-time rebuild. It runs under
  tsan with fork-based tests skipped as before.

**Budget.**
- LIMIT_quick goes from 20 MiB to 24 MiB.
- test_gc's quick target is ≤ 4 MiB, putting the quick total around 21 MiB.
- Each [PASS] line shows its bytes.
- G3 and the multi-round I1 variant are full-mode only.

## 10. Steps

After each step: `make test-quick && make asan && make tsan`, show the output, commit. If a step
fails, stop and report.

1. **Writer, replay and rebuild.** `appendRelocated` (D1), relocation-aware replay checks,
   order-independent rebuild with the chain pass, identity anchor (D2), gap audit. Tests: G1,
   G4, plus rebuild cases built by writing relocated entries directly.
2. **Counters.** In-memory updates, open-time rebuild, `gc_stats()`. Tests: A1–A4.
3. **Readers and lifecycle.** ReaderRegistry and `pin()`, segment states, the condemned list in
   EXT2 (D3), open-time cleanup. Tests: F1 and F2 (using a manual relocation helper), G2.
4. **GC pass.** `runGcPass`: selection, fast path with scan, relocation with batching,
   tombstones. Tests: B1–B3, C1–C3, D1–D4, G3.
5. **Races and crashes.** Hooks plus tests E1, E2, H1–H5.
6. **Background thread.** Rate limit and options. Test: I1.
7. **Bench and README.** bench_gc, `make bench`, README with measured numbers only.

## 11. Known limits after Phase 5
- Open reads every segment header once to rebuild `total_bytes`. The cost grows with vLog size.
- Tombstones of erased keys move forward each time their segment is a victim, until they reach
  the oldest segment. A workload with many erases keeps some dead bytes for longer.
- A full rebuild keeps a side map of erased keys in memory.
- index.db does not shrink.
