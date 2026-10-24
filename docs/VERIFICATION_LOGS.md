# Empirical Production Verification & Benchmark Logs

This document records the exact, untruncated terminal output from the comprehensive verification runs on the storage engine, including ThreadSanitizer (`make tsan`), AddressSanitizer with 50-iteration `kill -9` process crashes (`make asan`), and the write-amplification benchmark (`make bench`).

**Environment**:
- **Hardware**: Apple M5 (10-core ARM64), 17 GB RAM, Internal APFS NVMe SSD
- **Operating System**: macOS 27.0
- **Compiler**: Apple LLVM Clang 21.0.0 (`clang-2100.3.34.2`) with `-std=c++17 -Wall -Wextra -Werror -pthread`
- **Verification Date**: October 2026

---

## 1. ThreadSanitizer Verification (`make tsan`)

*Target: Data-Race Freedom under heavy concurrent readers, writers, and live background flushers across 1024 stripe locks.*

```text
praveen@Praveens-MacBook-Air LSM % cd /Users/praveen/Code/LSM
praveen@Praveens-MacBook-Air LSM % make tsan
check-deps: OK (vlog/ and doc/ have no upward includes)

=== [tsan] test_bptree ===
[PASS] B1.1 page CRC32C rejects every single-bit flip (32768 bits)
[PASS] B1.2 pager eviction, checkpoint, reopen from the superblock
[PASS] B1.3 bad page aborts the reader; TreeCorrupt, BadHeader, SizeMismatch, NoSuperblock, Missing detected
[PASS] B2.1 single leaf: put/get/erase/scan edge cases and key length limits
[PASS] B2.2 single leaf: 20k random ops vs std::map
[PASS] B2.3 leaf capacity: 14 max-size entries, compaction, the 15th splits
[PASS] B3.1 random ops vs std::map (seed 31, 100000 ops, 32 frames, height 3)
[PASS] B3.1 random ops vs std::map (seed 32, 100000 ops, 1024 frames, height 3)
[PASS] B3.1 random ops vs std::map (seed 33, 100000 ops, 1024 frames, height 3)
[PASS] B3.2 ascending and descending bulk inserts
[PASS] B3.3 range scans across page boundaries (462 scans)
[PASS] B3.4 keys > 256 bytes rejected on a multi-level tree
[PASS] B4.1 checkpoint + reopen with 1024/64/16 frames equals std::map (50000 ops)
[PASS] B4.2 crash, corruption, missing files: reopened or rebuilt index equals a vLog replay
[PASS] B5.1 IndexedStore put/get/erase through vLog + index
[SKIP] B5.2 kill -9 during writes, recovery, clean close (fork + kill -9 not run under ThreadSanitizer)
[PASS] B6.1 8 threads, mixed ops on IndexedStore: final index == vLog replay
[PASS] B6.2 4 writers + 4 readers on a 24-frame cache
[Budget] bytes written: 259206750 of 536870912
=== [tsan] test_bptree: exit 0 ===

=== [tsan] test_checkpoint ===
[PASS] CP1 checkpoint records the last applied seq and the vLog position after it
[PASS] CP2 bytes trigger (3 checkpoints for 253616 vLog bytes)
[PASS] CP3 timer trigger: checkpoints while entries arrive, none while idle
[PASS] CP4 checkpoints during 4 writers + 2 readers; crash recovers from the last superblock (379 checkpoints, max stall 31 ms)
[PASS] CP5 recovery reads only the vLog tail after the replay point (200 of 6000 entries)
[PASS] CP6 index.db without superblock.db (incl. format v2) -> full rebuild
[PASS] CP7 committed tree page with a bad CRC -> full rebuild
[SKIP] CP8 entries appended during a checkpoint are replayed after a crash (fork + kill -9 not run under ThreadSanitizer)
[PASS] CP9 page reuse never overwrites pages of the trees named by both slots (7 checkpoints, 29 pages reused)
[PASS] CP10 newest superblock slot corrupt -> recovery from the older slot
[SKIP] CP11 kill -9 at random points (fork + kill -9 not run under ThreadSanitizer)
[Budget] bytes written: 16045426 of 536870912
=== [tsan] test_checkpoint: exit 0 ===

=== [tsan] test_cow ===
[PASS] C1 committed tree untouched by later changes (197 pages byte-identical, 197 copies)
[PASS] C2 first change after a checkpoint copies the root-to-leaf path once (height 3)
[PASS] C3 page reuse over 6 checkpoints never overwrites the last 2 committed trees (408 pages reused)
[PASS] C4 free list rebuilt at open: no leaked pages; previous tree reserved until the next commit
[Budget] bytes written: 9625600 of 536870912
=== [tsan] test_cow: exit 0 ===

=== [tsan] test_delta ===
[PASS] P4.C6 no-op update writes nothing, seq unchanged (5 no-ops)
[PASS] P4.C8 NotFound on missing and erased keys (nothing written)
[PASS] P4.C9 TypeError on raw bytes, JSON array, number (9 stored values)
[PASS] P4.C10 every error case: vLog and index files identical before and after (17 error cases)
[PASS] P4.D1 depth 1..cap: correct doc, vlog_reads = depth + 1 (cap 4)
[PASS] P4.D2 update after cap deltas -> Full with kFlagConsolidated; depth resets
[PASS] P4.D3 delta > 50% of doc -> Full
[PASS] P4.D4 chain spans segments (5 entries in 5 segments)
[PASS] P4.D5 small update on a doc near the max entry size (16777176-byte limit)
[PASS] P4.D6 caps 0/1/2/3/4 (4 = max) give identical docs (cap 0: 0 deltas, cap 1: 44 deltas, cap 2: 57 deltas, cap 3: 65 deltas, cap 4: 69 deltas)
[PASS] P4.D7 a Full entry from put() is returned byte-for-byte by get() (6 values)
[PASS] P4.E1 put -> update -> update -> get
[PASS] P4.E2 erase -> update = NotFound; erase -> put -> update uses the new put as base
[PASS] P4.E3 put raw bytes -> update = TypeError -> put JSON -> update ok
[PASS] P4.F1 8 threads x 1000 $inc on one key -> exactly 8000 (6400 deltas, 1600 consolidated)
[PASS] P4.F2 concurrent update + put on one key: final state == seq-order vLog replay
[PASS] P4.F3 concurrent update + erase on one key: no update applies to an erased base (471 updates, 129 NotFound)
[PASS] P4.F4 parallel updates on many keys + concurrent gets: no partial document returned (36925 concurrent gets)
[PASS] P4.F5 checkpoints during updates: no deadlock, correct final state (31 checkpoints during 800 updates)
[SKIP] P4.G1 kill -9 during $inc stream (fork + kill -9 not run under ThreadSanitizer)
[SKIP] P4.G2 kill -9 during random updates (fork + kill -9 not run under ThreadSanitizer)
[PASS] P4.G3 delta appended but not applied at crash time -> replay applies it (3 entries replayed)
[PASS] P4.G4 checkpoint mid-chain (base before replay point, deltas after); crash; get() correct
[PASS] P4.G5 recover twice + full rebuild -> identical state (6 deltas replayed each time)
[PASS] P4.G6 base_seq mismatch on replay -> VLogCorruptError on open; nothing modified (3 cases (wrong base, absent key, rebuild path), 2 opens each)
[PASS] P4.G7 corrupt base entry -> get() error, never a wrong document (corrupt Full base and corrupt middle Delta)
[PASS] P4.G8 missing segment holding a chain's base -> get() error
[Budget] bytes written: 19794392 of 536870912
=== [tsan] test_delta: exit 0 ===

=== [tsan] test_doc ===
[PASS] P4.A1 round trip keeps tree and key order
[PASS] P4.A2 all types, empty {} and []
[PASS] P4.A3 escapes incl. surrogate pairs; bad escapes and lone/reversed surrogates rejected (14 rejected)
[PASS] P4.A4 duplicate keys, NaN/Inf, trailing commas, comments, leading zeros, unquoted keys, trailing garbage, control chars, invalid UTF-8 rejected (50 cases)
[PASS] P4.A5 numbers: int64 min/max ok, beyond rejected; -0.0, 1e308, 5e-324 exact
[PASS] P4.A6 depth 64 ok, 65 rejected (objects and arrays)
[PASS] P4.A7 max size ok, +1 byte rejected (16777176 bytes)
[PASS] P4.A8 multi-byte UTF-8 keys/values round-trip
[PASS] P4.B1 valid paths: "a", "a.b.c", "items.0", Unicode keys
[PASS] P4.B2 invalid paths rejected ("", ".", "a.", ".a", "a..b", "items.-1", "items.01", "items.1e2") (15 cases)
[PASS] P4.B3 array index out of range -> RangeError
[PASS] P4.B4 64 path segments ok, 65 rejected
[PASS] P4.C1 $set: new, overwrite, type change, nested auto-create
[PASS] P4.C2 subfield on a primitive -> TypeError ($unset: no-op)
[PASS] P4.C3 $unset: existing, missing (no change), nested; array element -> TypeError
[PASS] P4.C4 $inc: missing, int+int, int+double, double+double; non-number -> TypeError; INT64_MAX+1, INT64_MIN-1 -> RangeError
[PASS] P4.C5 conflicts: ("a","a.b"), ("a.b","a"), same path twice; ("a.b","a.bc") allowed
[PASS] P4.C7 one update with 1,000 ops
[Budget] bytes written: 0 of 536870912
=== [tsan] test_doc: exit 0 ===

=== [tsan] test_edge_cases ===
[PASS] E1 index ahead of the vLog: rebuild or fail loudly, never serve missing keys
[PASS] E2 index.db paired with a different vLog is detected and rebuilt
[PASS] E3 get() with a bad VPtr throws VLogReadError
[PASS] E4 0-byte doc works; docs over the 16 MiB entry limit rejected before writing
[PASS] E5 two threads put the same key: higher seq wins, live and after reopen
[PASS] E6 concurrent get + put on one key: old or new value, never a mix
[PASS] E7 scan edge cases: empty tree, start >= end, limit 0, 256-byte keys
[PASS] E8 200k sequential keys: height 4, no evictions, reopen
[PASS] E9 index.db pwrite/fsync failure aborts; reopen loses no acked write
[Budget] bytes written: 46416705 of 536870912
=== [tsan] test_edge_cases: exit 0 ===

=== [tsan] test_superblock ===
[PASS] SB1 slot CRCs reject every single-bit flip in the 96 checked bytes (768 flips; 5 out-of-range fields rejected)
[PASS] SB2 ping-pong: epochs alternate slots, read picks the highest valid epoch
[PASS] SB3 bit flip in slot 0, slot 1, both; torn write; short file: the valid slot is used, both bad -> Corrupt
[PASS] SB4 store: both slots invalid or superblock.db missing -> full rebuild (102 keys correct)
[Budget] bytes written: 235892 of 536870912
=== [tsan] test_superblock: exit 0 ===

=== [tsan] test_vlog_crash ===
[PASS] V1 CRC32C: ARMv8 and table versions give 0xe3069283 and agree on 100 random payloads
[PASS] V2 reader rejects total_len < 32, unaligned, > 16 MiB, past EOF
[PASS] V3 torn tail truncated in the last segment; fatal in a non-last segment; mid-log CRC and bounds corruption fatal
[PASS] V4 should_apply: older seqs skipped, tombstones erase; a second replay of 3 delta entries applies 0
[PASS] V5 reopen continues at seq 101 in segment 2; all 101 entries readable
[PASS] V6 torn segment header, entry header and payload: truncated when last, fatal when not last
[PASS] V7 redo over a checkpointed state applies seqs 6-10 only; a second pass applies 0
[PASS] V8 torn segment creation (3 shapes) is unlinked; two later restarts open normally
[SKIP] V9 kill -9 crash recovery (fork + kill -9 not run under ThreadSanitizer)
[PASS] V10 group commit benchmark (1t: 96 w/s, 4t: 191 w/s, 16t: 774 w/s, 64t: 2820 w/s)
[Budget] bytes written: 524313 of 536870912
=== [tsan] test_vlog_crash: exit 0 ===

=== [tsan] bytes written by all test binaries: 351849078 (limit 536870912) ===
```

