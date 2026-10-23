# LSM & Storage Engine Systems Design Workspace

This directory contains the entire conceptual, theoretical, architectural, and production code foundation for building a custom, high-performance database storage engine in C++.

## 📌 Master Context Dossier for Claude Opus / LLM Thinking
- **[`MASTER_CONTEXT_DATABASE_SYSTEMS_DESIGN.md`](file:///Users/praveen/Code/LSM/MASTER_CONTEXT_DATABASE_SYSTEMS_DESIGN.md)**: The 100% complete, unified dossier summarizing all 17 research notes, architectural decisions, mathematical trade-offs (RUM conjecture), hardware realities, code implementations, benchmarks, and the roadmap to a God-Tier engine.

---

## 🛠️ Implemented & Verified C++ Storage Engine Code
1. [`SlottedPage.hpp`](file:///Users/praveen/Code/LSM/SlottedPage.hpp): 4KB dual-growth slotted page binary format (Petrov Ch. 3).
2. [`DiskManager.hpp`](file:///Users/praveen/Code/LSM/DiskManager.hpp): Physical 4KB block I/O on disk.
3. [`LRUReplacer.hpp`](file:///Users/praveen/Code/LSM/LRUReplacer.hpp): $O(1)$ LRU frame replacer (Petrov Ch. 5).
4. [`BufferPoolManager.hpp`](file:///Users/praveen/Code/LSM/BufferPoolManager.hpp): In-memory frame cache with Steal eviction policy.
5. [`WALManager.hpp`](file:///Users/praveen/Code/LSM/WALManager.hpp): Append-only Write-Ahead Logging for ACID durability.
6. [`DocumentStore.hpp`](file:///Users/praveen/Code/LSM/DocumentStore.hpp): Multi-page collection manager with `RecordId(page_id, slot_id)`.
7. [`PrimaryIndex.hpp`](file:///Users/praveen/Code/LSM/PrimaryIndex.hpp): Thread-safe B-Tree index with `std::shared_mutex` and Range Scan API.
8. [`MongoLite.hpp`](file:///Users/praveen/Code/LSM/MongoLite.hpp): High-level document database API and streaming `Cursor`.

### Benchmark & Test Executables:
- [`test_range_scan.cpp`](file:///Users/praveen/Code/LSM/test_range_scan.cpp): 9-stage range query and multi-threaded concurrency benchmark (134,583 Range QPS, 3.5M docs/sec).
- [`concurrent_bench.cpp`](file:///Users/praveen/Code/LSM/concurrent_bench.cpp): Multi-threaded reader/writer stress test (1.52M read QPS, zero data races).
- [`benchmark_mongolite.cpp`](file:///Users/praveen/Code/LSM/benchmark_mongolite.cpp): Sequential insert & point lookup latency test.
- [`main_mongolite.cpp`](file:///Users/praveen/Code/LSM/main_mongolite.cpp): Smoke test and crash recovery verification.

---

## 📚 Architectural & Research Notes (`.t` files):
- `WiskkeyandBlobStoreDB.t`: Key-Value Separation, vLog, GC.
- `Important.lsmt9.t`: 6 God-Tier features (io_uring, SIMD Vector, MVCC, CRC32, EBR, RESP).
- `DatabaseHardware.t`: mmap vs File.Read, zero-copy, kernel page cache.
- `Disadvantages.t`: RUM conjecture, compaction stalls, amplification paradox.
- `atomicPointer.t` & `ImmutablePointer.t`: CPU CAS primitives, lock-free structures.
- `Safety.t`: Hardware protection and sandbox containment.
- `lsm.t` to `lsmt8.t`: SSTable immutability, Bw-Tree, LeanStore, ScyllaDB.
