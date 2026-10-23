# Phase 2 Plan: Simple Disk-Based B+Tree Primary Index

Status: **plan only**. No code has been written for Phase 2.

## 0. Scope

In scope:
- A B+Tree that maps `key -> IndexValueRef` (24 B, defined in `VLogFormat.hpp`).
- 4 KiB pages in a separate file, `index.db`. Every page has a header and a CRC32C.
- Operations: `put` (insert or overwrite), `get`, `erase`, `scan(start_key, end_key, limit)`.
- Leaf and internal node splits.
- One `std::shared_mutex` for the whole tree.
- Durability: all pages are flushed on clean close. After a crash, the index is rebuilt by
  replaying the full vLog with `should_apply()`.

Out of scope for Phase 2 (do not build):
- Pointer swizzling, lock-free structures, io_uring.
- Node merges or rebalancing after erase. Underfull and empty pages are allowed (see §5).
- Checkpoints and the `Superblock` (Phase 3).
- vLog GC (so no `kFlagRelocated` entries are produced yet, but replay must still honor them).

Unchanged: `VLogFormat.hpp` (on-disk vLog format), all Phase 1 files except where a bug fix
is needed, and all legacy files (`SlottedPage`, `BufferPoolManager`, `DocumentStore`,
`PrimaryIndex`, `MongoLite`, `WALManager`). Phase 2 does not reuse the legacy buffer pool.

Planned new files:

| File | Contents |
|---|---|
| `BPTreeFormat.hpp` | Packed on-disk structs, constants, `static_assert`s, key compare |
| `Pager.hpp` | Fixed-size page cache over `index.db`: pin/unpin, load with CRC check, write-back |
| `BPTree.hpp` | Tree operations, split logic, invariant checker (test-only entry point) |
| `IndexedStore.hpp` | Glue: vLog append + index apply, open/close, crash detection, rebuild |
| `test_bptree.cpp` | All Phase 2 tests (§3) |

---

## 1. On-Disk Page Layout

### 1.1 Constants

| Name | Value | Notes |
|---|---|---|
| `kPageSize` | 4096 | Every page, including the meta page |
| `kMaxKeyLen` | 256 | Keys longer than this are rejected with an error, never truncated |
| `kMinKeyLen` | 1 | Proposed: reject the empty key (open question Q3) |
| `kIndexFormatVersion` | 1 | |
| `kPageMagic` | `0x50544250` | Bytes `"PBTP"` (little-endian) |
| `kMetaMagic` | `0x3141544D58444E49` | Bytes `"INDXMTA1"` (little-endian) |
| `page_id_t` | `uint32_t` | Page id = file offset / 4096. Max file size 16 TiB (open question Q5) |
| `kMetaPageId` | 0 | Page 0 is always the meta page |
| `kNoPage` | 0 | Because page 0 is the meta page, 0 is never a valid child/sibling |

All integers are little-endian. Structs are copied out of page bytes with `std::memcpy`, never
`reinterpret_cast`, matching the rule in `VLogFormat.hpp`.

### 1.2 Common page header (every page, offset 0)

```cpp
enum class PageType : uint8_t { Meta = 1, Leaf = 2, Internal = 3 };

#pragma pack(push, 1)
struct PageHeader {            // 32 bytes at offset 0 of every page
    uint32_t crc32c;           // CRC32C over bytes [4, kPageSize)
    uint32_t magic;            // kPageMagic
    uint32_t page_id;          // must equal (file offset / kPageSize); catches misdirected I/O
    uint8_t  page_type;        // PageType
    uint8_t  level;            // 0 = leaf; parent = child level + 1
    uint16_t slot_count;       // number of entries
    uint16_t free_start;       // first byte after the slot array = 32 + 2 * slot_count
    uint16_t cell_start;       // first byte of the cell area (cells grow down from kPageSize)
    uint16_t frag_bytes;       // dead bytes inside the cell area (from erase)
    uint16_t reserved0;        // must be 0
    uint32_t right_sibling;    // leaf only: next leaf in key order, kNoPage if last; else 0
    uint32_t leftmost_child;   // internal only: child for keys < first separator; else 0
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == 32);
static_assert(offsetof(PageHeader, page_id)        == 8);
static_assert(offsetof(PageHeader, slot_count)     == 14);
static_assert(offsetof(PageHeader, right_sibling)  == 24);
static_assert(offsetof(PageHeader, leftmost_child) == 28);
```

