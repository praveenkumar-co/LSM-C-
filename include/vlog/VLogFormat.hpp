#pragma once
// VLogFormat.hpp — On-disk format of the Value Log (vLog), format v1.
//
// Invariants:
//  - All multi-byte integers are little-endian (x86-64 and ARM64 are both LE).
//  - Every struct is packed and size/offset-checked by static_assert.
//  - Never reinterpret_cast raw disk bytes into these structs before checking
//    length and CRC; copy into a local with std::memcpy.
//  - The vLog IS the redo log. Entries are appended in seq order, except
//    RELOCATED entries (written by GC), which carry their ORIGINAL seq.

#include <cstddef>
#include <cstdint>

namespace vlog {

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kBlockSize     = 4096;                   // LBA alignment
constexpr uint32_t kEntryAlign    = 8;
constexpr uint64_t kSegmentMagic  = 0x474F4C5653495757ULL;  // "WWISVLOG" (LE bytes)
constexpr uint64_t kSuperMagic    = 0x4B4C42505553574CULL;  // "LWSUPBLK" (LE bytes)
constexpr uint16_t kMaxDeltaChain = 4;                      // consolidate into Full at this depth

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

enum class EntryType : uint8_t { Full = 1, Delta = 2, Tombstone = 3 };

enum EntryFlags : uint8_t {
    kFlagNone         = 0,
    kFlagLZ4          = 1u << 0,  // payload is LZ4-compressed; raw_len = uncompressed size
    kFlagRelocated    = 1u << 1,  // written by GC; seq == original version's seq
    kFlagConsolidated = 1u << 2,  // Full produced by folding a delta chain
};

enum class DeltaOpCode : uint8_t { Set = 1, Unset = 2, IncI64 = 3, IncF64 = 4 };
enum class ValueEncoding : uint8_t { None = 0, JsonText = 1, I64 = 2, F64 = 3 };

#pragma pack(push, 1)

struct VPtr {
    uint32_t segment_id;
    uint32_t offset;     // entry start within segment; >= kBlockSize, 8-aligned
    uint32_t length;     // == EntryHeader::total_len
};

struct SegmentHeader {
    uint64_t magic;            // kSegmentMagic
    uint32_t format_version;
    uint32_t segment_id;       // must match the file name
    uint64_t first_seq;
    uint64_t created_unix_ns;
    uint32_t flags;            // reserved, 0
    uint32_t crc32c;           // over the 36 bytes above
};

struct EntryHeader {
    uint32_t crc32c;       // over bytes [4, total_len), padding included
    uint32_t total_len;    // 32 + align8(key_len) + align8(payload_len)
    uint64_t seq;          // global monotonic version
    uint8_t  type;         // EntryType
    uint8_t  flags;        // EntryFlags
    uint16_t key_len;
    uint32_t payload_len;  // bytes on disk (after compression)
    uint32_t raw_len;      // logical bytes
    uint32_t reserved;     // must be 0; covered by CRC
};

struct DeltaPrefix {
    uint64_t base_seq;     // version this delta applies to
    VPtr     base;         // location of that version (Full or Delta)
    uint16_t chain_depth;  // 1 => base is Full; <= kMaxDeltaChain
    uint16_t op_count;
};

struct DeltaOpHeader {     // followed by path[path_len], value[value_len], pad to 8
    uint8_t  opcode;       // DeltaOpCode
    uint8_t  encoding;     // ValueEncoding
    uint16_t path_len;     // dotted path: "status", "profile.score"
    uint32_t value_len;    // 0 for Unset
};

struct IndexValueRef {     // per key in a primary-index leaf
    uint64_t seq;          // drives idempotent redo
    uint8_t  kind;         // 0 = inline, 1 = vLog
    uint8_t  reserved[3];
    VPtr     vptr;         // valid when kind == 1
};

struct Superblock {        // two copies at manifest offsets 0 and kBlockSize
    uint64_t magic;
    uint32_t format_version;
    uint32_t reserved0;
    uint64_t epoch;               // +1 per checkpoint
    uint64_t checkpoint_seq;      // snapshot reflects all seq <= this
    uint32_t replay_segment_id;   // redo scan starts here
    uint32_t replay_offset;
    uint64_t index_root_page;
    uint64_t index_page_count;
    uint32_t oldest_live_segment; // segments below this may be unlinked
    uint32_t crc32c;              // over the 60 bytes above
};

#pragma pack(pop)

static_assert(sizeof(VPtr)          == 12);
static_assert(sizeof(SegmentHeader) == 40);
static_assert(sizeof(EntryHeader)   == 32);
static_assert(sizeof(DeltaPrefix)   == 24);
static_assert(sizeof(DeltaOpHeader) == 8);
static_assert(sizeof(IndexValueRef) == 24);
static_assert(sizeof(Superblock)    == 64);
static_assert(offsetof(EntryHeader, seq)         == 8);
static_assert(offsetof(EntryHeader, payload_len) == 20);
static_assert(offsetof(DeltaPrefix, chain_depth) == 20);
static_assert(offsetof(IndexValueRef, vptr)      == 12);
static_assert(offsetof(Superblock, crc32c)       == 60);

constexpr uint32_t payload_offset(uint16_t key_len) { return sizeof(EntryHeader) + align_up(key_len, kEntryAlign); }
constexpr uint32_t entry_size(uint16_t key_len, uint32_t payload_len) {
    return payload_offset(key_len) + align_up(payload_len, kEntryAlign);
}
constexpr uint32_t delta_op_size(uint16_t path_len, uint32_t value_len) {
    return align_up(sizeof(DeltaOpHeader) + path_len + value_len, kEntryAlign);
}

// Redo rule used by recovery and by GC's CAS.
constexpr bool should_apply(bool has_key, uint64_t idx_seq, uint64_t entry_seq, uint8_t flags) {
    if (flags & kFlagRelocated) return has_key && idx_seq == entry_seq;
    return !has_key || entry_seq > idx_seq;
}

} // namespace vlog
