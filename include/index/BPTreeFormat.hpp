#pragma once
// BPTreeFormat.hpp - On-disk format of index.db (B+Tree primary index), format v1.
// See docs/PHASE2_PLAN.md section 1.
//
// Rules (same as VLogFormat.hpp):
//  - All integers are little-endian.
//  - Structs are packed and size/offset-checked by static_assert.
//  - Page bytes are copied into structs with std::memcpy, never reinterpret_cast.

#include "vlog/CRC32.hpp"
#include "vlog/VLogFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace bpt {

using page_id_t = uint32_t;

constexpr uint32_t  kPageSize           = 4096;
constexpr uint32_t  kMaxKeyLen          = 256;
constexpr uint32_t  kMinKeyLen          = 1;   // the empty key is rejected (plan Q3)
constexpr uint32_t  kIndexFormatVersion = 1;
constexpr uint32_t  kPageMagic          = 0x50544250;             // "PBTP" (LE bytes)
constexpr uint64_t  kMetaMagic          = 0x3141544D58444E49ULL;  // "INDXMTA1" (LE bytes)
constexpr page_id_t kMetaPageId         = 0;
constexpr page_id_t kNoPage             = 0;  // page 0 is the meta page, so 0 is never a child/sibling

enum class PageType : uint8_t { Meta = 1, Leaf = 2, Internal = 3 };

#pragma pack(push, 1)

struct PageHeader {            // 32 bytes at offset 0 of every page
    uint32_t crc32c;           // CRC32C over bytes [4, kPageSize)
    uint32_t magic;            // kPageMagic
    uint32_t page_id;          // must equal (file offset / kPageSize)
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

struct CellKeyHeader {         // first 2 bytes of every leaf and internal cell
    uint16_t key_len;          // kMinKeyLen..kMaxKeyLen
};

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

static_assert(sizeof(PageHeader) == 32);
static_assert(offsetof(PageHeader, page_id)        == 8);
static_assert(offsetof(PageHeader, page_type)      == 12);
static_assert(offsetof(PageHeader, slot_count)     == 14);
static_assert(offsetof(PageHeader, free_start)     == 16);
static_assert(offsetof(PageHeader, cell_start)     == 18);
static_assert(offsetof(PageHeader, frag_bytes)     == 20);
static_assert(offsetof(PageHeader, right_sibling)  == 24);
static_assert(offsetof(PageHeader, leftmost_child) == 28);
static_assert(sizeof(CellKeyHeader) == 2);
static_assert(sizeof(vlog::IndexValueRef) == 24);
static_assert(sizeof(page_id_t) == 4);
static_assert(sizeof(MetaBody) == 56);
static_assert(offsetof(MetaBody, clean_shutdown)  == 28);
static_assert(offsetof(MetaBody, max_applied_seq) == 40);
static_assert(sizeof(PageHeader) + sizeof(MetaBody) <= kPageSize);
static_assert(kPageSize <= 65535 + 1, "cell offsets are uint16");

constexpr uint32_t kHeaderSize     = sizeof(PageHeader);
constexpr uint32_t kUsableBytes    = kPageSize - kHeaderSize;  // 4064
constexpr uint32_t kSlotSize       = 2;
constexpr uint32_t kLeafValueSize  = sizeof(vlog::IndexValueRef);  // 24
constexpr uint32_t kChildValueSize = sizeof(page_id_t);            // 4

constexpr uint32_t leafCellSize(uint32_t key_len)     { return 2 + key_len + kLeafValueSize; }
constexpr uint32_t internalCellSize(uint32_t key_len) { return 2 + key_len + kChildValueSize; }

// Three max-size entries per page: a split always yields two non-empty halves and the new
// entry always fits (plan section 2.4).
static_assert(kUsableBytes >= 3 * (kSlotSize + leafCellSize(kMaxKeyLen)));
static_assert(kUsableBytes >= 3 * (kSlotSize + internalCellSize(kMaxKeyLen)));

inline bool validKeyLen(size_t n) { return n >= kMinKeyLen && n <= kMaxKeyLen; }

// memcmp over the shorter length, then shorter-first. Same order as std::string::compare.
inline int compareKeys(std::string_view a, std::string_view b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    int c = (n == 0) ? 0 : std::memcmp(a.data(), b.data(), n);
    if (c != 0) return c < 0 ? -1 : 1;
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

inline uint32_t computePageCrc(const uint8_t* page) {
    return vlog::crc32c(page + 4, kPageSize - 4);
}

inline PageHeader loadHeader(const uint8_t* page) {
    PageHeader h;
    std::memcpy(&h, page, sizeof(h));
    return h;
}

inline void storeHeader(uint8_t* page, const PageHeader& h) {
    std::memcpy(page, &h, sizeof(h));
}

// Zeroes the page and writes an empty-page header (CRC is filled in at write-back).
inline void initPage(uint8_t* page, page_id_t id, PageType type, uint8_t level) {
    std::memset(page, 0, kPageSize);
    PageHeader h{};
    h.magic      = kPageMagic;
    h.page_id    = id;
    h.page_type  = static_cast<uint8_t>(type);
    h.level      = level;
    h.slot_count = 0;
    h.free_start = static_cast<uint16_t>(kHeaderSize);
    h.cell_start = static_cast<uint16_t>(kPageSize);
    storeHeader(page, h);
}

// Checks CRC, magic, page id, type, and the header's structural fields.
// Does not walk cells (BPTree::checkInvariants does that).
inline bool checkPage(const uint8_t* page, page_id_t expected_id, std::string* why) {
    auto fail = [&](const std::string& m) {
        if (why) *why = "page " + std::to_string(expected_id) + ": " + m;
        return false;
    };
    PageHeader h = loadHeader(page);
    if (h.crc32c != computePageCrc(page)) return fail("CRC32C mismatch");
    if (h.magic != kPageMagic) return fail("bad magic");
    if (h.page_id != expected_id) return fail("page_id field is " + std::to_string(h.page_id));
    if (h.reserved0 != 0) return fail("reserved0 != 0");
    bool is_meta = (expected_id == kMetaPageId);
    if (is_meta) {
        if (h.page_type != static_cast<uint8_t>(PageType::Meta) || h.slot_count != 0) return fail("bad meta header");
        return true;
    }
    if (h.page_type == static_cast<uint8_t>(PageType::Leaf)) {
        if (h.level != 0) return fail("leaf with level != 0");
    } else if (h.page_type == static_cast<uint8_t>(PageType::Internal)) {
        if (h.level == 0) return fail("internal page with level 0");
    } else {
        return fail("bad page_type " + std::to_string(h.page_type));
    }
    if (h.free_start != kHeaderSize + kSlotSize * h.slot_count) return fail("free_start != 32 + 2*slot_count");
    if (h.free_start > h.cell_start || h.cell_start > kPageSize) return fail("free_start/cell_start out of order");
    return true;
}

} // namespace bpt