---

## 2. AddressSanitizer & Process-Crash Verification (`make asan`)

*Target: Memory Safety (Zero Leaks, Zero Bounds Violations) + Durability under Randomized `kill -9` (`SIGKILL`) Process Crashes.*

```text
praveen@Praveens-MacBook-Air LSM % cd /Users/praveen/Code/LSM
praveen@Praveens-MacBook-Air LSM % make asan
check-deps: OK (vlog/ and doc/ have no upward includes)

=== [asan] test_bptree ===
[PASS] B1.1 page CRC32C rejects every single-bit flip (32768 bits)
[PASS] B1.2 pager eviction, checkpoint, reopen from the superblock
[PASS] B1.3 bad page aborts the reader; TreeCorrupt, BadHeader, SizeMismatch, NoSuperblock, Missing detected
[PASS] B2.1 single leaf: put/get/erase/scan edge cases and key length limits
[PASS] B2.2 single leaf: 20k random ops vs std::map
[PASS] B2.3 leaf capacity: 14 max-size entries, compaction, the 15th splits
[PASS] B3.1 random ops vs std::map (seed 31, 100000 ops, 32 frames, height 3)
[PASS] B3.1 random ops vs std::map (seed 32, 100000 ops, 1024 frames, height 3)
[PASS] B3.1 random ops vs std::map (seed 33, 100000 ops, 1024 frames, height 3)
[PASS] B3.2 ascending and descending bulk inserts
[PASS] B3.3 range scans across page boundaries (462 scans)
[PASS] B3.4 keys > 256 bytes rejected on a multi-level tree
[PASS] B4.1 checkpoint + reopen with 1024/64/16 frames equals std::map (50000 ops)
[PASS] B4.2 crash, corruption, missing files: reopened or rebuilt index equals a vLog replay
[PASS] B5.1 IndexedStore put/get/erase through vLog + index
[PASS] B5.2 kill -9 during writes, recovery, clean close (50 runs, 11906 acked ops, newest acked op reflected for 3083 keys, 124 recovered-but-unacked)
[PASS] B6.1 8 threads, mixed ops on IndexedStore: final index == vLog replay
[PASS] B6.2 4 writers + 4 readers on a 24-frame cache
[Budget] bytes written: 263705318 of 536870912
=== [asan] test_bptree: exit 0 ===

=== [asan] test_checkpoint ===
[PASS] CP1 checkpoint records the last applied seq and the vLog position after it
[PASS] CP2 bytes trigger (3 checkpoints for 253616 vLog bytes)
[PASS] CP3 timer trigger: checkpoints while entries arrive, none while idle
[PASS] CP4 checkpoints during 4 writers + 2 readers; crash recovers from the last superblock (397 checkpoints, max stall 56 ms)
[PASS] CP5 recovery reads only the vLog tail after the replay point (200 of 6000 entries)
[PASS] CP6 index.db without superblock.db (incl. format v2) -> full rebuild
[PASS] CP7 committed tree page with a bad CRC -> full rebuild
[PASS] CP8 entries appended during a checkpoint are replayed after a crash (8 entries)
[PASS] CP9 page reuse never overwrites pages of the trees named by both slots (7 checkpoints, 29 pages reused)
[PASS] CP10 newest superblock slot corrupt -> recovery from the older slot
[PASS] CP11 kill -9 x50 (writes, checkpoint flush, fsync->superblock, recovery) (7281 acked ops, newest acked op reflected for 5832 keys; recovery read 1522 of 5400 entries)
[Budget] bytes written: 21398922 of 536870912
=== [asan] test_checkpoint: exit 0 ===

=== [asan] test_cow ===
[PASS] C1 committed tree untouched by later changes (197 pages byte-identical, 197 copies)
[PASS] C2 first change after a checkpoint copies the root-to-leaf path once (height 3)
[PASS] C3 page reuse over 6 checkpoints never overwrites the last 2 committed trees (408 pages reused)
[PASS] C4 free list rebuilt at open: no leaked pages; previous tree reserved until the next commit
[Budget] bytes written: 9625600 of 536870912
=== [asan] test_cow: exit 0 ===

=== [asan] test_delta ===
[PASS] P4.C6 no-op update writes nothing, seq unchanged (5 no-ops)
[PASS] P4.C8 NotFound on missing and erased keys (nothing written)
[PASS] P4.C9 TypeError on raw bytes, JSON array, number (9 stored values)
[PASS] P4.C10 every error case: vLog and index files identical before and after (17 error cases)
[PASS] P4.D1 depth 1..cap: correct doc, vlog_reads = depth + 1 (cap 4)
[PASS] P4.D2 update after cap deltas -> Full with kFlagConsolidated; depth resets
[PASS] P4.D3 delta > 50% of doc -> Full
[PASS] P4.D4 chain spans segments (5 entries in 5 segments)
[PASS] P4.D5 small update on a doc near the max entry size (16777176-byte limit)
[PASS] P4.D6 caps 0/1/2/3/4 (4 = max) give identical docs (cap 0: 0 deltas, cap 1: 44 deltas, cap 2: 57 deltas, cap 3: 65 deltas, cap 4: 69 deltas)
[PASS] P4.D7 a Full entry from put() is returned byte-for-byte by get() (6 values)
[PASS] P4.E1 put -> update -> update -> get
[PASS] P4.E2 erase -> update = NotFound; erase -> put -> update uses the new put as base
[PASS] P4.E3 put raw bytes -> update = TypeError -> put JSON -> update ok
[PASS] P4.F1 8 threads x 1000 $inc on one key -> exactly 8000 (6400 deltas, 1600 consolidated)
[PASS] P4.F2 concurrent update + put on one key: final state == seq-order vLog replay
[PASS] P4.F3 concurrent update + erase on one key: no update applies to an erased base (440 updates, 160 NotFound)
[PASS] P4.F4 parallel updates on many keys + concurrent gets: no partial document returned (60718 concurrent gets)
[PASS] P4.F5 checkpoints during updates: no deadlock, correct final state (31 checkpoints during 800 updates)
[PASS] P4.G1 kill -9 during $inc stream: acked <= value <= acked + 1, value == $inc entries (20 runs, 1994 acked; 0 runs where the in-flight update landed)
[PASS] P4.G2 kill -9 during random updates on many keys: every acked update visible; index == full vLog replay (20 runs, 2238 acked updates)
[PASS] P4.G3 delta appended but not applied at crash time -> replay applies it (3 entries replayed)
[PASS] P4.G4 checkpoint mid-chain (base before replay point, deltas after); crash; get() correct
[PASS] P4.G5 recover twice + full rebuild -> identical state (6 deltas replayed each time)
[PASS] P4.G6 base_seq mismatch on replay -> VLogCorruptError on open; nothing modified (3 cases (wrong base, absent key, rebuild path), 2 opens each)
[PASS] P4.G7 corrupt base entry -> get() error, never a wrong document (corrupt Full base and corrupt middle Delta)
[PASS] P4.G8 missing segment holding a chain's base -> get() error
[Budget] bytes written: 23213136 of 536870912
=== [asan] test_delta: exit 0 ===

=== [asan] test_doc ===
[PASS] P4.A1 round trip keeps tree and key order
[PASS] P4.A2 all types, empty {} and []
[PASS] P4.A3 escapes incl. surrogate pairs; bad escapes and lone/reversed surrogates rejected (14 rejected)
[PASS] P4.A4 duplicate keys, NaN/Inf, trailing commas, comments, leading zeros, unquoted keys, trailing garbage, control chars, invalid UTF-8 rejected (50 cases)
[PASS] P4.A5 numbers: int64 min/max ok, beyond rejected; -0.0, 1e308, 5e-324 exact
[PASS] P4.A6 depth 64 ok, 65 rejected (objects and arrays)
[PASS] P4.A7 max size ok, +1 byte rejected (16777176 bytes)
[PASS] P4.A8 multi-byte UTF-8 keys/values round-trip
[PASS] P4.B1 valid paths: "a", "a.b.c", "items.0", Unicode keys
[PASS] P4.B2 invalid paths rejected ("", ".", "a.", ".a", "a..b", "items.-1", "items.01", "items.1e2") (15 cases)
[PASS] P4.B3 array index out of range -> RangeError
[PASS] P4.B4 64 path segments ok, 65 rejected
[PASS] P4.C1 $set: new, overwrite, type change, nested auto-create
[PASS] P4.C2 subfield on a primitive -> TypeError ($unset: no-op)
[PASS] P4.C3 $unset: existing, missing (no change), nested; array element -> TypeError
[PASS] P4.C4 $inc: missing, int+int, int+double, double+double; non-number -> TypeError; INT64_MAX+1, INT64_MIN-1 -> RangeError
[PASS] P4.C5 conflicts: ("a","a.b"), ("a.b","a"), same path twice; ("a.b","a.bc") allowed
[PASS] P4.C7 one update with 1,000 ops
[Budget] bytes written: 0 of 536870912
=== [asan] test_doc: exit 0 ===

=== [asan] test_edge_cases ===
[PASS] E1 index ahead of the vLog: rebuild or fail loudly, never serve missing keys
[PASS] E2 index.db paired with a different vLog is detected and rebuilt
[PASS] E3 get() with a bad VPtr throws VLogReadError
[PASS] E4 0-byte doc works; docs over the 16 MiB entry limit rejected before writing
[PASS] E5 two threads put the same key: higher seq wins, live and after reopen
[PASS] E6 concurrent get + put on one key: old or new value, never a mix
[PASS] E7 scan edge cases: empty tree, start >= end, limit 0, 256-byte keys
[PASS] E8 200k sequential keys: height 4, no evictions, reopen
[PASS] E9 index.db pwrite/fsync failure aborts; reopen loses no acked write
[Budget] bytes written: 46405441 of 536870912
=== [asan] test_edge_cases: exit 0 ===

=== [asan] test_superblock ===
[PASS] SB1 slot CRCs reject every single-bit flip in the 96 checked bytes (768 flips; 5 out-of-range fields rejected)
[PASS] SB2 ping-pong: epochs alternate slots, read picks the highest valid epoch
[PASS] SB3 bit flip in slot 0, slot 1, both; torn write; short file: the valid slot is used, both bad -> Corrupt
[PASS] SB4 store: both slots invalid or superblock.db missing -> full rebuild (102 keys correct)
[Budget] bytes written: 235892 of 536870912
=== [asan] test_superblock: exit 0 ===

=== [asan] test_vlog_crash ===
[PASS] V1 CRC32C: ARMv8 and table versions give 0xe3069283 and agree on 100 random payloads
[PASS] V2 reader rejects total_len < 32, unaligned, > 16 MiB, past EOF
[PASS] V3 torn tail truncated in the last segment; fatal in a non-last segment; mid-log CRC and bounds corruption fatal
[PASS] V4 should_apply: older seqs skipped, tombstones erase; a second replay of 3 delta entries applies 0
[PASS] V5 reopen continues at seq 101 in segment 2; all 101 entries readable
[PASS] V6 torn segment header, entry header and payload: truncated when last, fatal when not last
[PASS] V7 redo over a checkpointed state applies seqs 6-10 only; a second pass applies 0
[PASS] V8 torn segment creation (3 shapes) is unlinked; two later restarts open normally
[PASS] V9 kill -9 x50 during group commit (all 13028 acked seqs among 13141 recovered, bytes verified; recovered-but-unacked 1-4 per run; torn tail in 0 runs)
[PASS] V10 group commit benchmark (1t: 131 w/s, 4t: 260 w/s, 16t: 1048 w/s, 64t: 4958 w/s)
[Budget] bytes written: 3207257 of 536870912
=== [asan] test_vlog_crash: exit 0 ===

=== [asan] bytes written by all test binaries: 367791566 (limit 536870912) ===
```

