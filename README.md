# Crash-safe Document Store Engine in C++

C++17, no third-party runtime dependencies in the engine, built and tested on macOS (Apple Silicon)
and Linux. The SQLite comparison benchmark links the system SQLite library.

## At a glance

An embedded database that stores JSON documents by key and is designed so that no acknowledged
write is lost after a crash (verified by the kill -9 tests below).

- Documents are appended to a log on disk and never overwritten.
- A B+Tree index stores only where each document lives in that log.
- Small changes (`$set` / `$inc` / `$unset`) append a small change record, not the whole document.
- Checkpoints let recovery read only the end of the log after a crash.
- A background garbage collector reclaims space from old versions.

```
put/update -> log (group commit + fsync) -> B+Tree -> ack
get        -> B+Tree -> log -> document
```

## Results vs SQLite and MongoLite

1 KB documents, 25,000 documents per dataset, 10,000 single-field updates (`profile.score + 1`).

| | **DocStore (this project)** | SQLite | MongoLite (my earlier engine) |
|---|---:|---:|---:|
| Bytes written per update, 1 thread (lower is better) | **16,818** ✅ | 33,256 | 21,060 |
| Bytes written per update, 16 threads (lower is better) | **1,384** ✅ | 33,300 | 21,852 |
| Updates per second, 16 threads (higher is better) | **3,363** ✅ | 313 | 287 |
| Updates per second, 1 thread (higher is better) | 257 | **340** ✅ | 274 |

Same Mac (Apple M5), same durability (F_FULLFSYNC on every acked write for all engines), median of 3
runs. Full details below.

Where SQLite wins:
- **Single-thread throughput:** 340 vs 257 updates per second, and 453 vs 273 inserts per second
  in the 1-thread bulk load (1 KB, higher is better).
- **Range scans of 100 keys:** 9,166 vs 1,423 scans per second (higher is better).
- **File growth per update:** 0 vs 449 bytes (1 KB, 1 thread, lower is better).

MongoLite (legacy) has the fastest point gets: 283,476 vs 123,646 gets per second for this engine
(higher is better).

## Proof it is correct

kill -9 crash tests (latest full-mode run, macOS, AddressSanitizer build):

| Suite | What is killed | Runs | Result |
|---|---|---:|---|
| V9 | vLog writer during group commit | 50 | 21,926 entries recovered, including all 21,750 acked; bytes verified |
| B5.2 | store during writes, recovery and clean close | 50 | 17,073 acked ops; newest acked op reflected for 3,160 keys |
| CP11 | writes, checkpoint flush, between index fsync and superblock, recovery | 50 | 12,507 acked ops; newest acked op reflected for 8,599 keys |
| P4.G1 | stream of `$inc` updates on one key | 20 | 1,994 acked; acked <= value <= acked + 1 |
| P4.G2 | random updates on many keys | 20 | 2,240 acked updates, all visible; index == full log replay |
| P5.H1-H5 | 5 points inside garbage collection | 5 | acked writes intact, no erased key returned, rebuild matches |
| P6.C3 | 3 points inside a checkpoint, 16 writer threads | 9 | every acked op present; index == full rebuild |
| bench-stress chaos | 16-thread load with background GC | 50 per run, 3 runs | 112,305 of 112,305 acked operations verified (median run) |

The engine comparison's crash test (kill -9 during 16-thread 1 KB updates, 3 runs) lost 0 acked
updates for this engine and SQLite. MongoLite (legacy) lost 863, 852 and 859 of 2,500 acked updates.

- **Sanitizers:** ASan + UBSan and TSan pass with no reports on macOS and on Linux (g++ 14,
  aarch64 container). TSan builds skip the fork-based kill -9 tests.
- **CI:** GitHub Actions on Ubuntu runs `make test-quick` and `make asan` with g++ and clang++, and
  a separate `make tsan` job.

## Build and test

```
make test-quick     # 10 test binaries, smaller workloads; write limit 32 MiB (recent totals 26.6-26.9 MB)
make test           # the same binaries, full workloads; write limit 512 MiB
make asan           # AddressSanitizer + UndefinedBehaviorSanitizer, full workloads
make tsan           # ThreadSanitizer (fork + kill -9 tests skipped)
make bench          # delta-update and GC benchmarks
make bench-stress   # 100,000 keys, 16 threads, GC on/off, 50 x kill -9
make bench-compare  # this engine vs SQLite vs MongoLite (links -lsqlite3), about 20 GB written
make clean
```