### 1.3 Leaf and internal pages (slotted)

```
offset 0                32                free_start       cell_start            4096
       | PageHeader     | slot[0..n-1]    |   free space   | cells (grow down)   |
                          uint16 each
```

- `slot[i]` is a `uint16_t` offset of cell `i` inside the page. Slots are kept sorted by key.
- Cells are self-describing (key length is in the cell), so a slot holds only the offset.
- Bytes in `[free_start, cell_start)` are zero whenever a page is written (keeps CRCs deterministic).

```cpp
#pragma pack(push, 1)
struct CellKeyHeader {         // first 2 bytes of every leaf and internal cell
    uint16_t key_len;          // kMinKeyLen..kMaxKeyLen
};
#pragma pack(pop)
static_assert(sizeof(CellKeyHeader) == 2);
static_assert(sizeof(vlog::IndexValueRef) == 24);   // already asserted in VLogFormat.hpp
static_assert(sizeof(page_id_t) == 4);
```

| Cell | Bytes | Size |
|---|---|---|
| Leaf cell | `[uint16 key_len][key bytes][IndexValueRef 24 B]` | `2 + key_len + 24` (27..282) |
| Internal cell | `[uint16 key_len][key bytes][page_id_t child 4 B]` | `2 + key_len + 4` (7..262) |

`IndexValueRef` and `child` are unaligned inside the cell and are always read/written with `memcpy`.

Capacity (usable bytes = 4096 - 32 = 4064; each entry also costs a 2-byte slot):

| Page | Largest entry (cell + slot) | Entries per page at max key size | at 16-byte keys |
|---|---|---|---|
| Leaf | 284 B | 14 | 92 |
| Internal | 264 B | 15 | 169 (+ leftmost child) |

`static_assert`s to add: `kPageSize - sizeof(PageHeader) >= 3 * (2 + 2 + kMaxKeyLen + 24)` and the
same for internal cells. Three max-size entries per page are required so that a split always
produces two non-empty halves and the new entry always fits (see §2.4).

Internal node semantics: with separators `K1 < K2 < ... < Kn` and children `C0 = leftmost_child, C1..Cn`,
child `Ci` holds keys in `[Ki, Ki+1)`; `C0` holds keys `< K1`.

### 1.4 Meta page (page 0)

Page 0 uses `PageHeader` (`page_type = Meta`, `slot_count = 0`, siblings/children 0) followed by:

```cpp
#pragma pack(push, 1)
struct MetaBody {              // at offset 32 of page 0; rest of the page is zero
    uint64_t magic;            // kMetaMagic
    uint32_t format_version;   // kIndexFormatVersion
    uint32_t page_size;        // must equal kPageSize
    uint32_t root_page_id;     // never kNoPage once initialized
    uint32_t page_count;       // pages in the file including page 0; also the next new page id
    uint32_t tree_height;      // 1 = root is a leaf
    uint32_t clean_shutdown;   // 1 only after a complete clean close; set to 0 at open
    uint64_t key_count;        // live keys
    uint64_t max_applied_seq;  // highest vLog seq reflected in this file (trusted only if clean)
    uint64_t open_count;       // incremented on every open (debug aid)
};
#pragma pack(pop)
static_assert(sizeof(MetaBody) == 56);
static_assert(offsetof(MetaBody, clean_shutdown)  == 28);
static_assert(offsetof(MetaBody, max_applied_seq) == 40);
static_assert(sizeof(PageHeader) + sizeof(MetaBody) <= kPageSize);
```

