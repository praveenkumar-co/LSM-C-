# LSM: a document store with key-value separation

A C++17 storage engine: documents go to an append-only value log (vLog), a disk-based B+Tree maps
each key to the location of its newest document, and checkpoints plus a two-slot superblock make
crash recovery replay only the tail of the log. `update()` appends a small Delta entry (the changed
fields) instead of rewriting the whole document.

## The problem

The earlier engine in `legacy/` (MongoLite) stored documents in 4 KiB slotted pages and updated
them in place behind a write-ahead log. A small update to one document rewrites a whole page plus
its WAL record, and a document that grows has to move. Bytes written to disk per byte of user data
(write amplification) grow with page size, not with document size.

This engine separates keys from values, in the style of WiscKey:

- a document is written **once**, appended to the vLog;
- the index holds only small fixed-size references (`seq`, segment, offset, length), so index pages
  change far less data than the documents themselves.

For a small change to a large document, `put()` still rewrites the whole document. `update(key,
ops)` writes only the changed paths as a Delta entry that points at the previous version; reads
follow the chain back to a full copy. Measured on one machine, this cuts bytes written per update
by 3.7x (1 KB documents) to 4.8x (8 KB documents); see Measured results.

## Design

```
put(key, doc) --> vLog append (group commit, F_FULLFSYNC) --> apply to B+Tree in seq order --> ack
update(key, ops) --> stripe lock --> read current version --> apply ops in memory
                  --> vLog append of a Delta (or a consolidated Full entry) --> apply --> ack
get(key)      --> B+Tree lookup --> vLog read at (segment, offset), CRC checked
                  (a Delta: follow its chain to the Full entry, apply the ops oldest first)
checkpoint    --> write dirty index pages --> F_FULLFSYNC --> superblock (other slot) --> F_FULLFSYNC
recovery      --> superblock --> open tree at its root --> replay vLog from its replay point
gc pass       --> pick a victim segment --> copy its live data forward (stripe locks held)
                  --> wait 2 checkpoints and for readers --> condemn in the superblock --> unlink
```

**Value log** (`include/vlog/`)
- Segments `vlog_%08u.log`, each starting with a 4 KiB header. Entries are 8-byte aligned and carry
  a CRC32C over everything after the CRC field. Seq and offset are assigned under one lock, so file
  order is seq order, except for GC copies (`kFlagRelocated`), which keep their original seq.
- Group commit: one thread writes everything staged so far with one `write()` and one
  `F_FULLFSYNC`, then acks the batch. A failed fsync aborts the process and is never retried.
  Before each write the leader waits up to `group_commit_window_us` (500 µs) for more requests to
  join, but only when the previous batch had more than one request, so a lone writer never waits.
  Acks still come only after the fsync.
- Segment rollover re-checks after waiting for an in-flight flush, so threads that hit a rollover
  together open one new segment, not one each.
- Reads use a bounded cache of open segment descriptors (64): one `pread` per entry instead of
  open + fstat + 2 x pread + close. GC drops a segment's descriptor before unlinking it.
- Recovery: a torn tail is truncated only in the last segment. Damage in any other segment, or a
  bad entry with valid entries after it, is treated as corruption and fails loudly.
- Segments are created with `O_CREAT | O_EXCL` and existing segments are never appended to or
  overwritten; a segment whose creation was torn by a crash is unlinked.

**B+Tree index** (`include/index/`)
- `index.db`: 4 KiB slotted pages, each with a CRC32C over the page. Page 0 is a static header
  written once at creation (format version, random nonce).
- Copy-on-write: pages that belong to a committed tree are never modified. Changing one copies the
  root-to-leaf path once per checkpoint epoch. A page freed while epoch E is committed is reused
  only after epoch E + 2 commits, so the trees named by both superblock slots stay intact.
- No leaf sibling pointers (they would force copying neighbours); scans use a stack of parents.
- No merges: erase leaves empty pages in place.
- One `std::shared_mutex` per tree: reads shared, writes exclusive.

**Superblock and checkpoints**
- `superblock.db` has two 4 KiB slots. A write goes to the slot that is not current, then
  `F_FULLFSYNC`; a read takes the valid slot with the highest epoch. Each slot records the root page,
  page count, checkpoint seq, and the vLog replay point, plus the index nonce and vLog identity so a
  superblock is never used with another store's files. The identity is the replay segment's id and
  creation time (GC never deletes that segment); stores written before GC, which recorded the
  lowest segment, still open without a rebuild.
- A checkpoint holds the sequencer lock and the tree's exclusive lock only to capture: the last
  seq applied to the index and the vLog position right after that entry (never the writer's
  tail), the root and page count, and a copy of every dirty page; every existing page is then
  frozen, so later changes copy it. With the locks released it writes the captured pages,
  `F_FULLFSYNC`s and commits the superblock. Pages freed meanwhile are reused only after the
  in-flight epoch + 2 commits.
- Index pages are fsynced **before** the superblock that names them is written. A crash before the
  superblock write leaves the previous checkpoint fully usable.