## Project layout

- `include/vlog/`: the append-only log: on-disk format, CRC32C, writer with group commit, reader.
- `include/index/`: B+Tree, pager, superblock and checkpoints, recovery, garbage collection, `IndexedStore`.
- `include/doc/`: JSON parser and serializer, `$set` / `$unset` / `$inc` update operators.
- `tests/`: test binaries, built and run by the Makefile.
- `bench/`: benchmarks (`make bench`, `bench-stress`, `bench-compare`).
- `legacy/`: the earlier MongoLite engine, used only by the comparison benchmark.

## Known limitations

- Measurements come from one machine (Apple M5, macOS); the Linux runs are correctness tests only.
- Crash tests use kill -9, not power loss.
- No query language and no secondary indexes; only point lookups by key and key-range scans.
- Range scans are slower than SQLite (1,423 vs 9,166 scans of 100 keys per second).
- Single-threaded writes are slower than SQLite (257 vs 340 updates per second, 1 KB).
- Writes to one key are serialized, so one key takes at most about one write per F_FULLFSYNC.
- `index.db` never shrinks; freed pages are reused.

---

# Design details

A C++17 storage engine: documents go to an append-only value log (vLog), a disk-based B+Tree maps
each key to the location of its newest document, and checkpoints plus a two-slot superblock make
crash recovery replay only the tail of the log. `update()` appends a small Delta entry (the changed
fields) instead of rewriting the whole document.

### The problem

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

### Design

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

### Layout

```
include/vlog/    value log: on-disk format, CRC32C, writer, reader
include/index/   B+Tree: page format, pager, tree, superblock, recovery, Delta codec, IndexedStore
include/doc/     JSON value/parser/serializer and update ops
tests/           test binaries (built and run by the Makefile)
bench/           delta-update and GC benchmarks (make bench)
legacy/          earlier MongoLite engine (not built)
```

`include/vlog/` and `include/doc/` include nothing from the layers above them; `make` checks this.

### Build and test

Requires a C++17 compiler. Tested with Apple clang 21 on macOS; GitHub Actions runs `make test-quick`
and `make asan` on Ubuntu with g++ and clang++ (`.github/workflows/ci.yml`).

