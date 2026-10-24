# LSM: a document store with key-value separation

A C++17 storage engine: documents go to an append-only value log (vLog), a disk-based B+Tree maps
each key to the location of its newest document, and checkpoints plus a two-slot superblock make
crash recovery replay only the tail of the log.

## The problem

The earlier engine in `legacy/` (MongoLite) stored documents in 4 KiB slotted pages and updated
them in place behind a write-ahead log. A small update to one document rewrites a whole page plus
its WAL record, and a document that grows has to move. Bytes written to disk per byte of user data
(write amplification) grow with page size, not with document size.

This engine separates keys from values, in the style of WiscKey:

- a document is written **once**, appended to the vLog;
- the index holds only small fixed-size references (`seq`, segment, offset, length), so index pages
  change far less data than the documents themselves.

Write amplification of the two designs has not been measured yet (see Known limitations).

## Design

```
put(key, doc) --> vLog append (group commit, F_FULLFSYNC) --> apply to B+Tree in seq order --> ack
get(key)      --> B+Tree lookup --> vLog read at (segment, offset), CRC checked
checkpoint    --> write dirty index pages --> F_FULLFSYNC --> superblock (other slot) --> F_FULLFSYNC
recovery      --> superblock --> open tree at its root --> replay vLog from its replay point
```

**Value log** (`include/vlog/`)
- Segments `vlog_%08u.log`, each starting with a 4 KiB header. Entries are 8-byte aligned and carry
  a CRC32C over everything after the CRC field. Seq and offset are assigned under one lock, so file
  order is seq order.
- Group commit: one thread writes everything staged so far with one `write()` and one
  `F_FULLFSYNC`, then acks the batch. A failed fsync aborts the process and is never retried.
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
  superblock is never used with another store's files.
- A checkpoint holds the sequencer lock and the tree's exclusive lock. It records the last seq
  applied to the index and the vLog position right after that entry (never the writer's tail).
- Index pages are fsynced **before** the superblock that names them is written. A crash before the
  superblock write leaves the previous checkpoint fully usable.
- Automatic checkpoints run every 4 MiB of vLog writes or 30 s, and on clean close.

**Recovery**
1. Check `index.db` header, the superblock, nonce, vLog identity, and every page of the committed
   tree (CRC + structure), before the vLog writer opens.
2. `ftruncate` `index.db` to the committed page count (drops pages written after the checkpoint).
3. Rebuild the free list by walking the current and previous trees.
4. The vLog writer's tail scan and the index replay both start at the replay point; the index uses
   `should_apply()` (newer seq wins, so replay is idempotent).

If anything in step 1 fails (no superblock, both slots corrupt, a bad tree page, files from another
store), the index is rebuilt from the whole vLog. If the replay point is missing from the vLog
(acked entries are gone), open fails with an error and changes nothing.

## Layout

```
include/vlog/    value log: on-disk format, CRC32C, writer, reader
include/index/   B+Tree: page format, pager, tree, superblock, recovery, IndexedStore
tests/           test binaries (built and run by the Makefile)
legacy/          earlier MongoLite engine (not built)
docs/            design notes (written before checkpoints were added; parts are out of date)
```

`include/vlog/` never includes `include/index/` or `legacy/`; `make` checks this.

## Build and test

Requires a C++17 compiler (tested with Apple clang 21 on macOS).

```
make test-quick  # optimized, smaller workloads, under 20 MiB written in total
make test        # optimized, full workloads
make asan        # AddressSanitizer + UndefinedBehaviorSanitizer
make tsan        # ThreadSanitizer (fork + kill -9 tests are skipped)
make clean
```

All builds use `-Wall -Wextra -Werror`. Each test prints one `[PASS] name (summary)` line; run with
`TEST_VERBOSE=1` to also print the measurements behind each test. Every test binary uses its own
temp directory, removes it on exit, aborts if free disk space is below 10 GB, and enforces a write
budget as it writes (512 MiB in total for `test`/`asan`/`tsan`, 20 MiB for `test-quick`).

| Binary | Covers |
|---|---|
| `test_vlog_crash` | CRC32C, reader bounds, torn tails, redo rule, torn segment creation, 50 x kill -9 during group commit, group commit benchmark |
| `test_bptree` | pager, tree vs `std::map`, splits, scans, checkpoint + reopen, corruption handling, IndexedStore, 50 x kill -9, concurrency |
| `test_edge_cases` | index ahead of the vLog, wrong vLog, bad VPtr, 16 MiB limit, races, height 4, injected I/O failures |
| `test_superblock` | slot CRCs, ping-pong writes, one or both slots corrupt, torn slot writes |
| `test_cow` | committed pages never modified, copy cascade, delayed page reuse, free list rebuilt at open |
| `test_checkpoint` | checkpoint contents and triggers, tail-only recovery, fallback rebuilds, page reuse safety, corrupt newest slot, 50 x kill -9 at random points |

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

**Tree shape**: 200,000 sequential 32-byte keys give a height-4 tree of 5,999 pages (24 MB).

These are single runs on one machine, not averaged benchmarks.

## Known limitations

- **No vLog garbage collection.** The log only grows. The vLog identity check uses the lowest
  segment, which has to move into the superblock once old segments can be deleted.
- **Write amplification is not measured** for this engine or the legacy one.
- **Only full documents are written.** The format defines delta entries and an LZ4 flag; the store
  does not use them yet.
- **Checkpoints stall the store** for the duration of two `F_FULLFSYNC` calls (about 10 ms here).
- **Open reads every page of the committed tree** to check CRCs, so open time grows with index size.
- **Lost vLog data before the replay point is not detected** unless the replay segment itself is
  missing or too short.
- **get() opens the segment file** for every read (no file handle cache, no value cache).
- **No page merges**: a tree with many deletes keeps empty leaves.
- **One lock per tree**: writers are serialized.
- **Crash tests use kill -9**, not power loss. Only macOS with `F_FULLFSYNC` is tested; the
  `fdatasync` path for other systems is untested.
- **ThreadSanitizer builds skip the fork + kill -9 tests.**