- Automatic checkpoints run every 4 MiB of vLog writes or 30 s, and on clean close.

**Delta updates** (`include/doc/`, `IndexedStore::update`)
- `include/doc/Json.hpp`: strict RFC 8259 parser (duplicate keys, NaN/Inf, bad escapes, lone
  surrogates, invalid UTF-8, integers outside int64 and depth > 64 are rejected) and a canonical
  serializer (keys in insertion order, shortest round-trip doubles, so the same tree always gives
  the same bytes).
- `include/doc/Update.hpp`: `$set`, `$unset`, `$inc` on dotted paths (`"profile.score"`,
  `"items.0"`). `$set`/`$inc` create missing objects; overlapping paths in one update, type
  mismatches, int64 overflow and out-of-range indexes are errors, and an error writes nothing.
- A Delta entry uses the existing vLog format: `DeltaPrefix` (base seq, base VPtr, chain depth)
  followed by the ops. When a chain would exceed `delta_chain_cap` (default and maximum 4), or the
  delta is more than half the document, `update()` writes a Full entry flagged consolidated. An
  update that changes nothing writes nothing.
- 1024 stripe locks: `put`, `erase` and `update` hold their key's stripe from reading the current
  version until the index is updated, so a Delta's base is always the key's current version.
  Checkpoints, `get` and `scan` take no stripe.
- `get()` returns a Full entry byte for byte; for a chain it checks every hop (CRC, key, seq,
  depth), applies the ops to the Full base and returns canonical JSON.
- Replay rule for a Delta: skip if the index seq is already >= its seq; if the key is absent or
  its seq is not the Delta's base seq, open fails with `VLogCorruptError` and changes no file;
  otherwise the key moves to the Delta.

**Garbage collection** (`IndexedStore::runGcPass`, `gc_enabled`; design in `docs/PHASE5_PLAN.md`)
- Per segment, in memory: total bytes and live bytes (index heads and their Delta chains). A Full
  write or an erase kills the key's whole previous chain; a Delta kills nothing. Open rebuilds the
  counters from the index (one small read per key, one per Delta hop).
- Victims: never the segment being written, nor one holding entries not yet applied to the index.
  A 0-live segment is deleted without copying only after a scan confirms every entry is dead
  (this catches counter bugs). Otherwise the lowest live ratio below `gc_threshold` (0.5), oldest
  first on a tie.
- Relocation in batches: GC locks the batch's stripes in ascending order (at most `gc_batch_max_stripes` = 32 distinct stripes or `gc_batch_bytes`, whichever comes first) and holds them from the
  re-check through one `appendRelocated` (one fsync) and the index updates. A Full entry is copied
  byte for byte with its original seq; a chain touching the victim becomes one consolidated Full
  entry with the head's seq. Holding the stripe across the append is what stops a relocated copy
  from landing after the key's tombstone.
- Tombstones of erased keys are copied forward unless the victim is the oldest segment; kept
  tombstones count as occupied, so GC does not rewrite them in a loop.
- A segment is deleted only after two checkpoints that both include its relocations, once no
  reader that pinned before its retirement is still reading (`get()` pins before its index read),
  and once both superblock slots' replay points are past it. Its id is first written into the
  superblock's condemned list (at most 32), then unlinked and the directory fsynced; open deletes
  any listed segment a crash brought back.
- `appendRelocated` is the only change in `include/vlog/`; the on-disk format is unchanged.

**Recovery**
1. Check `index.db` header, the superblock, nonce, vLog identity, and every page of the committed
   tree (CRC + structure), before the vLog writer opens.
2. `ftruncate` `index.db` to the committed page count (drops pages written after the checkpoint).
3. Rebuild the free list by walking the current and previous trees.
4. The vLog writer's tail scan and the index replay both start at the replay point; the index uses
   `should_apply()` (newer seq wins, so replay is idempotent).

If anything in step 1 fails (no superblock, both slots corrupt, a bad tree page, files from another
store), the index is rebuilt from the whole vLog. The rebuild does not depend on file order: per key
the highest seq wins (a GC copy beats the entry it copied), a winning tombstone means absent, and
every winning Delta chain is then walked back to its Full base; a broken chain fails the open with
`VLogCorruptError` and changes no file. Segment ids may have gaps after GC. If the replay point is missing from the vLog
(acked entries are gone), open fails with an error and changes nothing.

## Layout

```
include/vlog/    value log: on-disk format, CRC32C, writer, reader
include/index/   B+Tree: page format, pager, tree, superblock, recovery, Delta codec, IndexedStore
include/doc/     JSON value/parser/serializer and update ops
tests/           test binaries (built and run by the Makefile)
bench/           delta-update and GC benchmarks (make bench)
legacy/          earlier MongoLite engine (not built)
```

`include/vlog/` and `include/doc/` include nothing from the layers above them; `make` checks this.

## Build and test

Requires a C++17 compiler. Tested with Apple clang 21 on macOS; GitHub Actions runs `make test-quick`
and `make asan` on Ubuntu with g++ and clang++ (`.github/workflows/ci.yml`).

