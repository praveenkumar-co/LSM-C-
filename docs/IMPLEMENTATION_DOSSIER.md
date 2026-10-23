# Implementation & Verification Dossier: Production Value Log (vLog) Storage Engine

> **Document Type**: Storage Engine Implementation Specification & Verification Report  
> **Target Reviewer**: Claude Opus (Senior Systems & Storage Engine Architect)  
> **Source Repository**: `/Users/praveen/Code/LSM/`  
> **Implementation Language**: C++17 (`clang++` / `g++` on macOS Darwin ARM64, Apple Silicon)  
> **Status**: **Phase 1 (vLog Engine) 100% Implemented & Verified**. All 8 Claude Opus feedback items incorporated and passed across all 3 sanitizer build configurations. Ready for Phase 2 (Pointer-Swizzled B-Tree Index).

---

## 1. Architectural Model & Scope

### 1.1 Key-Value Separation
Traditional document storage engines (e.g., MongoDB WiredTiger slotted pages, PostgreSQL heap pages) suffer from severe **Write Amplification (WA)**:
- Updating a single 10-byte field in a 2KB JSON document requires dirtying and rewriting an entire 4KB / 8KB data page to disk ($400\times$ write amplification).
- Value separation decouples payload writes from index updates:
  1. **Payloads Go to Append-Only Log (vLog)**: Payloads are written once in sequential, 8-byte aligned append-only segment files.
  2. **Index Holds Slim 24-Byte References (`IndexValueRef`)**: The primary B-Tree stores only the key and a compact 24-byte struct containing value pointers (`VPtr`), delta pointers, and version sequence numbers.
  3. **Write Amplification Reduction**: Payload write amplification is eliminated. Index-level write amplification is controlled through group commit batching and checkpoint intervals.

### 1.2 Scope Boundaries & Disclaimers
- **Process-Crash Recovery vs. Power-Loss Durability**: Verified using 50 iterations of randomized `kill -9` (`SIGKILL`) with pipe-acknowledged sequence verification and deterministic payload byte validation. This guarantees process-crash recovery and torn-write truncation. Physical NVMe durability is enforced via `fcntl(fd, F_FULLFSYNC)` (macOS) / `fdatasync(fd)` (Linux), but full sudden power-loss survival requires hardware drive power-loss capacitor testing beyond OS scope.
- **Scope Isolation**: This dossier covers the isolated, production-grade **Value Log (vLog) storage engine**. Legacy MongoLite engine gaps (WAL replay, slotted page data races) remain untouched until Phase 2, where the pointer-swizzled B-Tree replaces the legacy `DocumentStore` and `PrimaryIndex`.

---

## 2. On-Disk Binary Specification (`VLogFormat.hpp`)

All structures use Claude's exact verbatim `VLogFormat.hpp` specification, packed with `#pragma pack(push, 1)` and guarded by compile-time `static_assert` statements.

```
+-----------------------------------------------------------------------------------------+
| Segment Header (40B): Magic 0x474F4C5653495757 | Ver | SegID | FirstSeq | Created | Flags | CRC |
+-----------------------------------------------------------------------------------------+
| Entry 1: CRC32C (4B) | TotalLen (4B) | Seq (8B) | Type (1B) | Flags (1B) | KeyLen (2B)   |
|          PayloadLen (4B) | RawLen (4B) | Reserved (4B) | Key ... | Payload ... | Pad 8B |
+-----------------------------------------------------------------------------------------+
| Entry 2: CRC32C (4B) | TotalLen (4B) | Seq (8B) | ...                                   |
+-----------------------------------------------------------------------------------------+
```

### 2.1 Struct Definitions

#### 1. `VPtr` (12 Bytes)
Compact pointer to any payload in the vLog:
```cpp
struct VPtr {
    uint32_t segment_id; // Identifies segment file vlog_{segment_id}.log
    uint32_t offset;     // Entry start within segment; >= kBlockSize, 8-aligned
    uint32_t length;     // == EntryHeader::total_len
};
static_assert(sizeof(VPtr) == 12);
```

