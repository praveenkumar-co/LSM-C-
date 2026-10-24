# Phase 4 plan: delta updates

Status: approved with changes (section 0); being implemented in the steps of section 13.

## 0. Changes from review

- 1024 stripe locks (was 128).
- The chain cap is a store option, `Options::delta_chain_cap`, default 4. `kMaxDeltaChain` (4) in
  `VLogFormat.hpp` is the hard maximum, so "max" = 4. A cap of 0 writes every update as a Full
  entry.
- New store option `Options::max_document_bytes` (default: the largest payload the vLog accepts
  for the key). `put()` and `update()` reject larger documents. Tests use a small value to check
  the limit without writing 16 MiB.
- Every error path leaves the vLog and index byte-for-byte unchanged, including open failing
  with `VLogCorruptError`:
  - full rebuild: `superblock.db` is now removed after the replay into `index.db.rebuild`
    succeeds (was: before). Crash safety is kept: a crash before the removal leaves the old
    files, which fail the same check and rebuild again; a crash after it leaves no superblock,
    which rebuilds. On `VLogCorruptError` the temp file is removed;
  - the vLog writer creates a new segment when it opens; on `VLogCorruptError` the store closes
    the writer and removes that segment (it holds only its header).
- `$unset` on a path that runs through a primitive (`{"a":5}`, unset `"a.b"`) is a no-op, like a
  missing key.
- A segment on an array that is not a canonical index (`-1`, `01`, `1e2`, `x`) -> `PathError`.
- Test names use the `P4.` prefix; the list in section 11 is the approved one.
- The existing vLog-replay helpers in older tests are not changed: those tests never write
  Delta entries. `test_delta.cpp` has its own replay oracle with the delta rule.
- Bench: chain caps 2 and 4 (4 is also the maximum).

## 1. Goal

`update(key, ops)` appends a small Delta entry (the changed paths and their new values) to the
vLog instead of a full copy of the document. The main result of the phase is a measurement of how
many bytes this saves per update compared with `put()` of the whole document.

Fixed constraints:

- The vLog format does not change. Delta entries use the existing `EntryType::Delta`,
  `DeltaPrefix`, `DeltaOpHeader`, `DeltaOpCode`, `ValueEncoding` and `kFlagConsolidated` from
  `include/vlog/VLogFormat.hpp`. That file is not edited.
- Checkpoint and superblock logic do not change.
- The index stores only pointers (`IndexValueRef`). Ops are applied when a document is read.
- `put()` keeps accepting raw bytes; no JSON validation is added to it.

Not in this phase: upsert, multi-key operations, LZ4, vLog garbage collection (Phase 5), a
JSON text syntax for ops (ops are built through a C++ API).

## 2. What already exists

| Piece | Where | State |
|---|---|---|
| `EntryType::Delta`, `DeltaPrefix` (base_seq, base VPtr, chain_depth, op_count), `DeltaOpHeader` | `VLogFormat.hpp` | defined, unused |
| `kMaxDeltaChain = 4`, `kFlagConsolidated` | `VLogFormat.hpp` | defined, unused |
| `VLogWriter::append(type, key, data, len, flags, raw_len, delta_prefix, payload_fn)` | `VLogWriter.hpp` | already writes a `DeltaPrefix` in front of the payload and sets flags |
| `BPTree::apply()` with `should_apply()` | `BPTree.hpp` | the single entry point for live writes, replay and rebuild; treats Delta like Full today |
| Sequencer (`applyInOrder`) | `IndexedStore.hpp` | applies index updates in seq order |

## 3. Public API

```cpp
namespace doc {
struct UpdateOp {                 // built with the helpers below
    enum class Kind { Set, Unset, IncI64, IncF64 };
    Kind kind;
    std::string path;             // dotted path, e.g. "profile.score"
    Value value;                  // Set: any JSON value; Inc: Int or Double; Unset: unused
};
UpdateOp set(std::string path, Value v);
UpdateOp unset(std::string path);
UpdateOp inc(std::string path, int64_t by);
UpdateOp inc(std::string path, double by);
}

struct UpdateResult {
    uint64_t seq;          // seq of the written entry, or the current seq if nothing was written
    bool written;          // false: the ops changed nothing, no entry was appended
    bool consolidated;     // true: a Full entry with kFlagConsolidated was written
    uint16_t chain_depth;  // depth of the key's chain after this update (0 = Full)
};
UpdateResult IndexedStore::update(std::string_view key, const std::vector<doc::UpdateOp>& ops);

struct ReadStats {
    uint32_t vlog_reads;   // entries read from the vLog for this get (chain_depth + 1)
    uint16_t chain_depth;
};
std::optional<std::string> IndexedStore::get(std::string_view key, uint64_t* seq_out = nullptr,
                                             ReadStats* stats = nullptr) const;
```