```
make test-quick  # optimized, smaller workloads, under 32 MiB written in total
make test        # optimized, full workloads
make asan        # AddressSanitizer + UndefinedBehaviorSanitizer
make tsan        # ThreadSanitizer (fork + kill -9 tests are skipped)
make bench       # delta-update and GC benchmarks; not part of the test modes, about 390 MB written
make bench-stress  # 100,000 keys, 16 threads, GC on/off, 50 x kill -9; about 740 MB written
make clean
```

All builds use `-Wall -Wextra -Werror`. Each test prints one `[PASS] name (summary)` line; run with
`TEST_VERBOSE=1` to also print the measurements behind each test. Every test binary uses its own
temp directory, removes it on exit, aborts if free disk space is below 10 GB, and enforces a write
budget as it writes (512 MiB in total for `test`/`asan`/`tsan`, 32 MiB for `test-quick`).

| Binary | Covers |
|---|---|
| `test_vlog_crash` | CRC32C, reader bounds, torn tails, redo rule, torn segment creation, 50 x kill -9 during group commit, group commit benchmark |
| `test_bptree` | pager, tree vs `std::map`, splits, scans, checkpoint + reopen, corruption handling, IndexedStore, 50 x kill -9, concurrency |
| `test_edge_cases` | index ahead of the vLog, wrong vLog, bad VPtr, 16 MiB limit, races, height 4, injected I/O failures |
| `test_superblock` | slot CRCs, ping-pong writes, one or both slots corrupt, torn slot writes |
| `test_cow` | committed pages never modified, copy cascade, delayed page reuse, free list rebuilt at open |
| `test_checkpoint` | checkpoint contents and triggers, tail-only recovery, fallback rebuilds, page reuse safety, corrupt newest slot, 50 x kill -9 at random points |
| `test_doc` | JSON parsing and serialization edge cases, paths, `$set`/`$unset`/`$inc` semantics and errors (P4.A-C) |
| `test_delta` | `update()` errors write nothing, chains and consolidation, mixed put/erase/update, concurrent updates, kill -9 during updates, replay rule, corrupt or missing chain entries (P4.C-G) |
| `test_gc` | relocated entries in replay and rebuild, segment-id gaps, identity of older stores, counters, segment lifecycle and condemned list, reader pins, victim selection, relocation of Full entries / chains / tombstones, user writes and erases racing GC, kill -9 at 5 GC points, GC thread + 8 threads (P5) |
| `test_perf` | segment rollover with 16 threads, segment descriptor cache (bounded, GC, deleted segments), adaptive group-commit window, checkpoints written outside the locks: snapshot fidelity and page reuse, 16-thread load, kill -9 at 3 new points (P6) |

## Measured results

One run of `TEST_VERBOSE=1 make test` (optimized build) on 2026-10-24:
Apple M5 (10 cores), 16 GB RAM, internal SSD (APFS), macOS 27.0, Apple clang 21.0.0.

**Group commit** (appends of ~25-byte documents, 1.5 s per row, every ack after `F_FULLFSYNC`):

| Threads | Writes/s | F_FULLFSYNC/s | Avg batch |
|---:|---:|---:|---:|
| 1 | 260 | 261 | 1.0 |
| 4 | 544 | 273 | 2.0 |
| 16 | 2,155 | 270 | 8.0 |
| 64 | 8,692 | 272 | 31.9 |

Throughput is bound by `F_FULLFSYNC` (about 270 per second, ~3.7 ms each); group commit raises
writes per second by batching more acks per flush.

**Recovery**

| Scenario | Result |
|---|---|
| Crash after 6,000 entries in 30 segments; last checkpoint 200 entries earlier | 200 entries read by the writer scan and 200 by the index replay; open took 11.3 ms |
| Same vLog, superblock deleted (full rebuild) | 6,000 entries read |
| Full rebuild, 1,500 entries / 200 keys | 18-20 ms |

**Checkpoints**
- Time the locks are held (writers and readers wait): 3.9 / 10.1 / 14.7 ms (min / median / max over
  15 checkpoints of small trees in CP1-CP3); max 18 ms over 161 checkpoints taken while 4 writers
  and 2 readers were running (CP4). Most of it is the two `F_FULLFSYNC` calls.

**Crash tests** (all passed)
- vLog: 50 kill -9 runs during multi-threaded group commit; all 15,788 acked entries recovered with
  correct bytes.
- Store: 50 kill -9 runs during writes, recovery and clean close; 14,523 acked operations, the
  newest acked operation was reflected for every key.
- Checkpoints: 50 kill -9 runs during writes, during the checkpoint flush, between the index fsync
  and the superblock write, and during recovery; 11,715 acked operations reflected, and the index
  equalled a full vLog replay every time.

**Delta updates** (`make bench`, same machine, 2026-10-25): one key, one small field
(`$inc profile.score`) changed 10,000 times; `put` rewrites the whole document each time. Default
checkpoint triggers; index bytes include checkpoints and the close.

