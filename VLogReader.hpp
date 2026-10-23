#pragma once

#include "VLogFormat.hpp"
#include "CRC32.hpp"
#include <string>
#include <vector>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace vlog {

// Durability flush helper
inline int full_fsync(int fd) {
#if defined(__APPLE__)
    return fcntl(fd, F_FULLFSYNC);
#else
    return fdatasync(fd);
#endif
}

constexpr uint32_t kMaxEntryLen = 16 * 1024 * 1024; // 16 MiB max entry bounds cap

struct RecoveredEntry {
    VPtr vptr;
    EntryHeader header;
    std::string key;
    std::vector<uint8_t> payload;
};

struct ScanResult {
    std::vector<RecoveredEntry> entries;
    bool truncated{false};
    operator std::vector<RecoveredEntry>() const { return entries; }
};

class VLogReader {
private:
    std::string dir_path_;

    std::string getSegmentPath(uint32_t seg_id) const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/vlog_%08u.log", seg_id);
        return dir_path_ + buf;
    }

    // Helper: Validates basic header bounds
    static bool validateHeaderBounds(const EntryHeader& hdr, uint64_t offset, uint64_t file_size) {
        if (hdr.total_len < sizeof(EntryHeader)) return false;
        if (hdr.total_len > kMaxEntryLen) return false;
        if ((hdr.total_len % kEntryAlign) != 0) return false;
        if (offset + hdr.total_len > file_size) return false;
        return true;
    }

    // Helper: Scans forward at 8-byte aligned offsets to check if any valid entry exists
    static bool hasValidEntryAhead(int fd, uint64_t start_offset, uint64_t file_size) {
        uint64_t scan_off = align_up(static_cast<uint32_t>(start_offset), kEntryAlign);
        uint64_t max_scan = std::min(file_size, start_offset + kMaxEntryLen);
        while (scan_off + sizeof(EntryHeader) <= max_scan) {
            EntryHeader cand_hdr;
            if (pread(fd, &cand_hdr, sizeof(cand_hdr), scan_off) != sizeof(cand_hdr)) {
                break;
            }
            if (validateHeaderBounds(cand_hdr, scan_off, file_size)) {
                std::vector<uint8_t> buf(cand_hdr.total_len);
                if (pread(fd, buf.data(), cand_hdr.total_len, scan_off) == static_cast<ssize_t>(cand_hdr.total_len)) {
                    uint32_t computed = crc32c(buf.data() + 4, cand_hdr.total_len - 4);
                    if (computed == cand_hdr.crc32c) {
                        return true; // Found valid entry ahead -> Mid-log corruption!
                    }
                }
            }
            scan_off += kEntryAlign;
        }
        return false;
    }

    // Helper: Safely truncates and durable fsyncs, throwing if either system call fails (Rule 3)
    static void safeTruncateAndSync(int fd, uint64_t offset) {
        if (ftruncate(fd, offset) != 0) {
            throw std::runtime_error("Fatal: ftruncate failed at offset " + std::to_string(offset));
        }
        if (full_fsync(fd) != 0) {
            throw std::runtime_error("Fatal: full_fsync failed after truncation at offset " + std::to_string(offset));
        }
    }

