#pragma once
// VLogReader.hpp - reads the value log: point reads by VPtr (bounds + CRC checked) and the
// recovery scan of one segment, which truncates a torn tail of the last segment and fails loudly
// on anything that looks like corruption in the middle of the log.

#include "CRC32.hpp"
#include "VLogFormat.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vlog {

// F_FULLFSYNC on macOS: plain fsync() does not flush the drive's write cache there.
inline int full_fsync(int fd) {
#if defined(__APPLE__)
    return fcntl(fd, F_FULLFSYNC);
#else
    return fdatasync(fd);
#endif
}

constexpr uint32_t kMaxEntryLen = 16 * 1024 * 1024; // largest total_len accepted by readers

struct RecoveredEntry {
    VPtr vptr;
    EntryHeader header;
    std::string key;
    std::vector<uint8_t> payload;
};

struct ScanResult {
    std::vector<RecoveredEntry> entries;
    bool truncated{false}; // the scan cut a torn tail (only ever in the last segment)
};

class VLogReader {
public:
    explicit VLogReader(const std::string& dir_path) : dir_path_(dir_path) {}

    // Reads the entry at vptr. False if the segment is missing, the bounds or length do not match,
    // or the CRC fails. Never returns unchecked bytes.
    bool readEntry(const VPtr& vptr, EntryHeader& out_hdr, std::string& out_key,
                   std::vector<uint8_t>& out_payload) {
        int fd = open(getSegmentPath(vptr.segment_id).c_str(), O_RDONLY);
        if (fd < 0) return false;
        // Read-only fd: a failing close() cannot lose data, so its result is not needed.
        struct stat st;
        if (fstat(fd, &st) != 0) {
            (void)close(fd);
            return false;
        }
        const uint64_t file_size = static_cast<uint64_t>(st.st_size);
        EntryHeader hdr;
        if (vptr.offset + sizeof(EntryHeader) > file_size ||
            pread(fd, &hdr, sizeof(hdr), vptr.offset) != static_cast<ssize_t>(sizeof(hdr)) ||
            !validateHeaderBounds(hdr, vptr.offset, file_size) || hdr.total_len != vptr.length) {
            (void)close(fd);
            return false;
        }
        std::vector<uint8_t> entry_buf(hdr.total_len);
        const bool full = pread(fd, entry_buf.data(), hdr.total_len, vptr.offset) ==
                          static_cast<ssize_t>(hdr.total_len);
        (void)close(fd);
        if (!full || crc32c(entry_buf.data() + 4, hdr.total_len - 4) != hdr.crc32c) return false;
        out_hdr = hdr;
        extractKeyAndPayload(hdr, entry_buf, &out_key, &out_payload);
        return true;
    }

    // Reads the entry at vptr from an open descriptor of its segment with a single pread of
    // vptr.length bytes, then the same checks as readEntry (header bounds, length, CRC). A short
    // read (the entry would end past the end of the file) fails.
    static bool readEntryAt(int fd, const VPtr& vptr, EntryHeader& out_hdr, std::string& out_key,
                            std::vector<uint8_t>& out_payload) {
        if (vptr.length < sizeof(EntryHeader) || vptr.length > kMaxEntryLen) return false;
        std::vector<uint8_t> buf(vptr.length);
        if (pread(fd, buf.data(), vptr.length, vptr.offset) != static_cast<ssize_t>(vptr.length))
            return false;
        EntryHeader hdr;
        std::memcpy(&hdr, buf.data(), sizeof(hdr));
        if (!validateHeaderBounds(hdr, vptr.offset, uint64_t(vptr.offset) + vptr.length) ||
            hdr.total_len != vptr.length ||
            crc32c(buf.data() + 4, hdr.total_len - 4) != hdr.crc32c)
            return false;
        out_hdr = hdr;
        extractKeyAndPayload(hdr, buf, &out_key, &out_payload);
        return true;
    }