Errors, all thrown before anything is written (no vLog bytes, no seq consumed):

| Exception | Cause |
|---|---|
| `doc::NotFoundError` | key absent or deleted |
| `doc::TypeError` | stored value is not a JSON object (including bytes that do not parse); subfield of a non-object; `$inc` on a non-number; `$unset` of an array element |
| `doc::ConflictError` | two ops in one update touch overlapping paths |
| `doc::RangeError` | array index out of range; int64 overflow in `$inc`; result deeper than 64 levels; result larger than the maximum entry payload |
| `doc::PathError` | empty path, empty segment (`"a..b"`), more than 64 segments, path longer than 1024 bytes, non-canonical array index |
| `std::invalid_argument` | bad key length, empty op list (same as `put`) |

All derive from `doc::UpdateError : std::runtime_error`. `doc::ParseError` (bad JSON) is
separate; `update()` reports a stored value that does not parse as `TypeError`.

## 4. JSON library (`include/doc/Json.hpp`)

- `doc::Value`: a tagged tree with owned storage: Null, Bool, Int (int64), Double, String,
  Array (`std::vector<Value>`), Object (`std::vector<std::pair<std::string, Value>>`). No
  `string_view` into input buffers. Int and Double are distinct types.
- Objects keep insertion order, so serializing the same tree always gives the same bytes. Key
  lookup is linear; documents in this phase are small (measured up to 8 KB).
- `doc::parse(std::string_view) -> Value` (throws `doc::ParseError`):
  - strict RFC 8259 grammar: no comments, no trailing commas, no leading zeros, no `+`, no
    `NaN`/`Infinity` tokens;
  - duplicate keys in one object are rejected;
  - escapes: `\" \\ \/ \b \f \n \r \t \uXXXX`; anything else is rejected; `\uXXXX` surrogate pairs
    are combined; a lone or reversed surrogate is rejected; raw strings must be valid UTF-8;
    unescaped control characters (< 0x20) are rejected;
  - numbers: a token without fraction or exponent that fits in int64 is Int; one outside the
    int64 range is rejected (no silent precision loss); a token with fraction or exponent is
    Double; a Double that overflows to infinity (e.g. `1e400`) is rejected; `-0` is Int 0;
  - nesting depth limit 64 (an array or object counts one level); input size limit
    `kMaxEntryLen` (16 MiB).