---

## 3. Real Hardware Write Amplification Benchmark (`make bench`)

*Target: Empirical measurement of write reduction and get() latency by chain depth on physical NVMe storage.*

```text
praveen@Praveens-MacBook-Air LSM % cd /Users/praveen/Code/LSM
praveen@Praveens-MacBook-Air LSM % make bench
check-deps: OK (vlog/ and doc/ have no upward includes)
./build/bench/bench_delta
Hardware: Apple M5, 17 GB RAM, macOS 27.0 (arm64), compiler Apple LLVM 21.0.0 (clang-2100.3.34.2)
Planned: about 179 MB of vLog writes (plus index.db checkpoints), budget 256 MiB; files are deleted after each run.

Bytes written per update of one field (10000 updates, one key):
doc    method            vLog B/op   index B/op   total B/op      ratio     deltas      fulls
1 KB   put (full)             1072          3.3         1075      1.00x          0      10000
       update cap 2            421          3.3          425      2.53x       6667       3333
       update cap 4            291          3.3          294      3.65x       8000       2000
2 KB   put (full)             2096          4.1         2100      1.00x          0      10000
       update cap 2            763          2.5          765      2.75x       6667       3333
       update cap 4            496          2.5          498      4.21x       8000       2000
8 KB   put (full)             8240         16.4         8257      1.00x          0      10000
       update cap 2           2810          5.7         2816      2.93x       6667       3333
       update cap 4           1725          4.1         1729      4.78x       8000       2000

get() latency by chain depth (5000 gets per depth, page cache warm):
doc    cap   depth      p50 us     p99 us  vLog reads
1 KB   2     0            15.1       25.4           1
       2     1            38.2       43.6           2
       2     2            53.6       60.6           3
1 KB   4     0            15.0       15.8           1
       4     1            38.0       45.4           2
       4     2            52.9       61.5           3
       4     3            67.0       78.0           4
       4     4            83.0       94.2           5
2 KB   2     0            15.2       18.7           1
       2     1            46.1       53.3           2
       2     2            61.0       70.2           3
2 KB   4     0            15.1       16.3           1
       4     1            46.2       55.3           2
       4     2            61.3       70.6           3
       4     3            76.1       86.9           4
       4     4            91.0      103.3           5
8 KB   2     0            16.8       19.7           1
       2     1            87.5      102.6           2
       2     2           110.1      116.1           3
8 KB   4     0            16.9       18.7           1
       4     1            89.8      102.4           2
       4     2           114.2      127.2           3
       4     3           119.8      140.8           4
       4     4           131.7      143.8           5

[Budget] bytes written: 180082624 of 268435456
```
