#pragma once
// SuperblockManager.hpp - superblock.db: the single source of truth for the committed index.
//
// Layout: two 4096-byte slots, slot 0 at byte 0 and slot 1 at byte 4096. Each slot holds:
//   [0, 64)   vlog::Superblock (VLogFormat.hpp, unchanged); its CRC32C covers bytes [0, 60)
//   [64, 96)  SuperblockExt: binds the slot to one index.db (nonce) and one vLog (identity);
//             its own CRC32C covers its first 28 bytes
//   [96, 4096) zero
//
// Read:  decode both slots; the valid slot with the highest epoch is current. The other slot,
//        if valid and older, is the previous checkpoint (kept so its pages are never reused
//        while it could still be needed).
// Write: the slot that is NOT current (ping-pong), then F_FULLFSYNC. A torn or failed write can
//        only damage that slot; the current slot stays readable. Creating the file writes slot 0,
//        zeroes slot 1, F_FULLFSYNCs the file and fsyncs the directory.
// Every syscall is checked. A failed fsync aborts and is never retried.

#include "index/IndexCommon.hpp"
#include "vlog/CRC32.hpp"
#include "vlog/VLogFormat.hpp"
#include "vlog/VLogReader.hpp" // vlog::full_fsync
#include "vlog/VLogWriter.hpp" // vlog::sync_directory

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bpt {

constexpr uint32_t kSuperblockFormatVersion = 1;
constexpr uint32_t kSuperSlotSize = 4096;
constexpr uint64_t kSuperExtMagic = 0x3154584542505553ULL; // "SUPBEXT1" (LE bytes)

#pragma pack(push, 1)
struct SuperblockExt {           // at offset 64 of each slot
    uint64_t magic;              // kSuperExtMagic
    uint64_t index_nonce;        // equals MetaBody::index_nonce of the index.db this slot describes
    uint64_t vlog_created_ns;    // vLog identity: created_unix_ns of the lowest-id segment
    uint32_t vlog_first_segment; // vLog identity: id of that segment
    uint32_t crc32c;             // over the 28 bytes above
};
#pragma pack(pop)

static_assert(sizeof(SuperblockExt) == 32);
static_assert(offsetof(SuperblockExt, crc32c) == 28);
static_assert(sizeof(vlog::Superblock) + sizeof(SuperblockExt) <= kSuperSlotSize);

struct SuperblockRecord {
    vlog::Superblock sb{};
    SuperblockExt ext{};
};

enum class SlotState { Valid, Short, BadCrc, BadFields };

inline const char* toString(SlotState s) {
    switch (s) {
    case SlotState::Valid:
        return "Valid";
    case SlotState::Short:
        return "Short";
    case SlotState::BadCrc:
        return "BadCrc";
    case SlotState::BadFields:
        return "BadFields";
    }
    return "?";
}

// Fills magic/version/CRC fields of rec and writes the 4096-byte slot image.
inline void encodeSlot(SuperblockRecord rec, uint8_t* slot) {
    rec.sb.magic = vlog::kSuperMagic;
    rec.sb.format_version = kSuperblockFormatVersion;
    rec.sb.reserved0 = 0;
    rec.sb.crc32c = vlog::crc32c(&rec.sb, offsetof(vlog::Superblock, crc32c));
    rec.ext.magic = kSuperExtMagic;
    rec.ext.crc32c = vlog::crc32c(&rec.ext, offsetof(SuperblockExt, crc32c));
    std::memset(slot, 0, kSuperSlotSize);
    std::memcpy(slot, &rec.sb, sizeof(rec.sb));
    std::memcpy(slot + sizeof(rec.sb), &rec.ext, sizeof(rec.ext));
}

// Checks CRCs and field bounds. n is the number of bytes available for this slot.
// Page ids are uint32_t in index.db; the superblock stores them in uint64_t fields, so both
// index_root_page and index_page_count are checked against UINT32_MAX here.
inline SlotState decodeSlot(const uint8_t* slot, size_t n, SuperblockRecord* out) {
    if (n < sizeof(vlog::Superblock) + sizeof(SuperblockExt)) return SlotState::Short;
    SuperblockRecord r;
    std::memcpy(&r.sb, slot, sizeof(r.sb));
    std::memcpy(&r.ext, slot + sizeof(r.sb), sizeof(r.ext));
    if (r.sb.crc32c != vlog::crc32c(&r.sb, offsetof(vlog::Superblock, crc32c)))
        return SlotState::BadCrc;
    if (r.ext.crc32c != vlog::crc32c(&r.ext, offsetof(SuperblockExt, crc32c)))
        return SlotState::BadCrc;
    if (r.sb.magic != vlog::kSuperMagic || r.sb.format_version != kSuperblockFormatVersion ||
        r.sb.reserved0 != 0 || r.ext.magic != kSuperExtMagic) {
        return SlotState::BadFields;
    }
    if (r.sb.epoch == 0) return SlotState::BadFields;
    if (r.sb.index_page_count < 2 || r.sb.index_page_count > UINT32_MAX)
        return SlotState::BadFields;
    if (r.sb.index_root_page == 0 || r.sb.index_root_page >= r.sb.index_page_count)
        return SlotState::BadFields;
    if (r.sb.replay_offset < vlog::kBlockSize || r.sb.replay_offset % vlog::kEntryAlign != 0)
        return SlotState::BadFields;
    if (out) *out = r;
    return SlotState::Valid;
}

class SuperblockManager {
public:
    struct LoadResult {
        enum class Status { Ok, Missing, Corrupt } status = Status::Missing;
        SlotState slot[2] = {SlotState::Short, SlotState::Short};
        int current_slot = -1;
        SuperblockRecord current; // valid when status == Ok
        bool has_older = false;   // the other slot is valid and has a lower epoch
        SuperblockRecord older;
    };

    struct Options {
        std::function<void(uint64_t)> on_write; // bytes per pwrite (tests count bytes)
    };

    explicit SuperblockManager(std::string path, Options opts = {})
        : path_(std::move(path)), opts_(std::move(opts)) {}

    const std::string& path() const { return path_; }
    uint64_t epoch() const { return epoch_; }
    int currentSlot() const { return cur_slot_; }

    // Reads both slots. Does not modify the file. Remembers the current slot for the next commit.
    LoadResult load() {
        LoadResult res;
        known_ = true;
        int fd = ::open(path_.c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno != ENOENT) fatal("open failed for " + path_);
            exists_ = false;
            cur_slot_ = -1;
            epoch_ = 0;
            return res;
        }
        exists_ = true;
        uint8_t buf[2 * kSuperSlotSize];
        size_t got = 0;
        while (got < sizeof(buf)) {
            ssize_t r = ::pread(fd, buf + got, sizeof(buf) - got, static_cast<off_t>(got));
            if (r < 0) fatal("pread failed for " + path_);
            if (r == 0) break;
            got += static_cast<size_t>(r);
        }
        if (::close(fd) != 0) fatal("close failed for " + path_);
        SuperblockRecord rec[2];
        for (int i = 0; i < 2; ++i) {
            size_t start = static_cast<size_t>(i) * kSuperSlotSize;
            size_t avail = got > start ? std::min<size_t>(got - start, kSuperSlotSize) : 0;
            res.slot[i] = decodeSlot(buf + start, avail, &rec[i]);
        }
        bool v0 = res.slot[0] == SlotState::Valid, v1 = res.slot[1] == SlotState::Valid;
        if (!v0 && !v1) {
            res.status = LoadResult::Status::Corrupt;
            cur_slot_ = -1;
            epoch_ = 0;
            return res;
        }
        int cur = (v0 && (!v1 || rec[0].sb.epoch >= rec[1].sb.epoch)) ? 0 : 1;
        res.status = LoadResult::Status::Ok;
        res.current_slot = cur;
        res.current = rec[cur];
        int other = 1 - cur;
        if (res.slot[other] == SlotState::Valid && rec[other].sb.epoch < rec[cur].sb.epoch) {
            res.has_older = true;
            res.older = rec[other];
        }
        cur_slot_ = cur;
        epoch_ = rec[cur].sb.epoch;
        return res;
    }

    // Makes rec durable. rec.sb.epoch must be greater than the current epoch.
    // Existing file: writes the non-current slot, then F_FULLFSYNC.
    // No file: creates it (O_CREAT | O_EXCL) with rec in slot 0, F_FULLFSYNC, fsync of the
    // directory.
    void commit(const SuperblockRecord& rec) {
        if (!loaded_or_created()) fatal("SuperblockManager::commit before load() on " + path_);
        if (rec.sb.epoch <= epoch_) {
            errno = 0;
            fatal("superblock epoch " + std::to_string(rec.sb.epoch) + " is not above " +
                  std::to_string(epoch_));
        }
        if (rec.sb.index_page_count > UINT32_MAX ||
            rec.sb.index_root_page >= rec.sb.index_page_count) {
            errno = 0;
            fatal("superblock page fields out of range");
        }
        uint8_t slot[kSuperSlotSize];
        encodeSlot(rec, slot);
        if (!exists_) {
            int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
            if (fd < 0) fatal("open(O_CREAT|O_EXCL) failed for " + path_);
            uint8_t both[2 * kSuperSlotSize];
            std::memcpy(both, slot, kSuperSlotSize);
            std::memset(both + kSuperSlotSize, 0, kSuperSlotSize);
            pwriteAll(fd, both, sizeof(both), 0);
            if (vlog::full_fsync(fd) != 0) fatal("F_FULLFSYNC failed for new " + path_);
            if (::close(fd) != 0) fatal("close failed for " + path_);
            if (vlog::sync_directory(parentDir(path_)) != 0)
                fatal("fsync of directory failed after creating " + path_);
            exists_ = true;
            cur_slot_ = 0;
        } else {
            int target = cur_slot_ < 0 ? 0 : 1 - cur_slot_;
            int fd = ::open(path_.c_str(), O_RDWR);
            if (fd < 0) fatal("open failed for " + path_);
            pwriteAll(fd, slot, kSuperSlotSize, static_cast<off_t>(target) * kSuperSlotSize);
            if (vlog::full_fsync(fd) != 0)
                fatal("F_FULLFSYNC failed for " + path_); // never retried
            if (::close(fd) != 0) fatal("close failed for " + path_);
            cur_slot_ = target;
        }
        epoch_ = rec.sb.epoch;
        commits_++;
    }

    // Deletes superblock.db (if present) and fsyncs the directory. Used before a full rebuild,
    // so a crash during the rebuild always leads to another rebuild.
    void remove() {
        if (::unlink(path_.c_str()) != 0) {
            if (errno != ENOENT) fatal("unlink failed for " + path_);
        } else if (vlog::sync_directory(parentDir(path_)) != 0) {
            fatal("fsync of directory failed after unlinking " + path_);
        }
        exists_ = false;
        cur_slot_ = -1;
        epoch_ = 0;
        known_ = true;
    }

    uint64_t commits() const { return commits_; }

private:
    bool loaded_or_created() {
        if (!known_) {
            // First use without load(): find out whether the file exists.
            struct stat st;
            if (::stat(path_.c_str(), &st) == 0) return false; // exists but never read: refuse
            if (errno != ENOENT) fatal("stat failed for " + path_);
            exists_ = false;
            known_ = true;
        }
        return true;
    }

    void pwriteAll(int fd, const uint8_t* p, size_t n, off_t off) {
        ssize_t w = ::pwrite(fd, p, n, off);
        if (w != static_cast<ssize_t>(n))
            fatal("pwrite of " + std::to_string(n) + " bytes failed for " + path_);
        if (opts_.on_write) opts_.on_write(n);
    }

    std::string path_;
    Options opts_;
    bool exists_ = false;
    bool known_ = false;
    int cur_slot_ = -1;
    uint64_t epoch_ = 0;
    uint64_t commits_ = 0;
};

} // namespace bpt