#### 2. `SegmentHeader` (40 Bytes)
Written once at offset 0 of every segment file. Does not contain `max_seq` (no in-place header rewrites); includes `crc32c`:
```cpp
struct SegmentHeader {
    uint64_t magic;          // kSegmentMagic = 0x474F4C5653495757ULL ("WWISVLOG")
    uint32_t format_version; // kFormatVersion = 1
    uint32_t segment_id;     // Must match the file name
    uint64_t first_seq;      // Starting sequence number
    uint64_t created_unix_ns;// Epoch nanoseconds
    uint32_t flags;          // Reserved, 0
    uint32_t crc32c;         // Over the 36 bytes above
};
static_assert(sizeof(SegmentHeader) == 40);
```

#### 3. `EntryHeader` (32 Bytes)
Precedes every entry. `crc32c` is the first field; CRC covers bytes `[4, total_len)`, including `total_len` and `seq`:
```cpp
struct EntryHeader {
    uint32_t crc32c;       // Over bytes [4, total_len), padding included
    uint32_t total_len;    // 32 + align8(key_len) + align8(payload_len)
    uint64_t seq;          // Global monotonic version
    uint8_t  type;         // EntryType (1=Full, 2=Delta, 3=Tombstone)
    uint8_t  flags;        // EntryFlags (LZ4, Relocated, Consolidated)
    uint16_t key_len;      // Key length in bytes
    uint32_t payload_len;  // Bytes on disk (after compression)
    uint32_t raw_len;      // Logical uncompressed bytes
    uint32_t reserved;     // Must be 0; covered by CRC
};
static_assert(sizeof(EntryHeader) == 32);
static_assert(offsetof(EntryHeader, seq) == 8);
static_assert(offsetof(EntryHeader, payload_len) == 20);
```

#### 4. `DeltaPrefix` (24 Bytes) & `DeltaOpHeader` (8 Bytes)
For delta updates:
```cpp
struct DeltaPrefix {
    uint64_t base_seq;     // Version this delta applies to
    VPtr     base;         // Full VPtr of base version
    uint16_t chain_depth;  // 1 => base is Full; <= kMaxDeltaChain
    uint16_t op_count;     // Number of delta ops
};
static_assert(sizeof(DeltaPrefix) == 24);
static_assert(offsetof(DeltaPrefix, chain_depth) == 20);

struct DeltaOpHeader {
    uint8_t  opcode;       // DeltaOpCode (1=Set, 2=Unset, 3=IncI64, 4=IncF64)
    uint8_t  encoding;     // ValueEncoding (0=None, 1=JsonText, 2=I64, 3=F64)
    uint16_t path_len;     // JSONPath length (e.g., "balance")
    uint32_t value_len;    // 0 for Unset
};
static_assert(sizeof(DeltaOpHeader) == 8);
```

#### 5. `IndexValueRef` (24 Bytes)
The compact reference stored in the primary index:
```cpp
struct IndexValueRef {
    uint64_t seq;          // Version of latest Full or Delta; drives idempotent redo
    uint8_t  kind;         // 0 = inline, 1 = vLog
    uint8_t  reserved[3];
    VPtr     vptr;         // Valid when kind == 1
};
static_assert(sizeof(IndexValueRef) == 24);
static_assert(offsetof(IndexValueRef, vptr) == 12);
```

#### 6. `Superblock` (64 Bytes)
```cpp
struct Superblock {
    uint64_t magic;
    uint32_t format_version;
    uint32_t reserved0;
    uint64_t epoch;
    uint64_t checkpoint_seq;
    uint32_t replay_segment_id;
    uint32_t replay_offset;
    uint64_t index_root_page;
    uint64_t index_page_count;
    uint32_t oldest_live_segment;
    uint32_t crc32c;
};
static_assert(sizeof(Superblock) == 64);
static_assert(offsetof(Superblock, crc32c) == 60);
```

---

### 2.2 Redo Rule (`should_apply`)