public:
    explicit VLogReader(const std::string& dir_path) : dir_path_(dir_path) {}

    // Point read a single entry by VPtr
    bool readEntry(const VPtr& vptr, EntryHeader& out_hdr, std::string& out_key, std::vector<uint8_t>& out_payload) {
        std::string path = getSegmentPath(vptr.segment_id);
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return false;
        }

        uint64_t file_size = static_cast<uint64_t>(st.st_size);
        if (vptr.offset + sizeof(EntryHeader) > file_size) {
            close(fd);
            return false;
        }

        EntryHeader hdr;
        if (pread(fd, &hdr, sizeof(hdr), vptr.offset) != sizeof(hdr)) {
            close(fd);
            return false;
        }

        // Reader bounds check (Rule 7)
        if (!validateHeaderBounds(hdr, vptr.offset, file_size)) {
            close(fd);
            return false;
        }

        if (hdr.total_len != vptr.length) {
            close(fd);
            return false;
        }

        std::vector<uint8_t> entry_buf(hdr.total_len);
        if (pread(fd, entry_buf.data(), hdr.total_len, vptr.offset) != static_cast<ssize_t>(hdr.total_len)) {
            close(fd);
            return false;
        }
        close(fd);

        // CRC check
        uint32_t computed_crc = crc32c(entry_buf.data() + 4, hdr.total_len - 4);
        if (computed_crc != hdr.crc32c) {
            return false;
        }

        out_hdr = hdr;

        // Extract key
        out_key.clear();
        if (hdr.key_len > 0) {
            out_key.assign(reinterpret_cast<const char*>(entry_buf.data() + sizeof(EntryHeader)), hdr.key_len);
        }

        // Extract payload
        uint32_t p_off = payload_offset(hdr.key_len);
        out_payload.clear();
        if (hdr.payload_len > 0 && p_off + hdr.payload_len <= hdr.total_len) {
            out_payload.assign(entry_buf.data() + p_off, entry_buf.data() + p_off + hdr.payload_len);
        }

        return true;
    }

    // Sequential recovery scanner for a segment with durable torn-tail truncation
    // Rule 7: Torn tail is allowed only in the LAST segment. A bad tail in a non-last segment is fatal.
    // Rule 8: Bounds failure mid-file uses the same forward scan as a CRC mismatch.
    ScanResult scanSegment(uint32_t segment_id, bool is_last_segment = true, uint32_t start_offset = kBlockSize) {
        ScanResult result;
        std::string path = getSegmentPath(segment_id);

        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0) return result;

        struct stat st;
        if (fstat(fd, &st) != 0) {
            close(fd);
            return result;
        }

        uint64_t file_size = static_cast<uint64_t>(st.st_size);

        // Check SegmentHeader if file was created
    if (file_size < kBlockSize) {
        if (!is_last_segment) {
            close(fd);
            throw std::runtime_error("Fatal: Incomplete SegmentHeader in non-last segment " + std::to_string(segment_id));
        }
        safeTruncateAndSync(fd, 0);
        close(fd);
        result.truncated = true;
        return result;
    }

    SegmentHeader seg_hdr;
    if (pread(fd, &seg_hdr, sizeof(seg_hdr), 0) == sizeof(seg_hdr)) {
        bool valid_magic = (seg_hdr.magic == kSegmentMagic && seg_hdr.format_version == kFormatVersion);
        uint32_t expected_crc = crc32c(&seg_hdr, offsetof(SegmentHeader, crc32c));
        bool valid_crc = (expected_crc == seg_hdr.crc32c);

        if (!valid_magic || !valid_crc) {
            if (!is_last_segment) {
                close(fd);
                throw std::runtime_error("Fatal: Corrupt SegmentHeader in non-last segment " + std::to_string(segment_id));
            }
            // Rule 6: Last segment with bad SegmentHeader magic/CRC: truncate to 0 (torn creation) and halt scan immediately
            safeTruncateAndSync(fd, 0);
            close(fd);
            result.truncated = true;
            return result; // NEVER keep scanning!
        }
    }

    uint64_t offset = start_offset;

    while (offset < file_size) {
        // Check if partial header at EOF
        if (offset + sizeof(EntryHeader) > file_size) {
            if (!is_last_segment) {
                close(fd);
                throw std::runtime_error("Fatal: Incomplete/torn entry at EOF in non-last segment " + std::to_string(segment_id));
            }
            safeTruncateAndSync(fd, offset);
            result.truncated = true;
            break;
        }

        EntryHeader hdr;
        if (pread(fd, &hdr, sizeof(hdr), offset) != sizeof(hdr)) {
            if (!is_last_segment) {
                close(fd);
                throw std::runtime_error("Fatal: Read error on EntryHeader in non-last segment " + std::to_string(segment_id));
            }
            safeTruncateAndSync(fd, offset);
            result.truncated = true;
            break;
        }

        // Check header bounds (Rule 8: Bounds failure uses forward scan)
        if (!validateHeaderBounds(hdr, offset, file_size)) {
            if (hasValidEntryAhead(fd, offset + kEntryAlign, file_size)) {
                close(fd);
                throw std::runtime_error("Fatal: Mid-log corruption detected! Valid entries follow corrupt header at segment " 
                                         + std::to_string(segment_id) + " offset " + std::to_string(offset));
            }
            // No valid entries ahead -> check if last segment
            if (!is_last_segment) {
                close(fd);
                throw std::runtime_error("Fatal: Corrupted tail or torn write in non-last segment " + std::to_string(segment_id));
            }
            safeTruncateAndSync(fd, offset);
            result.truncated = true;
            break;
        }

        // Read full entry
        std::vector<uint8_t> entry_buf(hdr.total_len);
        if (pread(fd, entry_buf.data(), hdr.total_len, offset) != static_cast<ssize_t>(hdr.total_len)) {
            if (!is_last_segment) {
                close(fd);
                throw std::runtime_error("Fatal: Incomplete entry payload read in non-last segment " + std::to_string(segment_id));
            }
            safeTruncateAndSync(fd, offset);
            result.truncated = true;
            break;
        }

        // Verify CRC32C over [4, total_len)
        uint32_t computed_crc = crc32c(entry_buf.data() + 4, hdr.total_len - 4);
        if (computed_crc != hdr.crc32c) {
            // Bad CRC inside file: scan forward
            if (hasValidEntryAhead(fd, offset + kEntryAlign, file_size)) {
                close(fd);
                throw std::runtime_error("Fatal: Mid-log corruption detected! Valid entries follow bad CRC at segment " 
                                         + std::to_string(segment_id) + " offset " + std::to_string(offset));
            }
            // No valid entries ahead -> check if last segment
            if (!is_last_segment) {
                close(fd);
                throw std::runtime_error("Fatal: Corrupted CRC in non-last segment " + std::to_string(segment_id));
            }
            safeTruncateAndSync(fd, offset);
            result.truncated = true;
            break;
        }

            // Valid entry!
            RecoveredEntry rec;
            rec.vptr = VPtr{segment_id, static_cast<uint32_t>(offset), hdr.total_len};
            rec.header = hdr;

            if (hdr.key_len > 0) {
                rec.key.assign(reinterpret_cast<const char*>(entry_buf.data() + sizeof(EntryHeader)), hdr.key_len);
            }

            uint32_t p_off = payload_offset(hdr.key_len);
            if (hdr.payload_len > 0 && p_off + hdr.payload_len <= hdr.total_len) {
                rec.payload.assign(entry_buf.data() + p_off, entry_buf.data() + p_off + hdr.payload_len);
            }

            result.entries.push_back(std::move(rec));
            offset += hdr.total_len;
        }

        close(fd);
        return result;
    }
};

} // namespace vlog