- `doc::serialize(const Value&) -> std::string`, the canonical form: no whitespace, keys in
  insertion order, minimal string escaping (`"`, `\`, control characters), UTF-8 output as is.
  Doubles use the shortest round-trip form (`std::to_chars`), and always contain `.` or `e`
  (`1.0`, not `1`), so a serialized Double parses back as a Double. `-0.0` is kept.
- Not built with `-ffast-math`: `$inc` on doubles must give the same bits at write time and at
  every later read.

## 5. Path and op semantics (`include/doc/Update.hpp`)

Paths are split on `.`. Object keys that contain `.` cannot be addressed by a path; they can
still be stored and read through `put()`/`get()`, and `$set` of a parent object replaces them as
part of that object. This is a documented limitation.

Path resolution, segment by segment:

- current node is an Object: the segment is a key (a numeric segment such as `"0"` is the key
  `"0"`);
- current node is an Array: the segment must be a decimal index without sign or leading zeros
  (else `PathError`); index >= size -> `RangeError`;
- current node is anything else and more segments follow -> `TypeError` for `$set` / `$inc`,
  no-op for `$unset`.

| Op | Target exists | Target missing | Missing intermediate |
|---|---|---|---|
| `$set path v` | replaced by v | created (appended to its object) | object created (`$set`, `$inc` only) |
| `$unset path` | removed from its object; array element -> `TypeError` | no-op | no-op (also through a primitive) |
| `$inc path n` | Int + Int -> Int (overflow -> `RangeError`); any Double involved -> Double; non-number -> `TypeError` | created as 0 + n | object created |

Rules for one `update()` call:

- Ops apply in the order given, to the document as left by the previous op.
- Paths must not overlap: no path may equal another or be a segment-wise prefix of another
  (`"a"` and `"a.b"`). `"a.b"` and `"a.bc"` do not overlap. Checked before anything else ->
  `ConflictError`.
- Missing intermediates under an Array are not created: a numeric segment past the end of an
  array is always `RangeError`.
- The result must stay within 64 levels and the maximum entry payload, else `RangeError`.
- If the serialized result equals the serialized current document, nothing is written
  (`written = false`).

## 6. Delta entry layout (existing format, no changes)

```
EntryHeader   type = Delta, flags = 0, seq = new seq, key_len, payload_len = raw_len
key           padded to 8
DeltaPrefix   base_seq, base (VPtr of the current version), chain_depth, op_count
op_count x    DeltaOpHeader { opcode, encoding, path_len, value_len }, path, value, pad to 8
```

| UpdateOp | opcode | encoding | value bytes |
|---|---|---|---|
| set | `Set` | `JsonText` | canonical JSON of the value |
| unset | `Unset` | `None` | none (value_len = 0) |
| inc int64 | `IncI64` | `I64` | 8 bytes little-endian |
| inc double | `IncF64` | `F64` | 8 bytes IEEE-754 little-endian |

`chain_depth` of the new delta = 1 if the current version is a Full entry, else the current
delta's `chain_depth + 1`. So `chain_depth == 1` means the base is a Full entry.

## 7. Write path

### Striped key locks

`IndexedStore` gets 1024 `std::mutex` stripes; a key maps to `hash(key) % 128`. `put`, `erase`
and `update` each take exactly one stripe and hold it until their index update is applied. Lock
order inside one operation:

```
stripe lock -> read the key's current version (index + vLog) -> vLog append (group commit)
            -> sequencer (seq_mu_) -> index (tree lock) -> release stripe
```

Why there is no deadlock:

- Each operation holds at most one stripe and never asks for a second one.
- A thread waiting in the sequencer for seq s holds a stripe; every thread with a seq below s
  already holds its own stripe and needs only the sequencer and the tree lock, which never wait
  on a stripe. So the lowest unapplied seq can always make progress.
- `checkpoint()` takes `seq_mu_` and the tree lock but never a stripe lock.
- `get()` and `scan()` take no stripe lock.

Cost: writes to the same key (or to keys sharing a stripe) are serialized across the whole
group commit, so their rate is bounded by the fsync rate (about 270 per second on the test Mac).
Different stripes still share group commits as before.

### update(key, ops)

1. Validate key, op list, paths (`PathError`, `ConflictError`).
2. Take the key's stripe.
3. Read the current version: index lookup (absent -> `NotFoundError`), then materialize the
   document by following its chain (section 8). Not a JSON object -> `TypeError`.
4. Apply the ops to an in-memory copy. Any error -> throw, nothing written.
5. If the canonical serialization did not change -> return `written = false`.
6. Encode the delta. Write a Full entry with `kFlagConsolidated` (payload = canonical
   serialization of the new document) instead of a Delta if either:
   - the new `chain_depth` would exceed `Options::delta_chain_cap` (default 4, at most
     `kMaxDeltaChain`), or
   - the delta payload (`DeltaPrefix` + ops) is larger than 50% of the new document's serialized
     size.
7. Otherwise append the Delta with `base_seq` / `base` = the current version's seq and VPtr,
   read in step 3 under the stripe lock (so it is still current).
8. Apply through the sequencer and `BPTree::apply` (section 9), release the stripe, return.

`put()` and `erase()` keep their current behavior, plus the stripe lock.

## 8. Read path

`get(key)`:

1. Index lookup -> `{seq, vptr}`.
2. Read the entry at vptr. If it is a Full entry: depth 0, return its payload bytes unchanged
   (same as today, so documents written only by `put()` come back byte for byte).
3. If it is a Delta: decode its prefix and ops, then read `prefix.base`, and repeat until a Full
   entry is reached. Then parse the Full payload and apply the deltas oldest first; return the
   canonical serialization. After the first update, a document that was put with whitespace
   comes back in canonical form.
4. Checks on every hop, any failure -> `VLogReadError` (no bytes returned):
   - CRC and bounds (existing `readEntry`), key matches, entry seq == expected seq (the index
     seq for the first entry, then the delta's `base_seq`), type is Full or Delta;
   - `chain_depth` goes down by exactly 1 per hop, a Full entry is reached exactly when depth 1
     points to it, at most `kMaxDeltaChain` hops;
   - ops decode cleanly and apply without error (they were validated at write time, so a failure
     here means corruption).
5. `ReadStats.vlog_reads` = number of entries read (chain_depth + 1).

vLog entries are never modified or deleted in this phase, so a chain read without locks stays
valid even while the key is being updated; `get()` returns the version the index pointed to when
the lookup ran.

## 9. Index apply and replay rule

`BPTree::apply()` gets a `delta_base_seq` argument (used only for Delta entries). For Full and
Tombstone entries nothing changes (`should_apply()`). For a Delta entry, under the tree's
exclusive lock, in this order:

1. key present and `index_seq >= delta.seq` -> Skipped (already applied).
2. else key absent, or `index_seq != delta.base_seq` -> `ApplyResult::DeltaBaseMismatch`.
3. else Applied: the index entry becomes `{seq = delta.seq, vptr = delta's VPtr}`.

Callers:

- live `update()`: `DeltaBaseMismatch` is an internal invariant failure -> `fatal()` (cannot
  happen while the stripe lock is held);
- replay from the replay point and full rebuild (`IndexRecovery.hpp`): decode the `DeltaPrefix`
  from the scanned payload and pass `base_seq`. `DeltaBaseMismatch` -> open throws a new
  `VLogCorruptError` naming segment, offset, key and both seqs. Nothing in the vLog is modified.
  As with `VLogBehindError`, the store stays unopenable until someone looks at it. (A full
  rebuild would hit the same entry, so there is no fallback.)

Why rule 2 holds in Phase 4: an entry's base is always the key's previous entry in the vLog (the
stripe lock guarantees it), the vLog is in seq order, and no entry is ever removed. Replay from
the replay point starts from an index that reflects every seq up to `checkpoint_seq`; a full
rebuild starts from the first segment, where every chain begins with a Full entry.

A consolidated Full entry (`kFlagConsolidated`) is a normal new version: `should_apply()` treats
it like any Full entry (only `kFlagRelocated` is special).

The test helpers that rebuild the expected state from the vLog independently
(`checkIndexEqualsVLog` in test_checkpoint and test_edge_cases, `vlogOracle` in test_bptree)
implement the same three-step rule on their own maps.

## 10. Measurement (main result)

A separate benchmark, `bench/bench_delta.cpp`, built and run by a new `make bench` target. It
is not part of `test`, `asan`, `tsan` or `test-quick`, because the full-document baseline alone
writes about 110 MB. It uses its own temp dir, the 10 GB free-space check, and its own write
budget of 256 MiB.

Workload, for document sizes 1 KB, 2 KB and 8 KB (generated deterministically: a nested
`profile` object with an integer `score`, plus filler fields to reach the size):

- (a) baseline: 10,000 x `put()` of the full document with `profile.score` changed;
- (b) delta: 10,000 x `update(key, {inc("profile.score", 1)})`.

Single thread, default checkpoint triggers (4 MiB / 30 s), final checkpoint on close. Reported
per run:

| Column | Source |
|---|---|
| vLog bytes per update | `vlogBytesWritten()` / 10,000 |
| index.db + superblock.db bytes per update | `on_index_write` hook / 10,000 |
| total bytes per update, and ratio (a) / (b) | engine-level write amplification saved |
| Delta entries vs consolidated Full entries | expected 8,000 / 2,000 |
| F_FULLFSYNC per update | expected 1 for both |

Expected result before measuring: with `kMaxDeltaChain = 4`, every 5th update writes a full
copy, so the vLog ratio is capped below 5x. Estimate for a 16-byte key: a delta entry is about
104 bytes; 1 KB doc: 5 x 1,072 / (4 x 104 + 1,072) = about 3.6x; 8 KB doc: about 4.8x. The
measured numbers replace this estimate in the README.

get() latency: for chain depth 0 to 4 (and 1 KB / 8 KB documents), 10,000 gets of a key at that
depth; report median and p99 in microseconds and `vlog_reads` (expected depth + 1). Page cache
warm, single thread. Each vLog read opens the segment file (known limitation), and that cost
shows up here.

The quick test suite gets a small version (D10 below) that checks the ratio is above 2 for 1 KB
documents; it is a check, not a benchmark.

## 11. Tests

Quick mode uses fixed op counts only (rule in `tests/test_common.hpp`). Quick total must stay
under 18 MiB; it is about 15.0 MiB now, so the new tests get about 2.5 MiB together. Fork +
kill -9 tests are skipped under TSAN, as today.

`tests/test_doc.cpp` (no disk writes): P4.A1-A8 (JSON), P4.B1-B4 (paths), P4.C1-C5 and C7
(ops against an in-memory oracle).

`tests/test_delta.cpp`: P4.C6, C8-C10 (store-level op behavior), P4.D1-D7 (chains), P4.E1-E3
(mixed sequences), P4.F1-F5 (concurrency, also under TSAN), P4.G1-G8 (crash and recovery; fork
+ kill -9 tests skipped under TSAN).

| Group | Tests |
|---|---|
| JSON | A1 round trip keeps tree and key order; A2 all types, empty `{}` and `[]`; A3 escapes incl. surrogate pairs, lone/reversed surrogates and bad escapes rejected; A4 duplicate keys, NaN/Inf, trailing commas, comments, leading zeros, unquoted keys, trailing garbage, raw control characters, invalid UTF-8 rejected; A5 int64 min/max, beyond rejected, -0.0 / 1e308 / 5e-324 round-trip exactly; A6 depth 64 ok, 65 rejected; A7 max size ok, +1 byte rejected; A8 multi-byte UTF-8 |
| Paths | B1 valid paths incl. `items.0` and Unicode keys; B2 `""`, `"."`, `"a."`, `".a"`, `"a..b"`, `"items.-1"`, `"items.01"`, `"items.1e2"` rejected; B3 index out of range; B4 64 segments ok, 65 rejected |
| Ops | C1 `$set`; C2 subfield on a primitive; C3 `$unset`; C4 `$inc` incl. overflow; C5 conflicts; C6 no-op writes nothing; C7 1,000 ops in one update; C8 NotFound on missing and erased keys; C9 TypeError on raw bytes, JSON array, number; C10 every error case leaves the vLog byte count unchanged |
| Chains | D1 depth 1..cap, vlog_reads = depth + 1; D2 consolidation after cap deltas; D3 delta > 50% -> Full; D4 chain spans segments; D5 small update near the max document size; D6 caps 0-4 give identical documents; D7 a Full entry from put() returned byte for byte |
| Mixed | E1 put -> update -> update -> get; E2 erase -> update = NotFound, erase -> put -> update uses the new base; E3 raw bytes -> TypeError -> JSON put -> update ok |
| Concurrency | F1 8 threads x 1,000 `$inc` on one key (quick: x 100); F2 update + put on one key; F3 update + erase on one key; F4 many keys + concurrent gets, never a partial document; F5 checkpoints during updates |
| Crash | G1 kill -9 during `$inc`: acked <= value <= acked + 1 and value == `$inc` entries in the vLog; G2 kill -9 during random updates on many keys; G3 delta appended but not applied at crash -> replayed; G4 checkpoint mid-chain; G5 recover twice + full rebuild give identical state; G6 base_seq mismatch -> VLogCorruptError, files unchanged; G7 corrupt base entry -> get() error; G8 missing segment with a chain's base -> get() error |

Budget: test_doc + test_delta under 2.5 MiB in quick mode; quick total under 18 MiB. Each test
prints its bytes on its `[PASS]` line.

## 12. Files

| File | Change |
|---|---|
| `include/doc/Json.hpp` | new: Value, parse, serialize, ParseError |
| `include/doc/Update.hpp` | new: UpdateOp, path resolution, validation, apply, error types |
| `include/index/DeltaCodec.hpp` | new: UpdateOp <-> Delta payload bytes (uses `DeltaOpHeader`; `include/doc` stays independent of `vlog/`) |
| `include/index/BPTree.hpp` | `apply()`: delta rule, `DeltaBaseMismatch` |
| `include/index/IndexRecovery.hpp` | replay/rebuild decode `DeltaPrefix`, pass base_seq, `VLogCorruptError` |
| `include/index/IndexedStore.hpp` | stripe locks, `update()`, chain-following `get()`, `ReadStats` |
| `include/vlog/*` | no change |
| `tests/test_doc.cpp`, `tests/test_delta.cpp` | new |
| `bench/bench_delta.cpp`, `Makefile` | new `make bench` target; `check-deps` also checks that `include/doc` includes nothing from `index/` or `vlog/` |
| `.github/workflows/ci.yml` | add `make tsan` only if it passes on ubuntu-latest (optional) |
| `README.md` | design section for deltas, measured results, limitations |

## 13. Order of work

Each step ends with `make test-quick`, `make asan`, `make tsan` passing, then a commit.

1. `doc/Json.hpp` + P4.A (tests/test_doc.cpp).
2. `doc/Update.hpp` + P4.B, P4.C1-C5, C7.
3. Delta rule in `BPTree::apply` and recovery (`base_seq`, `VLogCorruptError`) + P4.G5, G6.
4. Stripe locks, `update()`, chain-following `get()`, `ReadStats` + P4.C6, C8-C10, P4.D, P4.E,
   P4.F (tests/test_delta.cpp).
5. P4.G1-G4, G7, G8.
6. `bench/bench_delta.cpp` + `make bench` (prints planned bytes first): 1/2/8 KB documents x
   10,000 one-field updates, put vs update bytes per update and the ratio for caps 2 and 4,
   get() p50/p99 at depth 0..cap, hardware. README gets the measured numbers.

## 14. GC invariant for Phase 5

A Delta stores its base's VPtr, so a base entry can never be moved on its own: relocating it
(`kFlagRelocated`) would leave every delta that points at it with a dangling pointer. Rule for
Phase 5: if GC reclaims a segment holding any entry of a live chain (the Full base or any
Delta), it must first consolidate the whole chain into a new Full entry with
`kFlagConsolidated` in the active segment, with a new seq, and apply it to the index through the
normal replay rule; only after that is durable may the segment be deleted.

Open issue to solve in Phase 5: replay rule 2 is strict. After GC deletes a segment holding a
chain's Full base, older Delta entries of that chain may survive in other segments. A full
rebuild from the oldest segment reaches those deltas with the key absent (or at a different
seq) before it reaches the consolidated Full at the log's tail, and rule 2 would report
corruption. Phase 5 must either make GC never leave such deltas behind, or change how a full
rebuild treats deltas whose base is gone (for example, start the rebuild at
`oldest_live_segment` and defer base mismatches until the end of the scan, failing only if the
key's final entry still depends on them).

## 15. Decisions (approved)

1. **kill -9 test (D4)**: an increment can be fsynced but killed before its ack reaches the
   parent, so "value equals the number of acked increments" cannot be guaranteed. The test
   asserts acked <= value <= acked + 1 (one writer thread, so at most one in flight) and value ==
   the number of `$inc` entries in the recovered vLog.
2. **Savings are capped below 5x** by `kMaxDeltaChain = 4`, which lives in `VLogFormat.hpp` and
   is not changed in this phase.
3. **get() output after an update is canonical JSON**, not the original bytes; documents written
   only by `put()` come back unchanged.
4. **Integer literals outside int64 are rejected**, not converted to double.
5. **`$unset` of an array element is a TypeError** (no index shifting, no null placeholder).
6. **The measurement is a separate `make bench` target**, outside the test write budgets.
7. **A Delta base mismatch during open throws `VLogCorruptError`** (store stays closed, nothing
   modified); during a live update it aborts the process.
8. **Same-key write rate is bounded by the fsync rate**, because the stripe lock is held across
   the group commit.