Used during recovery and by GC:
```cpp
constexpr bool should_apply(bool has_key, uint64_t idx_seq, uint64_t entry_seq, uint8_t flags) {
    if (flags & kFlagRelocated) return has_key && idx_seq == entry_seq;
    return !has_key || entry_seq > idx_seq;
}
```
- **Full & Delta Entries**: Applied if `!has_key || entry_seq > idx_seq`. When applied, `idx_seq` advances to `entry_seq`.
- **Idempotence**: A second replay of the same log records encounters `entry_seq <= idx_seq` and ignores them, ensuring 100% idempotent redo replay.
- **GC Relocation**: Applied if and only if `idx_seq == entry_seq`. If a concurrent user write modified the key (`idx_seq > entry_seq`), the stale GC relocation is rejected.

---

## 3. Hardware-Accelerated ARMv8 CRC32C (`CRC32.hpp`)

- **Instruction Set**: Built with `-march=armv8-a+crc`. Employs Apple Silicon ARMv8 hardware instructions (`__crc32cb`, `__crc32ch`, `__crc32cw`, `__crc32cd`) with Castagnoli polynomial `0x82F63B78`.
- **Unaligned-Safe Loads**: Uses `std::memcpy` rather than `reinterpret_cast` for 8-byte chunk loading to prevent undefined behavior:
  ```cpp
  while (len >= 8) {
      uint64_t v;
      std::memcpy(&v, p, sizeof(v));
      crc = __crc32cd(crc, v);
      p += 8;
      len -= 8;
  }
  ```
- **Test Vector**: Validated bit-identical to software table fallback against:
  $$\text{CRC32C}(\text{"123456789"}) = \mathtt{0xE3069283}$$

---

## 4. Group Commit & Durability Pipeline (`VLogWriter.hpp`)

```
 Caller Thread 1 ──┐
 Caller Thread 2 ──┼─> [std::mutex queue_mutex_]
 Caller Thread 3 ──┘        │
                            ├── 1. Assign atomic seq & 8-byte aligned offset
                            ├── 2. Copy entry to staging_buffer_
                            ├── 3. Elect Sync Leader (flush_in_progress_ == false)
                            │
               ┌────────────┴─────────────────────────────┐
               ▼                                          ▼
       [Sync Leader Thread]                     [Follower Threads]
       1. batch_bytes.swap(staging_buffer_)     1. Wait on req->cv until
       2. UNLOCK queue_mutex_                      req->done == true
       3. write(fd, batch_bytes)                2. Return AppendResult{vptr, seq}
       4. fcntl(fd, F_FULLFSYNC)
       5. LOCK queue_mutex_
       6. Ack all batch requests
       7. If pending_requests_ non-empty, loop
       8. flush_in_progress_ = false; notify_all()
```

### 4.1 Key Guarantees
1. **Never Truncate Existing Data (Item 1)**:
   - On open, `VLogWriter::recoverAndInitLocked()` scans existing segments using `VLogReader`.
   - Recovers `current_seq_ = max recovered seq` and `current_segment_id_ = max segment id`.
   - Starts a NEW segment opened with `O_RDWR | O_CREAT | O_EXCL, 0644`. **Never uses `O_TRUNC`**.
2. **Fatal Path Thread Safety (Item 2)**:
   - On write or `F_FULLFSYNC` failure in `append()`, `flushStagingBufferLocked()`, or `openNewSegmentLocked()`, the engine marks **both `batch_reqs` and `pending_requests_` failed**, notifies every single waiting condition variable (`r->cv.notify_one()`), and invokes `std::abort()`.
   - No worker thread can hang or wait forever on a condition variable.
3. **Physical Flush Outside Lock**: `write()` and `fcntl(fd, F_FULLFSYNC)` execute entirely outside `queue_mutex_`, allowing concurrent threads to stage writes into `staging_buffer_` while the disk is syncing.
4. **Directory Sync on Segment Creation**: Executed via `sync_directory(dir_path)` (`fsync(dir_fd)` with return check).
5. **Safe Rollover Flush**: Pending staging buffer bytes are flushed and synced to the active segment before opening the new segment.

---

## 5. Recovery Scanner & Strict Corruption Rules (`VLogReader.hpp`)