A new empty tree is: page 0 (meta) + page 1 (empty leaf, root), `page_count = 2`, `tree_height = 1`.

### 1.5 Key ordering

Keys are byte strings compared with `memcmp` over the shorter length, then shorter-first.
This matches `std::string::compare` (`std::char_traits<char>::compare` compares as
`unsigned char`), so `std::map<std::string, ...>` is a valid oracle.

### 1.6 Page cache (Pager)

- Fixed array of N frames (default 1024 = 4 MiB; tests use 32 to force eviction).
- Frame table and LRU list protected by a `std::mutex` inside the Pager (needed because readers
  holding the shared tree lock can still fault pages in).
- `PageGuard` RAII pin: a pinned frame is never evicted. If every frame is pinned, CHECK-fail.
  Worst case pinned during a put: root-to-leaf path + one new page per level + one new root,
  i.e. `2 * height + 1`. The minimum allowed N is 64.
- Load: `pread` exactly 4096 bytes, then verify CRC, magic, `page_id`, and `page_type`.
  Any failure aborts the process (see §1.7 for why that is recoverable).
- Write-back (eviction or flush): recompute CRC, `pwrite` exactly 4096 bytes, check the return
  value. No fsync on eviction: mid-run page writes need no ordering because the file is not
  trusted after a crash.
- New page: `page_id = meta.page_count++`, zeroed frame marked dirty, not read from disk. Reading a
  page id `>= page_count` is a CHECK failure.

### 1.7 Open / close / crash detection

Clean close (exclusive lock held):
1. Write back every dirty frame (`pwrite`, return value checked).
2. `F_FULLFSYNC` `index.db`. On failure: abort, never retry. `clean_shutdown` stays 0, so the
   next open rebuilds.
3. Write the meta page with `clean_shutdown = 1`, `max_applied_seq`, `key_count`, `page_count`.
4. `F_FULLFSYNC` again. On failure: abort.

Open:
1. Open the vLog first (Phase 1 recovery truncates any torn tail).
2. If `index.db` is missing, or the meta page fails CRC/magic/version/page-size checks, or
   `clean_shutdown != 1`, or file size != `page_count * 4096` → **rebuild** (below).
3. Otherwise, write the meta page with `clean_shutdown = 0` and `open_count + 1`, then
   `F_FULLFSYNC`, **before any page is modified**. A crash at any later point is then detected
   on the next open.
4. Replay vLog entries with `seq > max_applied_seq` through `should_apply()` (normally none;
   this covers vLog writes that landed after the last index close).

A page CRC failure after a clean open aborts the process. Because step 3 already set
`clean_shutdown = 0`, the next open rebuilds. Corruption in `index.db` is therefore recoverable
from the vLog.

Rebuild:
1. Delete any leftover `index.db.rebuild`. Create it fresh (`O_CREAT | O_EXCL`).
2. Scan all vLog segments in order (Phase 1 `VLogReader::scanSegment`). For each entry, call
   `apply()` (§4) on the new tree.
3. Clean-close the new tree (steps 1–4 above).
4. `rename("index.db.rebuild", "index.db")`, then fsync the directory. Check every return value.
5. Open normally.
A crash during rebuild leaves the old `index.db` (still dirty) or no `index.db`, so the next
open rebuilds again.

---

## 2. Algorithms

### 2.1 get(key) — shared lock
1. Reject a key outside `[kMinKeyLen, kMaxKeyLen]`.
2. From the root, at each internal page binary-search for the largest `i` with `Ki <= key`;
   follow `Ci` (or `C0` if none). Pin child before unpinning parent.
3. In the leaf, binary-search. Return the `IndexValueRef` or "not found".

### 2.2 put(key, ref) — exclusive lock
1. Reject a bad key length.
2. Descend as in get, recording the path `[(page_id, child_index)]` and keeping each page pinned.
3. Key exists in the leaf: overwrite the 24-byte `IndexValueRef` in place. The cell size does not
   change, so an overwrite never splits. Mark dirty. Done.
