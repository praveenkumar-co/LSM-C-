#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>

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

// Table-driven Castagnoli CRC32C (Polynomial 0x82F63B78 reflected)
inline uint32_t crc32c_table(const void* data, size_t len, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) {
                c = (c & 1) ? (0x82F63B78 ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        init = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Hardware-accelerated CRC32C (ARMv8 CRC extension or x86 SSE4.2)
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
    if (len >= 1) {
        crc = __crc32cb(crc, *p);
    }
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
    if (len >= 1) {
        c32 = _mm_crc32_u8(c32, *p);
    }
    return ~c32;
#else
    return crc32c_table(data, len, crc);
#endif
}

// Canonical CRC32C entry point
inline uint32_t crc32c(const void* data, size_t len, uint32_t crc = 0) {
#if defined(VLOG_HAS_ARM_CRC) || defined(VLOG_HAS_X86_CRC)
    return crc32c_hardware(data, len, crc);
#else
    return crc32c_table(data, len, crc);
#endif
}

// Validation function testing both against standard test vector "123456789" -> 0xE3069283
inline bool verify_crc32c_test_vectors() {
    const char* test_vector = "123456789";
    uint32_t table_val = crc32c_table(test_vector, 9);
    if (table_val != 0xE3069283) return false;

#if defined(VLOG_HAS_ARM_CRC) || defined(VLOG_HAS_X86_CRC)
    uint32_t hw_val = crc32c_hardware(test_vector, 9);
    if (hw_val != 0xE3069283) return false;
    if (hw_val != table_val) return false;
#endif
    return true;
}

} // namespace vlog