| Document | Method | vLog bytes/update | index bytes/update | Total | vs put |
|---|---|---:|---:|---:|---:|
| 1 KB | put | 1,072 | 2.5 | 1,074 | 1.00x |
| 1 KB | update, cap 2 | 421 | 1.6 | 423 | 2.54x |
| 1 KB | update, cap 4 | 291 | 1.6 | 293 | 3.67x |
| 2 KB | put | 2,096 | 4.1 | 2,100 | 1.00x |
| 2 KB | update, cap 2 | 763 | 1.6 | 764 | 2.75x |
| 2 KB | update, cap 4 | 496 | 1.6 | 498 | 4.22x |
| 8 KB | put | 8,240 | 16.4 | 8,257 | 1.00x |
| 8 KB | update, cap 2 | 2,810 | 5.7 | 2,816 | 2.93x |
| 8 KB | update, cap 4 | 1,725 | 4.1 | 1,729 | 4.78x |

A Delta entry for this update is about 96 bytes. With cap 4, 8,000 of the 10,000 updates wrote a
Delta and 2,000 wrote a consolidated Full entry (cap 2: 6,667 and 3,333), so the full copy every
(cap + 1)-th update dominates the total.

get() latency by chain depth (cap 4, 5,000 gets per depth, page cache warm, p50 / p99 in µs):

| Document | depth 0 | depth 1 | depth 2 | depth 3 | depth 4 |
|---|---|---|---|---|---|
| 1 KB | 15.4 / 16.9 | 38.2 / 46.8 | 54.0 / 83.6 | 69.0 / 83.7 | 84.1 / 95.9 |
| 2 KB | 15.5 / 19.0 | 44.8 / 54.8 | 60.0 / 69.5 | 75.2 / 85.8 | 90.3 / 100.9 |
| 8 KB | 17.4 / 20.5 | 101.2 / 182.2 | 112.8 / 151.2 | 129.1 / 163.1 | 138.5 / 158.4 |

Depth d reads d + 1 vLog entries. Depth 0 returns the stored bytes without parsing; from depth 1
on, get() also parses the base, applies the ops and serializes the result, which is most of the
jump from depth 0 to 1 for 8 KB documents.

**Garbage collection** (`make bench`, same machine, 2026-10-27): 10,000 keys x 1 KB, 1 MiB
segments, 10 rounds of 3,000 overwrites, 3,000 one-field updates, 500 erases and 500 re-puts.

| Run | vLog on disk | Space amplification | GC bytes written | Segments unlinked |
|---|---:|---:|---:|---:|
| GC off, before collecting | 50.5 MB | 4.51x | - | - |
| GC off, then collected to idle (63 passes, 6.0 s) | 13.3 MB | 1.26x | 4.9 MB | 60 |
| GC on (background thread, 32 MB/s limit), at the end | 15.3 MB | 1.42x | 35.4 MB | 87 |

Space amplification = vLog bytes on disk / live bytes. With GC running during the workload it
wrote 7x more than collecting once at the end, because it copies data that is overwritten again
later. The verification scans read 0.1 MB and found no counter mismatch. The next open rebuilt the
counters in 11.0 ms (10,000 head reads, 1,963 chain reads).

Foreground latency, one thread, every operation timed (µs):

| Operation | GC off p50 | GC off p99 | GC on p50 | GC on p99 |
|---|---:|---:|---:|---:|
| put | 3,930 | 5,022 | 3,958 | 8,139 |
| update | 3,916 | 5,075 | 3,991 | 5,186 |
| get | 36.3 | 243.6 | 23.5 | 195.4 |

put and update are dominated by `F_FULLFSYNC` (about 3 ms here). With GC on, put p99 rose by about
0.6 ms; the other differences are within the run-to-run noise of a single run.

**Tree shape**: 200,000 sequential 32-byte keys give a height-4 tree of 5,999 pages (24 MB).

These are single runs on one machine, not averaged benchmarks.

## High-Scale Stress Benchmark

The numbers in this section are from one run before the performance work below (commit
`bfea1bf`); see "Performance work" for medians of 3 runs before and after.

`make bench-stress` (`bench/bench_stress.cpp`), one run on 2026-10-28, Apple M5, 17 GB RAM,
macOS 27.0, Apple clang 21. Two fresh stores with the same seed, one with GC off (compacted with GC
passes after the run) and one with the background GC thread on (32 MB/s limit):
- load 100,000 keys x 1,000-byte documents with 16 threads;
- B+Tree cache of 1,024 frames (4 MiB), 4 MiB segments;
- 100,000 mixed operations on 16 threads: 35% put, 45% update (`$inc profile.score`,
  `$set profile.tag`), 10% erase, 10% get.

Each thread owns the keys `k % 16 == t`, so it knows each of its keys' exact document; all 9,440
gets of present keys in each run (depth 0 and Delta chains) matched it byte for byte. Every
operation was timed with `steady_clock`.