4. Key is new. `need = 2 + (2 + key_len + 24)`.
   - If `cell_start - free_start >= need`: write the cell at `cell_start - cell_size`, shift
     slots right of the insert position by one, update header. Done.
   - Else if `cell_start - free_start + frag_bytes >= need`: compact the page (rewrite live cells
     contiguously from the end, `frag_bytes = 0`), then insert. Done.
   - Else: split (§2.4).
5. `key_count++`, `max_applied_seq = max(max_applied_seq, ref.seq)` (tracked in memory, written at close).

### 2.3 erase(key) — exclusive lock
1. Descend to the leaf. If the key is absent, return false.
2. Remove the slot (shift later slots left), add the cell size to `frag_bytes`. Do not touch
   parents or siblings. `key_count--`. Return true.
3. No merge, no borrow. A leaf may become empty; it stays in the tree and in the sibling chain.
   Separators in parents stay valid as routing bounds.

### 2.4 Split, step by step

Leaf split (leaf `L` has no room for the new entry even after compaction):
1. Copy all live entries of `L` plus the new entry into an in-memory sorted vector `E[0..n)`
   (at most ~150 entries).
2. Pick the split index `s` by bytes: walk `E` accumulating `cell_size + 2`; `s` is the first index
   where the running total reaches half of the grand total. Clamp so `1 <= s <= n-1`.
   Each half then holds at most `4064/2 + 284` bytes, which fits in a page.
3. Allocate a new leaf `R` (`page_id = page_count++`, pinned, dirty).
4. Rewrite `L` from scratch with `E[0..s)` (compacted, `frag_bytes = 0`). Write `R` with `E[s..n)`.
5. Sibling chain: `R.right_sibling = L.right_sibling`; `L.right_sibling = R`.
6. Separator `S = first key of R` (full key, no prefix truncation in Phase 2).
7. Insert `(S, R)` into the parent at slot `child_index(L) + 1` (from the recorded path).
8. If `L` was the root: allocate internal page `N` with `level = 1`, `leftmost_child = L`, one cell
   `(S, R)`. Set `meta.root_page_id = N`, `tree_height++`.
9. If the parent has no room even after compaction: internal split (below), then continue up the path.

Internal split (internal page `P` cannot fit the new `(S, R)` entry):
1. Build the sorted vector of `P`'s entries plus the new one: `(K1,C1) .. (Kn,Cn)`, with `C0` separate.
2. Pick the middle index `m` by bytes, same rule as step 2 above, clamped to `2 <= m <= n-1` so
   both `P` and `P'` keep at least one separator (n >= 3 is guaranteed by the capacity assert).