### 5.1 Recovery Rules
1. **Rule 3 & 6: Safe Truncation & Return Checks**:
   - Every `ftruncate()` and `full_fsync()` is checked via `safeTruncateAndSync(int fd, uint64_t offset)`, throwing `std::runtime_error` if either system call fails.
   - If the last segment has an invalid or incomplete `SegmentHeader` (`magic` or `crc32c` mismatch, or size $< 4096$), it is truncated to 0 and the scan terminates immediately (Rule 6). It **never keeps scanning**.
2. **Rule 7 (Last Segment Only)**:
   - Torn tail truncation (`ftruncate` + `F_FULLFSYNC`) is allowed **only in the last segment**.
   - Any bad tail (incomplete header, bad bounds, or CRC mismatch) in a non-last segment is **FATAL**:
     `Fatal: Incomplete/torn entry at EOF in non-last segment {id}`
3. **Rule 8 (Forward Scan on Bounds Failure)**:
   - If header bounds check fails mid-file (`total_len < 32`, `total_len > 16MB`, unaligned, or `offset + total_len > file_size`), the reader scans forward at 8-byte aligned offsets up to 16 MiB for candidate valid entries.
   - If valid entries exist ahead: **FATAL: Mid-log corruption detected! Valid entries follow corrupt header**.
   - If no valid entries exist ahead:
     - Last segment $\to$ truncate and sync.
     - Non-last segment $\to$ fatal error.

---

## 6. Verification Test Suite (`test_vlog_crash.cpp`)

Every assertion in the test suite uses the `#define CHECK(cond)` macro, which strictly evaluates and aborts even under `NDEBUG` (Item 7).

### Stage 1: CRC32C Validation
- Validates `"123456789"` produces `0xE3069283` on Table and ARMv8 hardware.
- Status: **PASS**.

### Stage 2: Header Bounds & Corrupt Length Rejection (Item 5)
- Directly mutates the on-disk `total_len` at `vptr.offset + 4` and sets `bad_vptr.length` to match it.
- Confirms that `validateHeaderBounds()` is specifically the gate rejecting entries with `total_len < 32`, `total_len > 16MB`, unaligned length, or `offset + total_len > file_size`.
- Status: **PASS**.

### Stage 3: Torn Tail Truncation vs. Mid-Log Corruption Rules
- **Case A**: Torn tail in last segment $\to$ cleanly truncated.
- **Case B**: Bad tail in non-last segment $\to$ throws fatal exception: `Fatal: Incomplete/torn entry at EOF in non-last segment 1`.
- **Case C**: Mid-log bad CRC with valid entry ahead $\to$ throws fatal exception: `Fatal: Mid-log corruption detected! Valid entries follow bad CRC`.
- **Case D**: Mid-file bounds failure with valid entry ahead $\to$ throws fatal exception: `Fatal: Mid-log corruption detected! Valid entries follow corrupt header`.
- Status: **PASS**.

### Stage 4: Redo Rule & Idempotent Delta Replay
- Tests `NORMAL_PUT`, `TOMBSTONE`, and `GC_RELOCATED`.
- Writes full document `acct_99` (`balance: 1000`), delta 1 (`IncI64 +50`), delta 2 (`IncI64 +25`).
- Replay Pass 1: Balance reaches 1075, seq 3.
- Replay Pass 2: Exact same log replayed again; all entries skipped (`entry_seq <= idx_seq`); balance remains 1075.
- Status: **PASS**.

### Test A: Reopen Without Truncation (Item 1)
- Writes 100 entries into segment 1. Closes writer.
- Reopens `VLogWriter`: verifies it detects segment 1, recovers `max_seq = 100`, sets `current_segment_id_ = 1`, and opens segment 2 with `O_CREAT | O_EXCL`.
- Appends 1 entry (`seq = 101`).
- Scans segment 1 (100 entries) and segment 2 (1 entry). Total recovered = 101 entries, max seq = 101.
- Status: **PASS**.