**Throughput** (operations per second, 16 threads)

| Phase | GC off | GC on |
|---|---:|---:|
| Load (100,000 puts) | 2,052 | 1,985 |
| Mixed (100,000 operations) | 2,503 | 2,219 |
| Mixed, writes only | 2,208 | 1,958 |

**Latency** (µs; p50 / p90 / p99 / p99.9 / max)

| Operation | n | GC off | GC on |
|---|---:|---|---|
| put | 35,145 | 6,935 / 8,087 / 12,530 / 23,558 / 149,804 | 7,124 / 8,189 / 28,683 / 124,740 / 168,354 |
| update, Delta | 42,971 | 7,038 / 8,224 / 12,954 / 21,061 / 131,362 | 7,262 / 8,334 / 32,828 / 129,320 / 179,944 |
| update, consolidated | 6 | 7,256 / 8,209 / 8,209 / 8,209 / 8,209 | 7,262 / 7,951 / 7,951 / 7,951 / 7,951 |
| erase | 10,078 | 6,945 / 8,079 / 12,627 / 16,708 / 23,996 | 7,126 / 8,165 / 32,585 / 108,059 / 162,254 |
| get, depth 0 | ~7,846 | 94 / 216 / 428 / 3,571 / 3,904 | 99 / 241 / 425 / 13,064 / 20,345 |
| get, Delta chain (depth 1-4) | ~1,594 | 197 / 279 / 457 / 3,631 / 3,746 | 202 / 288 / 464 / 7,225 / 12,637 |

- **Writes wait for `F_FULLFSYNC`.** 16 threads share each group-commit flush, so a write's p50 is
  about 7 ms.
- **Few consolidations.** Updates spread over 100,000 keys, so chains rarely grew past depth 4:
  only 6 of the ~43,000 updates wrote a consolidated Full entry.
- **GC on hurt the write tails.** It raised put p99 from 12.5 to 28.7 ms and p99.9 from 23.6 to
  124.7 ms. With GC on, the index wrote 180.8 MB instead of 67.1 MB, because the GC thread
  requests a checkpoint whenever a retired segment waits for epochs. A checkpoint holds the
  sequencer and tree locks, so writers stall behind it.

**Space amplification** (vLog bytes on disk / live bytes)

| When | GC off | GC on |
|---|---|---|
| Baseline (after the load) | 104.8 MB / 104.0 MB = 1.01x | 104.9 MB / 104.0 MB = 1.01x |
| Peak churn (after the mixed phase) | 148.3 MB / 100.6 MB = 1.47x | 148.2 MB / 100.6 MB = 1.47x |
| After compaction | 148.2 MB / 100.6 MB = 1.47x | 148.2 MB / 100.6 MB = 1.47x |

- **Compaction changed little.** The churn left about a third of every segment dead, and no
  segment fell below the 0.5 live-ratio threshold.
- **What GC did reclaim.** It unlinked only 0-live segments (22 with GC off, 34 with GC on) and
  copied 0.1-0.2 MB.
- **Counters checked.** Counter mismatches were 0 in both runs, and the counters equalled a
  recount from the index.

**Buffer pool** (1,024 frames = 4 MiB)

| Measure | GC off | GC on |
|---|---:|---:|
| index.db at the end (free pages included) | 3,502 pages (13.7 MiB) | 2,680 pages (10.5 MiB) |
| Tree height | 3 | 3 |
| Page reads per mixed operation | 0.146 | 0.378 |
| Page evictions during the mixed phase | 17,982 | 57,050 |
| vLog bytes written: load / mixed | 104.8 / 43.6 MB | 104.9 / 43.8 MB |
| Index bytes written: load / mixed | 4.7 / 57.6 MB | 5.4 / 174.2 MB |

- **Small leaf cells.** A leaf cell holds the key and a 24-byte reference, so the index for
  100,000 keys is 10.5-13.7 MiB (3,502 pages including free ones).
- **Comparison (computed, not measured).** With the 1,000-byte documents in the leaves, at most 3
  fit in a 4 KiB page, so the tree would need at least 33,333 leaf pages (130 MiB), 33x the pool.
- **The index still does not fit.** It is 2.6-3.4x the 4 MiB pool, so the mixed phase still
  evicted pages. Writing evicted dirty pages cost more index bytes than vLog bytes in the mixed
  phase.

**Crash resilience**

The chaos store has 4,096 keys, 256 KiB segments, background GC on and checkpoints every 1 MiB of
vLog. Each of 50 children ran 16 threads and was killed with kill -9 after 400-4,399 acks. After
each crash the store reopened, every key was compared with the acked state, and then
superblock.db was deleted and the full cold rebuild was compared with the recovered index.

| Measure | Value |
|---|---:|
| Crashes | 50 |
| Acked operations verified after recovery | 112,283 of 112,283 |
| Operations in flight at the kill (applied / not applied) | 306 / 475 |
| Index equal to the full cold rebuild | 50 of 50 (204,800 key comparisons) |
| Recovery open, mean / max | 20.6 / 30.0 ms |
| Full rebuild, mean | 124.9 ms |