    // Recovery scan of one segment from start_offset (an entry boundary).
    //  - A torn tail is allowed only in the LAST segment: it is truncated with ftruncate + fsync.
    //    The same damage in a non-last segment throws (acked data would be lost).
    //  - A bad CRC or impossible bounds with a valid entry somewhere after it is mid-log
    //    corruption, not a torn tail: it throws instead of truncating valid entries.
    //  - Last segment with a short or corrupt segment header (torn creation): truncated to 0.
    ScanResult scanSegment(uint32_t segment_id, bool is_last_segment = true,
                           uint32_t start_offset = kBlockSize) {
        ScanResult result;
        const std::string seg = std::to_string(segment_id);
        int fd = open(getSegmentPath(segment_id).c_str(), O_RDWR);
        if (fd < 0) return result;
        struct stat st;
        if (fstat(fd, &st) != 0) {
            (void)close(fd);
            return result;
        }
        const uint64_t file_size = static_cast<uint64_t>(st.st_size);

        // Throws for a non-last segment; otherwise truncates at offset. Every truncation is
        // fsynced before close, so the close() result cannot lose data.
        auto tornOrFatal = [&](uint64_t offset, const std::string& non_last_msg) {
            if (!is_last_segment) {
                (void)close(fd);
                throw std::runtime_error("Fatal: " + non_last_msg + " in non-last segment " + seg);
            }
            safeTruncateAndSync(fd, offset);
            result.truncated = true;
        };

        if (file_size < kBlockSize) {
            tornOrFatal(0, "Incomplete SegmentHeader");
            (void)close(fd);
            return result;
        }
        SegmentHeader seg_hdr;
        if (pread(fd, &seg_hdr, sizeof(seg_hdr), 0) == static_cast<ssize_t>(sizeof(seg_hdr))) {
            const bool valid = seg_hdr.magic == kSegmentMagic &&
                               seg_hdr.format_version == kFormatVersion &&
                               seg_hdr.crc32c == crc32c(&seg_hdr, offsetof(SegmentHeader, crc32c));
            if (!valid) {
                // Nothing after a bad header can be trusted: never scan past it.
                tornOrFatal(0, "Corrupt SegmentHeader");
                (void)close(fd);
                return result;
            }
        }

        uint64_t offset = start_offset;
        while (offset < file_size) {
            EntryHeader hdr;
            if (offset + sizeof(EntryHeader) > file_size) {
                tornOrFatal(offset, "Incomplete/torn entry at EOF");
                break;
            }
            if (pread(fd, &hdr, sizeof(hdr), offset) != static_cast<ssize_t>(sizeof(hdr))) {
                tornOrFatal(offset, "Read error on EntryHeader");
                break;
            }
            if (!validateHeaderBounds(hdr, offset, file_size)) {
                if (hasValidEntryAhead(fd, offset + kEntryAlign, file_size)) {
                    (void)close(fd);
                    throw std::runtime_error("Fatal: Mid-log corruption detected! Valid entries "
                                             "follow corrupt header at segment " +
                                             seg + " offset " + std::to_string(offset));
                }
                tornOrFatal(offset, "Corrupted tail or torn write");
                break;
            }
            std::vector<uint8_t> entry_buf(hdr.total_len);
            if (pread(fd, entry_buf.data(), hdr.total_len, offset) !=
                static_cast<ssize_t>(hdr.total_len)) {
                tornOrFatal(offset, "Incomplete entry payload read");
                break;
            }
            if (crc32c(entry_buf.data() + 4, hdr.total_len - 4) != hdr.crc32c) {
                if (hasValidEntryAhead(fd, offset + kEntryAlign, file_size)) {
                    (void)close(fd);
                    throw std::runtime_error("Fatal: Mid-log corruption detected! Valid entries "
                                             "follow bad CRC at segment " +
                                             seg + " offset " + std::to_string(offset));
                }
                tornOrFatal(offset, "Corrupted CRC");
                break;
            }
            RecoveredEntry rec;
            rec.vptr = VPtr{segment_id, static_cast<uint32_t>(offset), hdr.total_len};
            rec.header = hdr;
            extractKeyAndPayload(hdr, entry_buf, &rec.key, &rec.payload);
            result.entries.push_back(std::move(rec));
            offset += hdr.total_len;
        }
        (void)close(fd);
        return result;
    }

private:
    std::string getSegmentPath(uint32_t seg_id) const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/vlog_%08u.log", seg_id);
        return dir_path_ + buf;
    }

    static bool validateHeaderBounds(const EntryHeader& hdr, uint64_t offset, uint64_t file_size) {
        return hdr.total_len >= sizeof(EntryHeader) && hdr.total_len <= kMaxEntryLen &&
               hdr.total_len % kEntryAlign == 0 && offset + hdr.total_len <= file_size;
    }

    static void extractKeyAndPayload(const EntryHeader& hdr, const std::vector<uint8_t>& buf,
                                     std::string* key, std::vector<uint8_t>* payload) {
        key->clear();
        if (hdr.key_len > 0)
            key->assign(reinterpret_cast<const char*>(buf.data() + sizeof(EntryHeader)),
                        hdr.key_len);
        payload->clear();
        const uint32_t p_off = payload_offset(hdr.key_len);
        if (hdr.payload_len > 0 && p_off + hdr.payload_len <= hdr.total_len) {
            payload->assign(buf.data() + p_off, buf.data() + p_off + hdr.payload_len);
        }
    }

    // True if any 8-aligned offset in [start_offset, start_offset + kMaxEntryLen) holds an entry
    // with valid bounds and CRC. Used to tell a torn tail (nothing valid follows) from mid-log
    // corruption (valid entries follow).
    static bool hasValidEntryAhead(int fd, uint64_t start_offset, uint64_t file_size) {
        uint64_t scan_off = align_up(static_cast<uint32_t>(start_offset), kEntryAlign);
        const uint64_t max_scan = std::min(file_size, start_offset + kMaxEntryLen);
        while (scan_off + sizeof(EntryHeader) <= max_scan) {
            EntryHeader cand;
            if (pread(fd, &cand, sizeof(cand), scan_off) != static_cast<ssize_t>(sizeof(cand)))
                break;
            if (validateHeaderBounds(cand, scan_off, file_size)) {
                std::vector<uint8_t> buf(cand.total_len);
                if (pread(fd, buf.data(), cand.total_len, scan_off) ==
                        static_cast<ssize_t>(cand.total_len) &&
                    crc32c(buf.data() + 4, cand.total_len - 4) == cand.crc32c) {
                    return true;
                }
            }
            scan_off += kEntryAlign;
        }
        return false;
    }

    static void safeTruncateAndSync(int fd, uint64_t offset) {
        if (ftruncate(fd, static_cast<off_t>(offset)) != 0) {
            throw std::runtime_error("Fatal: ftruncate failed at offset " + std::to_string(offset));
        }
        if (full_fsync(fd) != 0) {
            throw std::runtime_error("Fatal: full_fsync failed after truncation at offset " +
                                     std::to_string(offset));
        }
    }

    std::string dir_path_;
};

} // namespace vlog
