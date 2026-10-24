#pragma once
// CRC32C (Castagnoli, reflected polynomial 0x82F63B78): a table version and a hardware version
// (ARMv8 CRC extension or x86 SSE4.2). crc32c() picks the hardware version when it is available.
// Every vLog entry, segment header, index page and superblock slot is checked with it.

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__has_include)
#if __has_include(<arm_acle.h>)
#include <arm_acle.h>
#define VLOG_HAS_ARM_CRC 1
#endif
#endif
#elif defined(__x86_64__) || defined(_M_X64)
#include <nmmintrin.h>
#define VLOG_HAS_X86_CRC 1
#endif

namespace vlog {

struct Crc32cTable {
    uint32_t v[256];
    Crc32cTable() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) c = (c & 1) ? (0x82F63B78 ^ (c >> 1)) : (c >> 1);
            v[i] = c;
        }
    }
};

inline uint32_t crc32c_table(const void* data, size_t len, uint32_t crc = 0) {
    static const Crc32cTable table; // thread-safe one-time initialization
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = table.v[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline uint32_t crc32c_hardware(const void* data, size_t len, uint32_t crc = 0) {
#if defined(VLOG_HAS_ARM_CRC)
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    while (len >= 8) {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = __crc32cd(crc, v);
        p += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = __crc32cw(crc, v);
        p += 4;
        len -= 4;
    }
    if (len >= 2) {
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        crc = __crc32ch(crc, v);
        p += 2;
        len -= 2;
    }
    if (len >= 1) crc = __crc32cb(crc, *p);
    return ~crc;
#elif defined(VLOG_HAS_X86_CRC)
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t c = ~crc;
    while (len >= 8) {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        c = _mm_crc32_u64(c, v);
        p += 8;
        len -= 8;
    }
    uint32_t c32 = static_cast<uint32_t>(c);
    if (len >= 4) {
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        c32 = _mm_crc32_u32(c32, v);
        p += 4;
        len -= 4;
    }
    if (len >= 2) {
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        c32 = _mm_crc32_u16(c32, v);
        p += 2;
        len -= 2;
    }
    if (len >= 1) c32 = _mm_crc32_u8(c32, *p);
    return ~c32;
#else
    return crc32c_table(data, len, crc);
#endif
}

inline uint32_t crc32c(const void* data, size_t len, uint32_t crc = 0) {
#if defined(VLOG_HAS_ARM_CRC) || defined(VLOG_HAS_X86_CRC)
    return crc32c_hardware(data, len, crc);
#else
    return crc32c_table(data, len, crc);
#endif
}

} // namespace vlog