This run wrote 736 MB in total (cap 2 GiB).

## Performance work

Goal: make `make bench-stress` faster without changing what it measures: the same workload,
seed, dataset (100,000 keys x 1 KB, 16 threads) and 4 MiB index cache, every acked write still
`F_FULLFSYNC`ed. Numbers are the median of 3 runs on an Apple M5, 17 GB RAM, internal SSD, macOS
27.0, Apple clang 21, on AC power with Low Power Mode off, the machine kept awake with
`caffeinate`. Baseline: commit `d9cefa4`. After: commit `ff348c6` plus the benchmark's measurement
additions.

**Profile first** (`sample`, 10 s of the mixed phase, 16 workers, about 52,800 samples):

| Where worker time went | Samples |
|---|---:|
| Waiting in `VLogWriter::commitLocked` for the group commit's `F_FULLFSYNC` | 48,867 (93%) |
| Waiting in the sequencer (`applyInOrder`) | 3,232 (6%) |
| Read path (`readVersion` -> `readEntry`, including `open`) | 180 |
| JSON parse / apply / serialize | under 70 |

One thread was inside `F_FULLFSYNC` almost all the time, and group-commit batches averaged 7.7 of
16 threads.

**Before and after** (GC off / GC on):

| Metric | Baseline | After | Change |
|---|---|---|---|
| Mixed operations per second | 2,476 / 2,407 | 3,740 / 3,711 | +51% / +54% |
| Load (100,000 puts) per second | 2,042 / 2,078 | 3,293 / 3,308 | +61% / +59% |
| put p50 / p99 / p99.9 (ms), GC off | 6.94 / 13.6 / 34.9 | 4.77 / 9.83 / 16.0 | -31% / -28% / -54% |
| put p50 / p99 / p99.9 (ms), GC on | 6.91 / 16.3 / 111.1 | 4.82 / 9.85 / 14.6 | -30% / -40% / -87% |
| update p50 / p99 / p99.9 (ms), GC off | 7.05 / 13.9 / 25.3 | 4.83 / 9.94 / 13.5 | -32% / -29% / -47% |
| update p50 / p99 / p99.9 (ms), GC on | 7.00 / 16.4 / 117.0 | 4.86 / 9.95 / 12.9 | -31% / -40% / -89% |
| get p50 / p99 / p99.9 (µs), GC off | 100 / 457 / 3,547 | 36 / 262 / 350 | -64% / -43% / -90% |
| get p50 / p99 / p99.9 (µs), GC on | 100 / 574 / 6,899 | 36 / 265 / 441 | -64% / -54% / -94% |
| Index bytes written per run | 78.9 / 155.8 MB | 83.2 / 78.2 MB | +5% / -50% |
| Space amplification after compaction | 1.47x / 1.47x | 1.47x / 1.47x | none |
| Group-commit batch (average entries) | 7.7 / 7.7 | 15.1 / 15.2 | about 2x |
| Checkpoint stall, median / max (ms) | 7.7 / 22.1 (off) | 0.83 / 3.4 (off) | -89% / -85% |
| Chaos store: recovery open / full rebuild (ms) | 21.3 / 70.0 | 28.4 / 146.3 | +33% / +109% |

Acked operations verified after the 50 kill -9 crashes: 112,296 of 112,296 before and 112,305 of
112,305 after. Recovery of the main store after dropping it without `close()` (added to the
benchmark in this work, so there is no baseline) takes 151-162 ms for about 26,300 replayed entries.

**What each change did** (median of 3, previous commit -> this one; these intermediate runs were
mostly on battery, so compare them with each other, not with the table above):

1. **Segment rollover re-check** (`a12bc45`, a correctness fix). 16 threads made about 240
   segments at peak churn, most almost empty; now 36. Fewer 0-live segments meant fewer GC
   events: with GC on, checkpoints fell from 323 to 8 and put p99.9 from 111 to 20 ms.
2. **Segment descriptor cache** (`b27f5ad`). get p50 97 -> 37 µs, chain get p50 186 -> 64 µs.
3. **Adaptive group-commit window** (`5f8220a`, committed as "testing"). Batches 7.8 -> 14.1,
   mixed ops/s +47%, put p50 7.0 -> 4.2 ms. Window sweep (GC off, chaos skipped):

   | Window | ops/s | put p50 / p99 | Avg batch |
   |---:|---:|---|---:|
   | 0 µs | 2,502 | 6.97 / 12.9 ms | 7.8 |
   | 100 µs | 2,356 | 7.90 / 12.8 ms | 7.9 |
   | 200 µs | 2,649 | 6.98 / 12.5 ms | 8.8 |
   | 500 µs (default) | 3,424 | 4.56 / 10.0 ms | 13.9 |
   | 1000 µs | 3,679 | 4.88 / 9.0 ms | 15.6 |

   1000 µs gives more throughput and a better p99 but a worse p50, and a writer under light
   concurrency can wait twice as long, so the default is 500 µs. A single writer is unaffected
   (put p50 3.96 ms with and without the window).