### Test B: Torn-Write Fault Injection Matrix (Item 8a & Item 6)
- Evaluates 4 distinct fault injection scenarios:
  1. Torn `SegmentHeader` creation ($< 4096$ bytes): truncated to 0 in last segment; fatal error in non-last segment.
  2. Corrupted `SegmentHeader` magic/CRC (Rule 6): truncated to 0 in last segment and scan halted immediately; fatal error in non-last segment.
  3. Partial `EntryHeader` write at EOF (16 bytes): safely truncated in last segment.
  4. Incomplete payload write at EOF (header valid, payload cut short): safely truncated in last segment.
- Status: **PASS**.

### Test C: Mid-Checkpoint Redo & Tombstone State Machine (Item 8b)
- Simulates in-flight crash during checkpoint at `seq = 5`:
  - `key_A`: checkpointed at `seq = 5` (log seq 1 and seq 5 skipped).
  - `key_B`: old checkpoint `seq = 2` (log seq 7 applied).
  - `key_C`: checkpointed at `seq = 3` (tombstone seq 6 applied, then resurrected at seq 9).
  - `key_D`: checkpointed at `seq = 4` (tombstone seq 10 applied).
  - `key_E`: not in checkpoint (inserted at seq 8).
- Redo pass applied exactly 5 entries (seqs 6, 7, 8, 9, 10).
- Second replay pass applied exactly 0 entries (100% idempotent state convergence).
- Status: **PASS**.

### Stage 5: Process-Crash Recovery Under `kill -9` (50 Iterations)
- **Protocol**:
  - Child process runs 4 worker threads writing random documents with payload bytes deterministically derived from `seq` (`fill_deterministic_payload`).
  - Child transmits committed sequence numbers to parent via IPC pipe.
  - Parent supervisor process waits until **$\ge 200$ acks are received** from the pipe.
  - Parent sleeps random jitter (500 µs – 5 ms) and sends `SIGKILL` (`kill -9`).
  - Parent scans all segments dynamically.
  - **Verification**:
    1. Recovered sequences strictly increasing.
    2. 100% of pipe-acknowledged sequence numbers are recovered.
    3. Every single recovered entry's payload matches the deterministic pattern derived from its `seq`.
    4. Truncation reporting: Reports count of iterations triggering torn-tail truncation.
- **Sanitizer Compatibility (Item 4)**:
  Protected by GCC/Clang portable macros:
  ```cpp
  #if defined(TSAN_BUILD)
  #define VLOG_SKIP_FORK_KILL 1
  #elif defined(__has_feature)
  #if __has_feature(thread_sanitizer)
  #define VLOG_SKIP_FORK_KILL 1
  #endif
  #endif
  ```
- **Result**:
  ```
  Iteration [1/50] PASSED. Acked: 201, Total Recovered: 202
  Iteration [10/50] PASSED. Acked: 200, Total Recovered: 204
  Iteration [20/50] PASSED. Acked: 200, Total Recovered: 204
  Iteration [30/50] PASSED. Acked: 200, Total Recovered: 201
  Iteration [40/50] PASSED. Acked: 200, Total Recovered: 201
  Iteration [50/50] PASSED. Acked: 200, Total Recovered: 201
  PASS: All 50 random kill -9 crash iterations passed with ZERO acknowledged data loss!
  Report: Torn-tail truncation was triggered in 0 / 50 crash iterations.
  ```
- Status: **PASS**.

### Stage 6: Multi-Threaded Group Commit Scaling Benchmark
Executed on Apple Silicon NVMe with physical `fcntl(fd, F_FULLFSYNC)` barriers outside the mutex:

| Threads | Runtime | Committed Writes | Physical F_FULLFSYNCs | Writes/s | Barriers/s | Avg Batch Size |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **1** | 1.50s | 360 | 361 | **239.4** | 240.0 | **1.00** |
| **4** | 1.51s | 820 | 411 | **544.8** | 273.1 | **2.00** |
| **16** | 1.51s | 2,896 | 363 | **1,924.0** | 241.2 | **7.98** |
| **64** | 1.51s | 12,416 | 389 | **8,218.5** | 257.5 | **31.92** |