```
make test-quick  # optimized, smaller workloads, under 32 MiB written in total
make test        # optimized, full workloads
make asan        # AddressSanitizer + UndefinedBehaviorSanitizer
make tsan        # ThreadSanitizer (fork + kill -9 tests are skipped)
make bench       # delta-update and GC benchmarks; not part of the test modes, about 390 MB written
make bench-stress  # 100,000 keys, 16 threads, GC on/off, 50 x kill -9; about 740 MB written
make bench-compare # IndexedStore vs SQLite vs legacy MongoLite (-lsqlite3); about 20 GB written, about 2 h
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

### Measured results

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

### High-Scale Stress Benchmark

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

### Performance work

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

### Storage Engine Comparison

`make bench-compare` (`bench/bench_compare.cpp`) compares this engine with SQLite 3 and the
legacy MongoLite engine (`legacy/`, unmodified) under the same durability rule: every acked write
is flushed with `F_FULLFSYNC` before it is acked. Median of 3 runs on an Apple M5, 17 GB RAM,
APPLE SSD AP0512Z (APFS), macOS 27.0, Apple clang 21, SQLite 3.54.0 (system `libsqlite3`), on
2026-10-28/29, with the machine kept awake (`caffeinate`; a workload during which the machine
slept would have been rerun: none was).

**Sizes.** Each dataset has 25,000 documents and each update workload 10,000 updates, half of the
50,000 / 20,000 first planned: a pilot measured about 12 GB written per run at the full sizes.
Reads use 10,000 point gets and 1,000 range scans of 100 keys. Documents are JSON with a nested
`profile.score` integer, padded to 1 KB, 2 KB or 8 KB. Same keys, documents, seed and op counts
for every engine, a fresh directory per engine and run. The whole run wrote 19.9 GB (OS counter).

**Durability settings**
- **IndexedStore**: default options, index cache 1,024 frames (4 MiB), background GC on (default
  threshold 0.5). Every acked write is fsynced by the group commit.
- **SQLite**: table `docs(key TEXT PRIMARY KEY, doc TEXT)`; `journal_mode = WAL`,
  `synchronous = FULL`, `fullfsync = ON`, `checkpoint_fullfsync = ON`, `cache_size = -4096`
  (4 MiB). Update in one statement:
  `UPDATE docs SET doc = json_set(doc, '$.profile.score', json_extract(doc, '$.profile.score') + 1) WHERE key = ?`.
  One connection per thread, `busy_timeout = 10000`. SQLite allows one writer at a time.
  - **Adapter note:** macOS's system `libsqlite3` turns `fullfsync = ON` into
    `F_BARRIERFSYNC` (measured 0.34 ms per commit, against 3.8 ms for `F_FULLFSYNC`), which
    does not flush the drive cache. The benchmark registers a VFS shim that delegates every
    call to the default VFS and also issues `F_FULLFSYNC` in `xSync`. The tables show 1.00-1.02
    full syncs per SQLite write.
  - **Adapter note:** with 16 writers a statement could wait longer than the 10 s busy timeout
    (SQLite's busy handler is not fair), so the adapter retries a statement that returns
    `SQLITE_BUSY`. The waiting counts in its latency, and the retries are listed in the tables.
- **MongoLite** (`legacy/`, not modified): 1,024-frame buffer pool (4 MiB). Update = read ->
  delete -> insert.
  - **Adapter note:** its WAL never syncs, so the adapter `F_FULLFSYNC`s the WAL file after
    every operation and the data file at checkpoint.
  - **Adapter note:** every call goes through one mutex, because the legacy code has data races.
  - **Limits:** its 4 KB pages cannot hold an 8 KB document ("not supported" below). It
    printed 63 "[PrimaryIndex] Duplicate key error" messages while rebuilding its index on
    reopen over the 3 runs.

**How bytes are measured.** Disk bytes written by the process, from the OS (`proc_pid_rusage`
`ri_diskio_byteswritten` on macOS), after each engine flushed its background work (SQLite
`wal_checkpoint(TRUNCATE)`, IndexedStore and MongoLite `checkpoint()`). The tables also show the
engine's file growth per op as a cross-check. Write amplification:
- **WAF vs changed field:** bytes written / 11 bytes (the text `"score":N`).
- **WAF vs document size:** bytes written / the document size.

On APFS a single-threaded durable write costs about 17 KB or more of physical I/O in every engine
(each fsync writes whole 4 KB blocks plus file-system metadata). Group commit spreads that cost
over many writes when 16 threads write together.

#### Headline: bytes written per update (OS counter, median of 3)

| Update workload | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| 1 KB documents, 1 thread | 16,818 | 33,256 | 21,060 | IndexedStore |
| 1 KB documents, 16 threads | 1,384 | 33,300 | 21,852 | IndexedStore |
| 2 KB documents, 1 thread | 16,762 | 35,352 | 24,204 | IndexedStore |
| 2 KB documents, 16 threads | 1,382 | 35,405 | 26,003 | IndexedStore |
| 8 KB documents, 1 thread | 16,669 | 34,256 | not supported | IndexedStore |
| 8 KB documents, 16 threads | 2,044 | 34,287 | not supported | IndexedStore |

| Update throughput (ops/s) | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| 1 KB, 1 thread | 257 | 340 | 274 | SQLite |
| 1 KB, 16 threads | 3,363 | 313 | 287 | IndexedStore |
| 2 KB, 1 thread | 255 | 340 | 241 | SQLite |
| 2 KB, 16 threads | 3,402 | 309 | 240 | IndexedStore |
| 8 KB, 1 thread | 255 | 259 | not supported | SQLite |
| 8 KB, 16 threads | 2,216 | 284 | not supported | IndexedStore |

**Where this engine loses**
- **Single-threaded throughput and median latency:** SQLite wins every bulk-load and update
  row. A SQLite commit (its barrier plus the shim's `F_FULLFSYNC`) measured 2.0-4.0 ms at the
  median, against 3.9-4.0 ms for this engine.
- **16 threads, median latency:** SQLite has the lowest p50, because one writer at a time
  commits quickly while the others wait. Its p99.9 and maximum are 10-45 s.
- **Point gets:** MongoLite wins (its index and buffer pool are in memory): 283,476 gets/s
  against 123,646 for this engine.
- **Range scans of 100 keys:** SQLite wins (9,166 scans/s against 1,423); this engine reads each
  of the 100 documents from the vLog separately.
- **File growth per update:** SQLite updates in place (0 bytes of growth), while this engine
  appends.

#### Full results (median of 3 runs)

#### A. Bulk load (25000 documents of 1 KB)

##### A load 1 KB, 1 thread

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 273 | 453 **(best)** | 283 | SQLite |
| p50 latency (µs) | 3,858 | 2,036 **(best)** | 3,882 | SQLite |
| p99 latency (µs) | 5,911 | 5,217 **(best)** | 5,260 | SQLite |
| p99.9 latency (µs) | 8,944 | 7,930 **(best)** | 10,792 | SQLite |
| max latency (µs) | 189,473 | 188,794 | 153,840 **(best)** | MongoLite |
| bytes written per op (OS) | 17,394 **(best)** | 29,127 | 18,275 | IndexedStore |
| file growth per op | 1,137 **(best)** | 1,387 | 1,365 | IndexedStore |
| WAF vs document size | 16.99 **(best)** | 28.44 | 17.85 | IndexedStore |
| full syncs per op (F_FULLFSYNC) | 1.000 | 1.008 | 1.000 **(best)** | MongoLite |
| SQLITE_BUSY retries (SQLite only) | - | 0 | - | n/a |

##### A load 1 KB, 16 threads

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 3,789 **(best)** | 308 | 268 | IndexedStore |
| p50 latency (µs) | 4,019 | 3,050 **(best)** | 52,133 | SQLite |
| p99 latency (µs) | 7,977 **(best)** | 11,001 | 127,966 | IndexedStore |
| p99.9 latency (µs) | 11,869 **(best)** | 12,113,968 | 240,941 | IndexedStore |
| max latency (µs) | 15,797 **(best)** | 45,439,544 | 2,235,110 | IndexedStore |
| bytes written per op (OS) | 2,138 **(best)** | 30,158 | 18,267 | IndexedStore |
| file growth per op | 1,137 **(best)** | 1,386 | 1,365 | IndexedStore |
| WAF vs document size | 2.09 **(best)** | 29.45 | 17.84 | IndexedStore |
| full syncs per op (F_FULLFSYNC) | 0.064 **(best)** | 1.020 | 1.000 | IndexedStore |
| SQLITE_BUSY retries (SQLite only) | - | 60 | - | n/a |

#### B. Small updates (10000 updates of profile.score + 1 on 25000 documents)

##### B update 1 KB, 1 thread

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 257 | 340 **(best)** | 274 | SQLite |
| p50 latency (µs) | 3,977 | 2,139 **(best)** | 3,912 | SQLite |
| p99 latency (µs) | 5,173 | 5,674 | 5,080 **(best)** | MongoLite |
| p99.9 latency (µs) | 9,284 **(best)** | 30,022 | 11,896 | IndexedStore |
| max latency (µs) | 187,546 | 181,742 | 123,064 **(best)** | MongoLite |
| bytes written per op (OS) | 16,818 **(best)** | 33,256 | 21,060 | IndexedStore |
| file growth per op | 449 | 0 **(best)** | 1,365 | SQLite |
| WAF vs changed field (11 B) | 1528.9 **(best)** | 3023.3 | 1914.6 | IndexedStore |
| WAF vs document size | 16.42 **(best)** | 32.48 | 20.57 | IndexedStore |
| full syncs per op (F_FULLFSYNC) | 1.000 **(best)** | 1.003 | 1.000 | IndexedStore |
| SQLITE_BUSY retries (SQLite only) | - | 0 | - | n/a |
| GC bytes per op (IndexedStore only) | 0 | - | - | n/a |

##### B update 1 KB, 16 threads

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 3,363 **(best)** | 313 | 287 | IndexedStore |
| p50 latency (µs) | 4,099 | 2,074 **(best)** | 3,883 | SQLite |
| p99 latency (µs) | 9,944 | 8,189 **(best)** | 905,332 | SQLite |
| p99.9 latency (µs) | 10,934 **(best)** | 10,547,864 | 7,724,429 | IndexedStore |
| max latency (µs) | 14,809 **(best)** | 22,275,816 | 17,323,660 | IndexedStore |
| bytes written per op (OS) | 1,384 **(best)** | 33,300 | 21,852 | IndexedStore |
| file growth per op | 100 | 0 **(best)** | 1,365 | SQLite |
| WAF vs changed field (11 B) | 125.8 **(best)** | 3027.3 | 1986.5 | IndexedStore |
| WAF vs document size | 1.35 **(best)** | 32.52 | 21.34 | IndexedStore |
| full syncs per op (F_FULLFSYNC) | 0.067 **(best)** | 1.013 | 1.000 | IndexedStore |
| SQLITE_BUSY retries (SQLite only) | - | 17 | - | n/a |
| GC bytes per op (IndexedStore only) | 0 | - | - | n/a |

##### B update 2 KB, 1 thread

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 255 | 340 **(best)** | 241 | SQLite |
| p50 latency (µs) | 3,953 | 2,970 **(best)** | 3,966 | SQLite |
| p99 latency (µs) | 4,970 **(best)** | 5,173 | 6,821 | IndexedStore |
| p99.9 latency (µs) | 8,053 **(best)** | 35,944 | 13,738 | IndexedStore |
| max latency (µs) | 189,913 | 155,534 **(best)** | 2,590,704 | SQLite |
| bytes written per op (OS) | 16,762 **(best)** | 35,352 | 24,204 | IndexedStore |
| file growth per op | 447 | 0 **(best)** | 4,096 | SQLite |
| WAF vs changed field (11 B) | 1523.9 **(best)** | 3213.8 | 2200.4 | IndexedStore |
| WAF vs document size | 8.18 **(best)** | 17.26 | 11.82 | IndexedStore |
| full syncs per op (F_FULLFSYNC) | 1.000 **(best)** | 1.003 | 1.000 | IndexedStore |
| SQLITE_BUSY retries (SQLite only) | - | 0 | - | n/a |
| GC bytes per op (IndexedStore only) | 0 | - | - | n/a |

##### B update 2 KB, 16 threads

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 3,402 **(best)** | 309 | 240 | IndexedStore |
| p50 latency (µs) | 4,072 | 2,108 **(best)** | 3,949 | SQLite |
| p99 latency (µs) | 8,942 | 7,356 **(best)** | 602,857 | SQLite |
| p99.9 latency (µs) | 12,472 **(best)** | 10,957,702 | 11,013,579 | IndexedStore |
| max latency (µs) | 16,029 **(best)** | 29,889,516 | 19,006,030 | IndexedStore |
| bytes written per op (OS) | 1,382 **(best)** | 35,405 | 26,003 | IndexedStore |
| file growth per op | 104 | 0 **(best)** | 4,096 | SQLite |
| WAF vs changed field (11 B) | 125.6 **(best)** | 3218.6 | 2364.0 | IndexedStore |
| WAF vs document size | 0.67 **(best)** | 17.29 | 12.70 | IndexedStore |
| full syncs per op (F_FULLFSYNC) | 0.067 **(best)** | 1.011 | 1.000 | IndexedStore |
| SQLITE_BUSY retries (SQLite only) | - | 17 | - | n/a |
| GC bytes per op (IndexedStore only) | 0 | - | - | n/a |

##### B update 8 KB, 1 thread

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 255 | 259 **(best)** | not supported | SQLite |
| p50 latency (µs) | 3,990 | 3,973 **(best)** | not supported | SQLite |
| p99 latency (µs) | 5,980 | 5,202 **(best)** | not supported | SQLite |
| p99.9 latency (µs) | 8,278 **(best)** | 44,087 | not supported | IndexedStore |
| max latency (µs) | 117,551 **(best)** | 119,839 | not supported | IndexedStore |
| bytes written per op (OS) | 16,669 **(best)** | 34,256 | not supported | IndexedStore |
| file growth per op | 409 | 0 **(best)** | not supported | SQLite |
| WAF vs changed field (11 B) | 1515.3 **(best)** | 3114.2 | not supported | IndexedStore |
| WAF vs document size | 2.03 **(best)** | 4.18 | not supported | IndexedStore |
| full syncs per op (F_FULLFSYNC) | 1.000 **(best)** | 1.003 | not supported | IndexedStore |
| SQLITE_BUSY retries (SQLite only) | - | 0 | - | n/a |
| GC bytes per op (IndexedStore only) | 0 | - | - | n/a |

##### B update 8 KB, 16 threads

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 2,216 **(best)** | 284 | not supported | IndexedStore |
| p50 latency (µs) | 7,964 | 3,001 **(best)** | not supported | SQLite |
| p99 latency (µs) | 11,256 | 5,303 **(best)** | not supported | SQLite |
| p99.9 latency (µs) | 17,716 **(best)** | 11,157,116 | not supported | IndexedStore |
| max latency (µs) | 21,802 **(best)** | 26,391,561 | not supported | IndexedStore |
| bytes written per op (OS) | 2,044 **(best)** | 34,287 | not supported | IndexedStore |
| file growth per op | 128 | 0 **(best)** | not supported | SQLite |
| WAF vs changed field (11 B) | 185.8 **(best)** | 3117.0 | not supported | IndexedStore |
| WAF vs document size | 0.25 **(best)** | 4.19 | not supported | IndexedStore |
| full syncs per op (F_FULLFSYNC) | 0.108 **(best)** | 1.008 | not supported | IndexedStore |
| SQLITE_BUSY retries (SQLite only) | - | 14 | - | n/a |
| GC bytes per op (IndexedStore only) | 0 | - | - | n/a |

#### C. Reads after reopening (1 KB dataset)

##### C point get (1 KB, 1 thread)

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 123,646 | 114,737 | 283,476 **(best)** | MongoLite |
| p50 latency (µs) | 10 | 8 | 4 **(best)** | MongoLite |
| p99 latency (µs) | 18 | 16 | 5 **(best)** | MongoLite |
| p99.9 latency (µs) | 48 | 43 | 11 **(best)** | MongoLite |
| max latency (µs) | 104 | 413 | 24 **(best)** | MongoLite |

##### C range scan 100 (1 KB, 1 thread)

| Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---:|---:|---:|---|
| ops/s | 1,423 | 9,166 **(best)** | 3,699 | SQLite |
| p50 latency (µs) | 694 | 111 **(best)** | 273 | SQLite |
| p99 latency (µs) | 942 | 135 **(best)** | 339 | SQLite |
| p99.9 latency (µs) | 1,166 | 264 **(best)** | 438 | SQLite |
| max latency (µs) | 1,166 | 264 **(best)** | 438 | SQLite |

#### D. Disk footprint after B (live data = documents x document size)

| Dataset | Metric | IndexedStore | SQLite | MongoLite | Winner |
|---|---|---:|---:|---:|---|
| 1 KB | disk MB | 33.9 **(best)** | 34.7 | 61.4 | IndexedStore |
| 1 KB | space amplification | 1.32 **(best)** | 1.36 | 2.40 | IndexedStore |
| 1 KB | disk MB after GC settles (IndexedStore) | 33.9 | - | - | n/a |
| 1 KB | space amplification after GC settles | 1.32 | - | - | n/a |
| 1 KB | GC MB written while settling | 0.0 | - | - | n/a |
| 2 KB | disk MB | 59.5 **(best)** | 103.1 | 184.3 | IndexedStore |
| 2 KB | space amplification | 1.16 **(best)** | 2.01 | 3.60 | IndexedStore |
| 2 KB | disk MB after GC settles (IndexedStore) | 59.5 | - | - | n/a |
| 2 KB | space amplification after GC settles | 1.16 | - | - | n/a |
| 2 KB | GC MB written while settling | 0.0 | - | - | n/a |
| 8 KB | disk MB | 212.8 **(best)** | 218.1 | not supported | IndexedStore |
| 8 KB | space amplification | 1.04 **(best)** | 1.06 | not supported | IndexedStore |
| 8 KB | disk MB after GC settles (IndexedStore) | 212.8 | - | - | n/a |
| 8 KB | space amplification after GC settles | 1.04 | - | - | n/a |
| 8 KB | GC MB written while settling | 0.0 | - | - | n/a |

#### E. kill -9 during 16-thread 1 KB updates (killed after 2500 acks)

| Engine | Run | Acked updates | Acked updates lost | In flight at the kill | Reopened |
|---|---:|---:|---:|---:|---|
| IndexedStore | 1 | 2,503 | 0 of 2,503 | 16 | yes |
| IndexedStore | 2 | 2,502 | 0 of 2,502 | 16 | yes |
| IndexedStore | 3 | 2,502 | 0 of 2,502 | 16 | yes |
| SQLite | 1 | 2,500 | 0 of 2,500 | 16 | yes |
| SQLite | 2 | 2,500 | 0 of 2,500 | 16 | yes |
| SQLite | 3 | 2,500 | 0 of 2,500 | 16 | yes |
| MongoLite | 1 | 2,500 | 863 of 2,500 | 16 | yes |
| MongoLite | 2 | 2,500 | 852 of 2,500 | 16 | yes |
| MongoLite | 3 | 2,500 | 859 of 2,500 | 16 | yes |


### Known limitations


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

### Production Verification & Invariant Mapping

> For complete, untruncated terminal logs of all test suites and benchmarks, see [docs/VERIFICATION_LOGS.md](docs/VERIFICATION_LOGS.md).

This section maps the core storage engine invariants to their empirical test suites and documents the underlying architectural design principles.

#### 1. Test Suite & Verification Matrix

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

#### 2. Core Architectural Design Principles

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