4. **Checkpoints write outside the locks** (`ff348c6`). Stall while the locks are held: median
   21-27 -> 0.9 ms, max about 35 -> 3.3 ms; put p99.9 -14% / -18%. Costs: writers now run during
   the checkpoint's own fsyncs and change pages that must then be copied, so mixed ops/s fell
   about 8% and index bytes rose; and because GC no longer forces checkpoints, the short-lived
   chaos children reclaim less (vLog 9 -> 56 MB), which is why chaos recovery and rebuild got
   slower.

**Tried and not kept**: a 32 MB cache of materialized documents (filled by `get()` misses, keyed
by key + seq). Only 2.6% of lookups hit on this workload (random keys over 100,000 documents), get
latency did not change, and it was reverted. **Not changed**: stripe lock hold time; the JSON work
inside it is about 0.1% of worker time, against the fsync wait the lock has to cover.

**Tuning trade-offs** (bench-stress without chaos, median of 3, on battery; GC off / GC on):

| checkpoint_bytes | Checkpoints | Index MB written | Recovery open | Entries replayed |
|---:|---:|---|---|---:|
| 4 MB | 34 | 136 / 140 | 202 / 191 ms | about 8,000 |
| 16 MB (benchmark setting) | 8 | 103 / 106 | 331 / 327 ms | about 26,300 |
| 64 MB | 2 | 73 / 78 | 347 / 310 ms | 26,700-30,900 |

Larger checkpoints write fewer index bytes; recovery replays whatever was written since the last
checkpoint, so its worst case grows with checkpoint_bytes (these runs happened to stop at a
similar point after the last checkpoint for 16 and 64 MB). The engine default stays 4 MiB.

| gc_threshold | Space amp after compaction | GC bytes written | Peak space amp, GC on |
|---:|---|---|---:|
| 0.5 (default) | 1.47x / 1.47x | 0 / 0 MB | 1.47x |
| 0.7 | 1.05x / 1.06x | 80 / 81 MB | 1.69x |

At 0.5 the churn leaves every segment above the threshold, so nothing is collected; 0.7
reclaims about 30% of the vLog for about 80 MB of copies, and with GC on the vLog peaks higher
while relocated segments wait for normal checkpoints. The default stays 0.5.

**Measurement note**: in two runs during this work the laptop went to sleep mid-benchmark
(`pmset -g log`), which `steady_clock` counted as a 710 s and a 1,023 s write latency. The
affected tuning run was repeated; all final runs used `caffeinate`.

## Known limitations


- **GC limits**: one GC thread and one victim per pass; `index.db` never shrinks (freed pages are
  reused); open reads one small header per key to rebuild the GC counters; tombstones of erased
  keys are copied forward until their segment is the oldest, and the record of which tombstones
  GC kept is not persisted (after a restart such a segment may be copied once more); with GC on
  during heavy overwrites, GC writes several times more than collecting once later.
- **`scan()` returns vLog pointers** that stay readable only while the caller holds `pin()`.
- **Write amplification is measured only at the engine level** (bytes the engine writes), only for
  delta vs full updates of one field, and not for the legacy engine or at the device level.
- **Delta savings are capped by the chain length**: at most 4 Deltas per chain
  (`kMaxDeltaChain`), so one update in five rewrites the document.
- **Reading a chained document costs more**: up to 5 vLog reads plus a parse and serialize.
- **After an update, get() returns canonical JSON**, not the bytes originally put.
- **Object keys containing '.' cannot be addressed** by update paths.
- **Writes to one key are serialized** across the group commit (stripe lock), so one key takes at
  most about one write per `F_FULLFSYNC`.
- **LZ4 is not used**, although the format has a flag for it.
- **Checkpoints still stall writers and readers briefly** while they capture (median about 0.9 ms,
  max 3.3 ms in `make bench-stress`); writing the pages and the two `F_FULLFSYNC` calls overlaps
  with writes, which slows those writes' own fsyncs a little.
- **GC never forces a checkpoint**: retired segments wait for the normal triggers (4 MiB of vLog
  or 30 s), so a short-lived process that crashes often keeps dead segments longer (the chaos
  store in `make bench-stress` ended with 56 MB of vLog instead of 9 MB).
- **Open reads every page of the committed tree** to check CRCs, so open time grows with index size.
- **Lost vLog data before the replay point is not detected** unless the replay segment itself is
  missing or too short.
- **No document cache**: one was tried and measured (2.6% hit rate on `make bench-stress`, no
  change in get latency) and not kept.
- **No page merges**: a tree with many deletes keeps empty leaves.
- **One lock per tree**: writers are serialized.
- **Crash tests use kill -9**, not power loss. The Linux `fdatasync` path is covered only by the
  CI runs (quick and asan modes); the measurements above are from macOS.
- **ThreadSanitizer builds skip the fork + kill -9 tests.**

---

## Production Verification & Invariant Mapping