> **Key Takeaway**: Physical NVMe flash barriers stay constant at $\approx 240\text{--}273\text{ syncs/sec}$ ($\approx 3.7\text{--}4.1\text{ ms}$ per physical flash flush), while throughput scales smoothly from $239.4\text{ writes/s}$ to **$8,218.5\text{ writes/s}$** because batching efficiency scales from $1.00$ to $31.92\text{ writes/barrier}$.

---

## 7. Sanitizer Verification Matrix

| Build Configuration | Flags | Result | Correctness Observations |
|---|---|---|---|
| **Optimized Release** | `-O3 -march=armv8-a+crc -Wall -Wextra -Werror -pthread` | **PASS** | All stages and new tests passed; 50/50 crash iterations passed with zero lost writes. |
| **Address & UB Sanitizer** | `-fsanitize=address,undefined -g -march=armv8-a+crc -Wall -Wextra -Werror -pthread` | **PASS** | Zero memory leaks, zero misaligned accesses, zero UB. |
| **ThreadSanitizer** | `-fsanitize=thread -DTSAN_BUILD -g -march=armv8-a+crc -Wall -Wextra -Werror -pthread` | **PASS** | Zero data races detected in multi-threaded group commit queue. *(Fork test skipped under TSan)*. |

---

## 8. Summary of Addressed Claude Opus Feedback Items

| # | Item Description | Implementation Details | Verification Status |
|---|---|---|---|
| **1** | `VLogWriter` must never truncate on open | `recoverAndInitLocked()` scans segments via `VLogReader`, sets `current_seq_ = max_seq` and `current_segment_id_ = max_seg`, opens new segment with `O_CREAT \| O_EXCL` | Verified in `testReopenNoTruncation`: 100 entries + close + reopen + 1 entry $\to$ 101 recovered, seq 101 |
| **2** | Fatal path aborts and unblocks all requests | Marks both `batch_reqs` and `pending_requests_` failed, notifies `r->cv.notify_one()`, then calls `std::abort()` | Checked in rollover flush, segment creation, and batch commit paths |
| **3** | Check return value of every `ftruncate()` and `full_fsync()` | Handled via `safeTruncateAndSync(int fd, uint64_t offset)` which verifies return codes and throws on failure | Cleanly routes all reader truncations |
| **4** | GCC / Clang `-Werror` compatible sanitizer macros | Replaced with `#if defined(TSAN_BUILD)` and separate `#elif defined(__has_feature)` blocks | Clean compile with `-Wall -Wextra -Werror` |
| **5** | Stage 2 bounds testing via on-disk `total_len` corruption | Directly writes invalid lengths to offset + 4 and matches `vptr.length` so `validateHeaderBounds()` is the sole rejecting filter | Tested `<32`, `>16MB`, unaligned, and `>file_size` |
| **6** | Last segment bad `SegmentHeader` truncates to 0 and stops | Bad magic/CRC truncates to 0 and immediately returns without scanning | Verified in `testTornWriteFaultInjection` Subcase 2 |
| **7** | Replace `assert()` with `CHECK` macro | Defined `CHECK(cond)` macro evaluating conditions and aborting even under `NDEBUG` | All 39 `assert` instances replaced across test file |
| **8a**| Torn-write fault injection test | Added `testTornWriteFaultInjection` covering partial headers, corrupt magic, partial entry headers, and torn payloads | Verified across last and non-last segments |
| **8b**| Mid-checkpoint redo + tombstone test | Added `testMidCheckpointRedoTombstone` covering in-flight checkpoint crash, tombstones, key resurrection, and idempotent second replay | Verified 5 entries applied in pass 1, 0 applied in pass 2 |

---

## 9. Next Steps: Phase 2 (Pointer-Swizzled B-Tree Index)
Phase 1 (vLog Engine) is 100% complete, fully verified, and ready for integration with Phase 2:
1. **LeanStore Pointer Swizzling**: Convert 64-bit page IDs to direct virtual memory pointers in memory.
2. **Epoch-Based Reclamation & Optimistic Lock Coupling (OLC)**: Reader latches eliminated for $O(1)$ fast path reads.
3. **Index Integration**: Point `IndexValueRef.vptr` to the vLog segments created and managed by `VLogWriter` and `VLogReader`.
