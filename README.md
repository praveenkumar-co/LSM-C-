# LSM & Storage Engine Systems Design Workspace

A C++17 storage engine built in phases: an append-only value log (vLog, Phase 1) and a disk-based
B+Tree primary index over it (Phase 2). The older MongoLite engine is kept in `legacy/` for reference.

## Layout

```
include/vlog/    Phase 1: value log (on-disk format, CRC32C, writer, reader)
include/index/   Phase 2: B+Tree index (page format, pager, tree, recovery, IndexedStore)
tests/           Tests for vlog/ and index/ (run by the Makefile)
legacy/          Old MongoLite engine and its benchmarks (not built by the Makefile)
docs/            Plans and design notes
```

Dependency rule: `include/vlog/` never includes `include/index/` or `legacy/` (`make` checks this).

## Build and test

```
make test    # optimized build, -Werror
make asan    # AddressSanitizer + UndefinedBehaviorSanitizer, -Werror
make tsan    # ThreadSanitizer, -Werror (fork/kill -9 tests are skipped under TSan)
make clean
```

Every test uses its own temp directory, deletes it on exit, writes at most 512 MiB in total, and
aborts if free disk space is below 10 GB.

## Phase 1: value log (`include/vlog/`)
1. [`VLogFormat.hpp`](include/vlog/VLogFormat.hpp): on-disk format (segment header, entry header, deltas, `IndexValueRef`, superblock) and the `should_apply()` redo rule.
2. [`CRC32.hpp`](include/vlog/CRC32.hpp): CRC32C (ARMv8 / SSE4.2 instructions, table fallback).
3. [`VLogWriter.hpp`](include/vlog/VLogWriter.hpp): append with group commit and `F_FULLFSYNC`; segment rollover; recovery on open.
4. [`VLogReader.hpp`](include/vlog/VLogReader.hpp): point reads by `VPtr`; segment scan with torn-tail truncation and mid-log corruption detection.

## Phase 2: B+Tree index (`include/index/`)
1. [`BPTreeFormat.hpp`](include/index/BPTreeFormat.hpp): 4 KiB page header with CRC32C, meta page, key order.
2. [`Pager.hpp`](include/index/Pager.hpp): page cache over `index.db` (LRU, pin/unpin), `clean_shutdown` protocol.
3. [`BPTree.hpp`](include/index/BPTree.hpp): put/get/erase/scan, leaf/internal/root splits (no merges), one `std::shared_mutex`, invariant checker.
4. [`IndexRecovery.hpp`](include/index/IndexRecovery.hpp): open `index.db`, or rebuild it from the vLog after a crash.
5. [`IndexedStore.hpp`](include/index/IndexedStore.hpp): engine API — put → vLog append → index update in seq order → ack; get → index → vLog read.

## Tests (`tests/`)
- [`test_vlog_crash.cpp`](tests/test_vlog_crash.cpp): vLog CRC, header bounds, torn tails, redo rule, torn segment creation, 50 kill -9 iterations, group commit measurements.
- [`test_bptree.cpp`](tests/test_bptree.cpp): pager, tree vs `std::map` oracle, splits, page-boundary scans, persistence and rebuild, kill -9 of the store, concurrency.
- [`test_common.hpp`](tests/test_common.hpp): shared temp dir, write budget and disk space checks.

## Docs (`docs/`)
- [`PHASE2_PLAN.md`](docs/PHASE2_PLAN.md): Phase 2 design (page layout, split algorithm, tests, open questions).
- [`IMPLEMENTATION_DOSSIER.md`](docs/IMPLEMENTATION_DOSSIER.md): implementation notes.
- [`MASTER_CONTEXT_DATABASE_SYSTEMS_DESIGN.md`](docs/MASTER_CONTEXT_DATABASE_SYSTEMS_DESIGN.md): research summary and roadmap.

## Legacy engine (`legacy/`)
Not modified and not built by the Makefile. Numbers quoted for these files come from earlier runs and were not re-measured.
1. [`SlottedPage.hpp`](legacy/SlottedPage.hpp): 4KB dual-growth slotted page binary format (Petrov Ch. 3).
2. [`DiskManager.hpp`](legacy/DiskManager.hpp): Physical 4KB block I/O on disk.
3. [`LRUReplacer.hpp`](legacy/LRUReplacer.hpp): $O(1)$ LRU frame replacer (Petrov Ch. 5).
4. [`BufferPoolManager.hpp`](legacy/BufferPoolManager.hpp): In-memory frame cache with Steal eviction policy.
5. [`WALManager.hpp`](legacy/WALManager.hpp): Append-only Write-Ahead Logging.
6. [`DocumentStore.hpp`](legacy/DocumentStore.hpp): Multi-page collection manager with `RecordId(page_id, slot_id)`.
7. [`PrimaryIndex.hpp`](legacy/PrimaryIndex.hpp): B-Tree index with `std::shared_mutex` and Range Scan API.
8. [`MongoLite.hpp`](legacy/MongoLite.hpp): High-level document database API and streaming `Cursor`.

Benchmarks and tests:
- [`test_range_scan.cpp`](legacy/test_range_scan.cpp): 9-stage range query and multi-threaded concurrency benchmark (earlier run: 134,583 range QPS, 3.5M docs/sec).
- [`concurrent_bench.cpp`](legacy/concurrent_bench.cpp): Multi-threaded reader/writer stress test (earlier run: 1.52M read QPS).
- [`benchmark_mongolite.cpp`](legacy/benchmark_mongolite.cpp): Sequential insert & point lookup latency test.
- [`main_mongolite.cpp`](legacy/main_mongolite.cpp): Smoke test and crash recovery check.

## Research notes (`.t` files, not checked into this repository)
- `WiskkeyandBlobStoreDB.t`: Key-Value Separation, vLog, GC.
- `Important.lsmt9.t`: io_uring, SIMD, MVCC, CRC32, EBR, RESP.
- `DatabaseHardware.t`: mmap vs File.Read, zero-copy, kernel page cache.
- `Disadvantages.t`: RUM conjecture, compaction stalls, amplification paradox.
- `atomicPointer.t` & `ImmutablePointer.t`: CPU CAS primitives, lock-free structures.
- `Safety.t`: Hardware protection and sandbox containment.
- `lsm.t` to `lsmt8.t`: SSTable immutability, Bw-Tree, LeanStore, ScyllaDB.