3. Allocate a new internal page `P'` (same `level` as `P`).
4. `P` keeps `C0` and `(K1,C1) .. (Km-1,Cm-1)`.
5. `P'.leftmost_child = Cm`; `P'` gets `(Km+1,Cm+1) .. (Kn,Cn)`.
6. `Km` moves up: insert `(Km, P')` into `P`'s parent. It is not kept in `P` or `P'`.
7. If `P` was the root, create a new root as in leaf step 8 with `level = P.level + 1`.

Notes:
- All split steps happen in memory under the exclusive lock. No write ordering is needed:
  after a crash the index is rebuilt from the vLog, so a half-written split on disk is never read.
- Freshly allocated pages are written to disk only by eviction or close.

### 2.5 scan(start_key, end_key, limit) — shared lock
Proposed semantics (open question Q4): returns entries with `start_key <= key < end_key` in
ascending order, at most `limit` entries. `start_key >= end_key` or `limit == 0` returns empty.
1. Descend to the leaf that would contain `start_key`; binary-search to the first key `>= start_key`.
2. Emit entries until `key >= end_key` or `limit` is reached.
3. At the end of a leaf, pin `right_sibling` before unpinning the current leaf. Skip empty leaves.
4. Return a `std::vector<std::pair<std::string, IndexValueRef>>`.
The shared lock is held for the whole scan, so no split can run during it.

---

## 3. Tests (`test_vlog_crash.cpp` safety rules apply to every test)

Common rules:
- Each test uses a `mkdtemp` directory, deletes it on exit, and prints the path on any abort
  (same SIGABRT handler as Phase 1).
- A global atomic byte counter covers vLog writes, `index.db` page writes, and test-side writes.
  The run aborts if the total passes 512 MiB.
- Abort if free disk space is below 10 GB or `statvfs` fails.
- Every syscall return value is checked. A failed fsync aborts (never retried).
- Fixed, printed RNG seeds. A failing seed can be replayed.
- `checkInvariants()` (test-only) walks the whole tree and CHECKs:
  every page's CRC/magic/page_id after a flush; keys sorted within each page;
  every key in child `Ci` lies in `[Ki, Ki+1)`; all leaves at level 0 and the same depth;
  the leaf chain visits every leaf left to right and yields all keys in order;
  `32 + 2*slot_count == free_start <= cell_start`; cells lie in `[cell_start, 4096)` and
  do not overlap; sum of live cell bytes + `frag_bytes == 4096 - cell_start`;
  `key_count` equals the number of leaf entries.

### T1. Randomized oracle test (100,000 ops)
- Oracle: `std::map<std::string, IndexValueRef>`.
- Pager with 32 frames, so eviction and reload (with CRC checks) happen constantly.
- Key space: 20,000 distinct keys so overwrites, erases of present keys, and erases of absent keys
  all occur. Key lengths: mostly 1–32 bytes, 10% exactly 256, 10% in 200–255, some sharing long
  prefixes (to stress separators). Bytes include 0x00 and 0xFF.
- Op mix: 40% put, 25% get, 20% erase, 15% scan (random start, random end, random limit 1–500).
- `seq` in each `IndexValueRef` increases per op; `vptr` fields are random.
- Every op's result is compared with the oracle immediately (get value, erase return, scan output).
- Every 10,000 ops: full scan == oracle, plus `checkInvariants()`.
- Run with 3 seeds.
- Separate check: `put` with a 257-byte key fails, a 256-byte key succeeds, an empty key fails
  (if Q3 is accepted); a rejected put leaves the tree unchanged.

### T2. Range scans across page boundaries
- Insert keys of fixed size so each leaf holds a known number of entries. Record leaf boundaries
  through the test-only tree walker.
- Scans that: start at the last key of a leaf; end exactly at the first key of the next leaf;
  have `limit` ending exactly on a boundary; span 3+ leaves; cover the whole tree.
- Erase every key in one middle leaf (leaving it empty), then scan across it.
- Erase everything, scan the empty tree.
- Edge cases: `start == end`, `start > end`, `limit == 1`, `start` below the smallest key,
  `end` above the largest key, `start` between two existing keys.
- Each result compared with the oracle's `lower_bound(start)` .. `lower_bound(end)`.

### T3. Close + reopen
- Run a T1-style workload (50,000 ops), clean close, reopen (also with a different cache size),
  then full scan == oracle and `checkInvariants()`. Continue with 10,000 more ops and repeat.
- Check meta: `clean_shutdown == 1` on disk after close, `== 0` on disk right after open.
- Corruption: after a clean close, flip one byte in a leaf page. Reopen in a forked child and touch
  that page: the child must abort on the CRC check. Reopen in the parent: `clean_shutdown == 0`
  triggers rebuild, and the rebuilt index equals the oracle built from the vLog.
- Corrupt the meta page: open must rebuild.

### T4. kill -9 during writes, then rebuild from the vLog
- Child process: `IndexedStore` with 4 writer threads doing put/erase on a small key space
  (so keys are overwritten and deleted). After each op returns (vLog fsynced and index applied),
  the child writes `(seq, op, key id)` to a pipe.
- Parent: waits for at least 200 acks, sleeps a random 5–400 ms, sends SIGKILL, `waitpid`, drains the pipe.
- Parent opens the store: must detect `clean_shutdown == 0` and rebuild.
- Checks:
  1. Reference state: replay the recovered vLog into a `std::map` with `should_apply()`. The rebuilt
     index must equal it exactly (full scan).
  2. Every acked write is present: for every acked put `(seq s, key k)`, the index has `k` with
     `seq >= s`, or the vLog contains a tombstone for `k` with seq `> s` and `k` is absent.
     For every acked erase `(seq s, key k)`: `k` is absent, or the index has `k` with `seq > s`.
  3. `checkInvariants()` passes.
- 50 iterations.
- Additional crash points (each iteration picks one): during rebuild itself (kill the rebuilding
  child, then rebuild again); during clean close (between page flush and meta write).

### T5. Concurrency
- 4 writer threads (put/erase) + 4 reader threads (get/scan) for a fixed op count.
- Readers check: scan output is strictly ascending and within `[start, end)`; for each key, the
  `seq` a reader sees never goes backwards across its own successive gets.
- After join: full scan == oracle built from the writers' final applied ops; `checkInvariants()`.

### T6. Build matrix (all tests, every build)
```
clang++ -std=c++17 -O3 -Wall -Wextra -Werror -pthread test_bptree.cpp -o bpt && ./bpt
g++-XX  -std=c++17 -O3 -Wall -Wextra -Werror -pthread test_bptree.cpp -o bpt_gcc && ./bpt_gcc
clang++ -std=c++17 -g -fsanitize=address,undefined -pthread test_bptree.cpp -o bpt_asan && ./bpt_asan
clang++ -std=c++17 -g -fsanitize=thread -DTSAN_BUILD -pthread test_bptree.cpp -o bpt_tsan && ./bpt_tsan
```
- On this machine `g++` is Apple clang. A real GCC needs `brew install gcc` (gives `g++-XX`).
  Until then, the GCC row cannot be run, and reports must say so.
- Under TSan, the fork/kill test (T4) is skipped as in Phase 1; T5 is the TSan target.
- Report: exact output of each command, plus measured values only (ops count, bytes written,
  rebuild time in ms, tree height, page count). No performance claims beyond those numbers.

---

## 4. Integration with the vLog (`IndexedStore`)

```
put(key, value):   validate key length  ->  vlog.append(Full)  ->  apply(key, Full, seq, flags, vptr)
erase(key):        validate key length  ->  vlog.append(Tombstone)  ->  apply(key, Tombstone, seq, flags, vptr)
get / scan:        index only (reading the value from the vLog is outside the index)
```

- Key length is validated **before** the vLog append, so the vLog never holds a key the index
  cannot store. During replay, an oversize key is a CHECK failure.
- `apply()` is one tree method under one exclusive lock acquisition:
  `has_key, idx_seq = lookup(key)`; if `should_apply(has_key, idx_seq, seq, flags)` then
  Full/Delta → `put(key, {seq, kind=1, vptr})`, Tombstone → `erase(key)`.
- Live writes must reach `apply()` in seq order. Without that, this race loses data:
  thread A appends put(k) with seq 9, thread B appends tombstone(k) with seq 10, B applies first
  (k erased, its seq is gone), then A applies: `has_key == false`, so seq 9 is inserted and a
  deleted key comes back. Proposed fix: a sequencer in `IndexedStore`. After `append()` returns
  seq `s`, the thread waits until `next_apply_seq == s`, applies, sets `next_apply_seq = s + 1`,
  and notifies. Group commit acks a whole batch at once, so batch members apply back to back.
  Every assigned seq is either acked or the writer aborts, so the sequence has no holes.
  (Alternative in Q1.)
- Full replay after a crash goes through the same `apply()` in file order. vLog entries are in
  strictly increasing seq order in Phase 2 (Phase 1 Stage 5 asserts this), so erasing on a
  tombstone during replay is safe.
- `IndexValueRef.kind` is always 1 (vLog) in Phase 2 (Q2).

---

## 5. Documented Limitations

- **No merges.** After erases, leaves can be underfull or empty. Empty leaves stay in the tree and
  the sibling chain; scans skip them. Space is not reclaimed; the file never shrinks. A rebuild
  (after a crash) produces a compact tree. An offline compaction is left for a later phase.
- **No page reuse.** Pages are only appended (`page_count++`); there is no free list.
- **Full vLog replay after a crash.** Recovery time grows with total vLog size until Phase 3 checkpoints.
- **One tree-wide lock.** Writers block all readers; a long scan blocks writers. Page misses are
  serialized by the Pager mutex.
- **Full-length separators.** Up to 256 bytes each, which lowers internal fanout with long keys.

---

## 6. Open Questions and Risks

| # | Question / risk | Proposal |
|---|---|---|
| Q1 | Live apply ordering (tombstone vs delayed put race, §4). | Sequencer (apply in seq order). Alternative: keep tombstones in the index as `kind = 2` entries with their seq. That avoids waiting but changes the meaning of `IndexValueRef.kind` and needs tombstone cleanup later. **Needs your decision.** |
| Q2 | `IndexValueRef` has `kind = 0` (inline) but no room for inline data. | Phase 2 always uses `kind = 1`. Decide inline values in Phase 3 or drop `kind = 0`. |
| Q3 | Is the empty key valid? | Reject it (`kMinKeyLen = 1`). |
| Q4 | Scan end bound: exclusive or inclusive? How to express "no upper bound"? | `[start, end)`; add `scan_from(start, limit)` for unbounded. |
| Q5 | `page_id_t` is 32-bit; `Superblock::index_root_page` is 64-bit. | Keep 32-bit in pages (16 TiB max file); widen when writing the Superblock in Phase 3. |
| R1 | Rebuild memory: `VLogReader::scanSegment` returns every entry of a segment with payloads (up to 64 MB per segment in memory). | Accept for Phase 2; measure peak RSS in T4. A streaming scan API can come later without format changes. |
| R2 | Open already scans every vLog segment (`VLogWriter::recoverAndInitLocked`); rebuild scans them again. | Accept double scan in Phase 2; measure. Sharing one scan needs a `VLogWriter` API change (not a format change). |
| R3 | Directory fsync on macOS uses `fsync()`, not `F_FULLFSYNC` (Phase 1 `sync_directory`). The rebuild `rename` relies on it. | Note as a risk. If the rename is lost, the next open sees the old dirty `index.db` and rebuilds again, so it costs time, not correctness. |
| R4 | A torn meta-page write on clean close. | Meta CRC fails → treated as dirty → rebuild. No second meta copy needed. |
| R5 | Abort on page CRC failure mid-run takes the whole process down. | Intended: the next open rebuilds. Document in the API. |
| R6 | `kFlagRelocated` entries do not exist yet (no GC). | Replay still calls `should_apply()` with the entry flags; add a unit test with a hand-built relocated entry. |
| R7 | Deltas: the index stores the newest version (Full or Delta). Reading a value means walking the delta chain. | Outside the index; unchanged by Phase 2. |
| R8 | Whole-page CRC on every load and every write-back costs CPU. | Measure in T1 (count of loads/writes, wall time). No claims until measured. |
| R9 | 512 MiB budget: T1 + T3 + T4 write `index.db` pages repeatedly (evictions + closes). | Count page write-backs in the global counter; size workloads so the measured total stays well under the limit, and print it. |
| R10 | No real GCC on this machine. | `brew install gcc`, or accept clang-only results and say so in reports. |
| R11 | Phase 1 observation: with 4 writer threads, group commit averages 2.00 writes per fsync (16 threads: 7.98; 64 threads: 31.9), i.e. about half the threads per batch. | Not a Phase 2 blocker. Investigate separately if group commit throughput matters. |