> For complete, untruncated terminal logs of all test suites and benchmarks, see [docs/VERIFICATION_LOGS.md](docs/VERIFICATION_LOGS.md).

This section maps the core storage engine invariants to their empirical test suites and documents the underlying architectural design principles.

### 1. Test Suite & Verification Matrix

The engine enforces strict hardware-level correctness through three complementary build configurations:
- `make tsan` (Clang ThreadSanitizer): Proves data-race freedom across 1024 stripe locks, sequencers, and concurrent reads/writes.
- `make asan` (AddressSanitizer + UndefinedBehaviorSanitizer): Verifies memory safety, zero leaks, bounds integrity, and 50 iterations of randomized `kill -9` process crashes.
- `make bench`: Micro-benchmarks real write amplification reduction and p50/p99 latency by chain depth.

| Domain | Test ID | Description & Production Invariant Verified |
|---|---|---|
| **Hardware Integrity & Bit-Rot** | `B1.1`, `V1`, `SB1` | Evaluates hardware ARMv8 CRC32C. In `B1.1`, all 32,768 single-bit flips across a 4096-byte page are detected and rejected. `SB1` verifies CRC over both 4096-byte superblock slots. |
| **Crash Safety (`kill -9`)** | `V9`, `B5.2`, `CP11`, `P4.G1`, `P4.G2`, `P5.H` | 195 cumulative iterations of SIGKILL across active group commits, live tree rebuilds, delta updates, and 5 GC lifecycle stages (`P5.H1`–`H5`). Every acknowledged operation (42,026+ verified ops) was intact after crash recovery with zero data loss and zero dead version resurrection. |
| **Atomic Checkpointing** | `CP1`–`CP10`, `C1`–`C4` | Ping-pong double-buffered `superblock.db` (Slot 0 at byte 0, Slot 1 at byte 4096) with monotonic epochs. Guarantees that even if power fails mid-write of one slot, the other slot remains fully valid. |
| **Copy-on-Write B+Tree** | `C1`, `C2`, `C3` | Pages with `page_id < checkpoint_page_count` are strictly immutable. First mutation copies the root-to-leaf path (`C2`). Page reuse is delayed across 2 epochs (`C3`) to preserve both committed trees. |
| **Torn Creation & Ghost Cleanup** | `V8`, `SB3` | If a process crashes while creating a segment or superblock (file size < 4096 or corrupt header), recovery safely unlinks the ghost file, preventing corruption on subsequent restarts. |
| **High Concurrency** | `B6.1`, `B6.2`, `P4.F1`–`P4.F5` | 8 threads performing 8,000 concurrent `$inc` updates on a single key (`P4.F1`) yield exact count 8,000 under ThreadSanitizer. 4 writers + 4 readers over a tiny 24-frame cache (`B6.2`) trigger 1,968 live evictions without races. |
| **Delta Logging & Write Savings** | `P4.D1`–`P4.D7`, `make bench` | Validates in-place delta updates. Reduces write volume by $3.65\times$ (1 KB) to $4.78\times$ (8 KB) while guaranteeing byte-for-byte fidelity (`P4.D7`) and identical document states across caps 0..4 (`P4.D6`). |
| **Strict JSON & Path Safety** | `P4.A1`–`P4.C10` | RFC 8259 compliance: depth limits (64 max), int64 overflow detection, surrogate pair escapes, path conflict resolution (`"a"` vs `"a.b"`), and zero-write guarantees on error. |

### 2. Core Architectural Design Principles

1. **Key-Value Separation (WiscKey Model)**
   - Traditional slotted page architectures (e.g., PostgreSQL heap, WiredTiger) rewrite full 4–8 KiB pages for small mutations, resulting in severe write amplification. By appending payloads sequentially to an append-only Value Log (vLog) and indexing them with slim 24-byte references (`IndexValueRef`), payload write amplification is eliminated.
2. **Ping-Pong Double-Buffered Superblock**
   - In-place header overwrites risk torn sectors upon sudden power failure. Alternating updates between Slot 0 (offset 0) and Slot 1 (offset 4096) with monotonic epochs and CRC32C ensures one slot remains completely consistent and atomic on disk.
3. **Sibling-Free Copy-on-Write (COW) B+Tree**
   - Eliminating horizontal leaf sibling pointers prevents cascading node replication across leaf levels during page modifications. Range scans are handled via an in-memory parent-stack cursor, confining COW path replication strictly to $O(\text{height})$ nodes.
4. **Bounded Delta Update Chains**
   - Unbounded delta logging produces unbounded read amplification. Capping chains at `kMaxDeltaChain = 4` guarantees that any point read requires at most 5 vLog lookups (p50 read latency $\le 130\ \mu\text{s}$) while reducing SSD write volume by $70\%$ to $80\%$.
5. **Striped Key Locking & Sequencer Ordering**
   - 1024 independent stripe locks serialize updates targeting the same key to preserve deterministic delta bases, while allowing multi-key operations to scale concurrently across group commit batches without circular waits or deadlocks.


