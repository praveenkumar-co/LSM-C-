#pragma once

#include "VLogFormat.hpp"
#include "CRC32.hpp"
#include "VLogReader.hpp"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <functional>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <algorithm>
#include <cerrno>

namespace vlog {

// Directory sync helper
inline int sync_directory(const std::string& dir_path) {
    int dir_fd = open(dir_path.c_str(), O_RDONLY);
    if (dir_fd < 0) return -1;
    int sync_rc = fsync(dir_fd);
    int close_rc = close(dir_fd);
    return (sync_rc == 0 && close_rc == 0) ? 0 : -1;
}

struct AppendResult {
    VPtr vptr;
    uint64_t seq;
    operator VPtr() const { return vptr; }
};

class VLogWriter {
public:
    struct Options {
        uint32_t segment_size_limit; // Default 64 MB
        uint64_t max_bytes;          // Default 0 = unlimited write cap
        // Tail discovery start (from the index superblock's replay point). Segments below
        // scan_from_segment are not scanned; that segment is scanned from scan_from_offset.
        // 0 = scan every segment from the start.
        uint32_t scan_from_segment = 0;
        uint32_t scan_from_offset = kBlockSize;
        uint64_t min_seq = 0;        // seqs already used before the scan start (checkpoint seq)

        Options(uint32_t seg_limit = 64 * 1024 * 1024, uint64_t max_b = 0)
            : segment_size_limit(seg_limit), max_bytes(max_b) {}
    };

private:
    std::string dir_path_;
    Options options_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    uint32_t current_segment_id_{0};
    uint64_t current_seq_{0};
    uint32_t current_offset_{0};
    int current_fd_{-1};

    uint64_t total_bytes_written_{0};
    uint64_t total_ops_{0};
    uint64_t total_fsyncs_{0};
    uint64_t recovery_entries_scanned_{0};
    bool fatal_error_{false};

    struct Request {
        VPtr vptr{0, 0, 0};
        uint64_t assigned_seq{0};
        bool done{false};
        bool failed{false};
        std::condition_variable cv;
    };

    std::vector<uint8_t> staging_buffer_;
    std::deque<Request*> pending_requests_;
    bool flush_in_progress_{false};

    std::string getSegmentPath(uint32_t seg_id) const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/vlog_%08u.log", seg_id);
        return dir_path_ + buf;
    }

    void flushStagingBufferLocked() {
        if (staging_buffer_.empty() || current_fd_ < 0) return;

        ssize_t written = write(current_fd_, staging_buffer_.data(), staging_buffer_.size());
        if (written != static_cast<ssize_t>(staging_buffer_.size()) || full_fsync(current_fd_) != 0) {
            fatal_error_ = true;
            for (auto* r : pending_requests_) {
                r->failed = true;
                r->done = true;
                r->cv.notify_one();
            }
            pending_requests_.clear();
            std::cerr << "Fatal: Failed to write/fsync staging buffer during rollover. Aborting.\n";
            std::abort();
        }

        total_fsyncs_++;
        total_ops_ += pending_requests_.size();
        for (auto* r : pending_requests_) {
            r->done = true;
            r->failed = false;
            r->cv.notify_one();
        }
        staging_buffer_.clear();
        pending_requests_.clear();
    }

    // Fails every waiting request and aborts. Used when a write/fsync/close fails:
    // a failed fsync is never retried, because the page cache state is unknown after it.
    [[noreturn]] void failAllAndAbortLocked(const std::string& msg) {
        fatal_error_ = true;
        for (auto* r : pending_requests_) {
            r->failed = true;
            r->done = true;
            r->cv.notify_one();
        }
        pending_requests_.clear();
        std::cerr << "Fatal: " << msg << " (errno " << errno << ": " << std::strerror(errno) << "). Aborting.\n";
        std::abort();
    }

    void openNewSegmentLocked() {
        if (current_fd_ >= 0) {
            if (full_fsync(current_fd_) != 0) {
                failAllAndAbortLocked("F_FULLFSYNC failed on old segment during rollover");
            }
            if (close(current_fd_) != 0) {
                failAllAndAbortLocked("close failed on old segment during rollover");
            }
            current_fd_ = -1;
        }

        current_segment_id_++;
        std::string path = getSegmentPath(current_segment_id_);

        // Rule 1: Opened with O_CREAT | O_EXCL (NEVER O_TRUNC)
        current_fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (current_fd_ < 0) {
            fatal_error_ = true;
            for (auto* r : pending_requests_) {
                r->failed = true;
                r->done = true;
                r->cv.notify_one();
            }
            pending_requests_.clear();
            std::cerr << "Fatal: Failed to create segment file (O_CREAT|O_EXCL): " << path << ". Aborting.\n";
            std::abort();
        }

        // Initialize 40-byte SegmentHeader
        SegmentHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.magic = kSegmentMagic;
        hdr.format_version = kFormatVersion;
        hdr.segment_id = current_segment_id_;
        hdr.first_seq = current_seq_ + 1;
        hdr.created_unix_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
        hdr.flags = 0;
        hdr.crc32c = crc32c(&hdr, offsetof(SegmentHeader, crc32c));

        // Pad SegmentHeader up to kBlockSize (4096 bytes) so entries start 4K aligned
        uint8_t block[kBlockSize];
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &hdr, sizeof(hdr));

        ssize_t written = write(current_fd_, block, kBlockSize);
        if (written != static_cast<ssize_t>(kBlockSize) || full_fsync(current_fd_) != 0 || sync_directory(dir_path_) != 0) {
            fatal_error_ = true;
            for (auto* r : pending_requests_) {
                r->failed = true;
                r->done = true;
                r->cv.notify_one();
            }
            pending_requests_.clear();
            std::cerr << "Fatal: Failed to write/sync SegmentHeader or directory on new segment: " << path << ". Aborting.\n";
            std::abort();
        }

        current_offset_ = kBlockSize;
        total_bytes_written_ += kBlockSize;
        total_fsyncs_++;
    }

    // Torn segment creation: a crash between O_CREAT|O_EXCL and the header fsync leaves a short or
    // corrupt last segment, which scanSegment truncates to 0 bytes. Unlink it (and fsync the
    // directory) so it never becomes a 0-byte non-last segment that blocks later restarts.
    // Its id is not reused: the next segment gets id + 1.
    void removeGhostSegmentLocked(uint32_t sid) {
        std::string path = getSegmentPath(sid);
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            throw std::runtime_error("Fatal: stat failed on " + path + ": " + std::strerror(errno));
        }
        if (st.st_size != 0) return;  // only an emptied segment is removed
        if (unlink(path.c_str()) != 0) {
            throw std::runtime_error("Fatal: unlink of empty segment failed on " + path + ": " + std::strerror(errno));
        }
        if (sync_directory(dir_path_) != 0) {
            throw std::runtime_error("Fatal: directory fsync failed after unlinking " + path + ": " + std::strerror(errno));
        }
    }

    // Rule 1: Scan existing segments via VLogReader, recover max seq and max seg, then open NEW segment
    void recoverAndInitLocked() {
        DIR* dir = opendir(dir_path_.c_str());
        if (!dir) {
            throw std::runtime_error("Fatal: opendir failed on " + dir_path_ + ": " + std::strerror(errno));
        }
        std::vector<uint32_t> seg_ids;
        struct dirent* entry;
        errno = 0;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.rfind("vlog_", 0) == 0 && name.size() == 17 && name.substr(13) == ".log") {
                try {
                    uint32_t id = static_cast<uint32_t>(std::stoul(name.substr(5, 8)));
                    seg_ids.push_back(id);
                } catch (...) {}
            }
            errno = 0;
        }
        int readdir_errno = errno;  // readdir returns nullptr on both end-of-dir and error
        if (closedir(dir) != 0) {
            throw std::runtime_error("Fatal: closedir failed on " + dir_path_ + ": " + std::strerror(errno));
        }
        if (readdir_errno != 0) {
            throw std::runtime_error("Fatal: readdir failed on " + dir_path_ + ": " + std::strerror(readdir_errno));
        }

        std::sort(seg_ids.begin(), seg_ids.end());

        uint64_t max_seq = 0;
        uint32_t max_seg = 0;

        if (!seg_ids.empty()) {
            VLogReader reader(dir_path_);
            for (size_t i = 0; i < seg_ids.size(); ++i) {
                uint32_t sid = seg_ids[i];
                bool is_last = (i == seg_ids.size() - 1);
                if (sid > max_seg) {
                    max_seg = sid;
                }
                if (sid < options_.scan_from_segment) continue;  // before the replay point
                uint32_t start = (sid == options_.scan_from_segment) ? options_.scan_from_offset : kBlockSize;
                auto res = reader.scanSegment(sid, is_last, start);
                recovery_entries_scanned_ += res.entries.size();
                for (const auto& e : res.entries) {
                    if (e.header.seq > max_seq) {
                        max_seq = e.header.seq;
                    }
                }
                if (is_last && res.truncated && res.entries.empty()) {
                    removeGhostSegmentLocked(sid);
                }
            }
        }

        current_seq_ = std::max(max_seq, options_.min_seq);
        current_segment_id_ = max_seg;
        openNewSegmentLocked();
    }

public:
    VLogWriter(const std::string& dir_path, Options options = Options{})
        : dir_path_(dir_path), options_(options) {
        if (mkdir(dir_path_.c_str(), 0755) != 0 && errno != EEXIST) {
            throw std::runtime_error("Fatal: mkdir failed on " + dir_path_ + ": " + std::strerror(errno));
        }
        std::unique_lock<std::mutex> lock(mutex_);
        recoverAndInitLocked();
    }

    // A destructor cannot report errors, so a failed fsync/close here aborts.
    ~VLogWriter() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (current_fd_ >= 0) {
            if (!staging_buffer_.empty()) {
                flushStagingBufferLocked();
            }
            if (full_fsync(current_fd_) != 0) {
                failAllAndAbortLocked("F_FULLFSYNC failed on segment close");
            }
            if (close(current_fd_) != 0) {
                failAllAndAbortLocked("close failed on segment close");
            }
            current_fd_ = -1;
        }
    }

    using PayloadGenerator = std::function<void(uint64_t seq, uint8_t* out_buf, size_t len)>;

    // Append an entry using Group Commit with durable F_FULLFSYNC
    AppendResult append(EntryType type, 
                         const std::string& key, 
                         const void* payload_data, 
                         uint32_t payload_len, 
                         uint8_t flags = 0,
                         uint32_t raw_len = 0,
                         const DeltaPrefix* delta_prefix = nullptr,
                         PayloadGenerator payload_fn = nullptr) {

        uint16_t k_len = static_cast<uint16_t>(key.size());
        uint32_t p_len = payload_len + (delta_prefix ? sizeof(DeltaPrefix) : 0);
        uint32_t total_len = entry_size(k_len, p_len);

        // Prepare local entry buffer
        std::vector<uint8_t> entry_buf(total_len, 0);

        EntryHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.total_len = total_len;
        hdr.type = static_cast<uint8_t>(type);
        hdr.flags = flags;
        hdr.key_len = k_len;
        hdr.payload_len = p_len;
        hdr.raw_len = (raw_len > 0) ? raw_len : p_len;
        hdr.reserved = 0;

        // Copy key into entry buffer
        uint32_t k_offset = sizeof(EntryHeader);
        if (k_len > 0) {
            std::memcpy(entry_buf.data() + k_offset, key.data(), k_len);
        }

        // Copy payload if static data provided
        uint32_t p_offset = payload_offset(k_len);
        if (delta_prefix) {
            std::memcpy(entry_buf.data() + p_offset, delta_prefix, sizeof(DeltaPrefix));
            p_offset += sizeof(DeltaPrefix);
        }
        if (payload_data && payload_len > 0) {
            std::memcpy(entry_buf.data() + p_offset, payload_data, payload_len);
        }

        Request req;

        // Lock Acquisition: Assign seq and offset under the same lock (Rule 4)
        std::unique_lock<std::mutex> lock(mutex_);

        if (fatal_error_) {
            throw std::runtime_error("Fatal: VLogWriter in fatal error state");
        }

        // Check hardware write cap
        if (options_.max_bytes > 0 && (total_bytes_written_ + total_len) > options_.max_bytes) {
            fatal_error_ = true;
            throw std::runtime_error("Fatal: Hard write cap exceeded (" + 
                                     std::to_string(total_bytes_written_ + total_len) + " > " + 
                                     std::to_string(options_.max_bytes) + ")");
        }

        // If rollover needed, wait for any in-flight flush, flush staging buffer, then rotate
        if (current_offset_ + total_len > options_.segment_size_limit) {
            while (flush_in_progress_) {
                cv_.wait(lock);
            }
            flushStagingBufferLocked();
            openNewSegmentLocked();
        }

        // Assign sequence and offset under the exact same lock
        current_seq_++;
        hdr.seq = current_seq_;
        uint32_t assigned_offset = current_offset_;
        VPtr assigned_vptr{current_segment_id_, assigned_offset, total_len};

        // If payload generator is provided, derive payload directly from seq
        if (payload_fn && payload_len > 0) {
            payload_fn(hdr.seq, entry_buf.data() + p_offset, payload_len);
        }

        // Compute CRC over [4, total_len)
        std::memcpy(entry_buf.data(), &hdr, sizeof(hdr));
        uint32_t crc = crc32c(entry_buf.data() + 4, total_len - 4);
        std::memcpy(entry_buf.data(), &crc, sizeof(uint32_t));

        // Append to staging buffer
        staging_buffer_.insert(staging_buffer_.end(), entry_buf.begin(), entry_buf.end());
        current_offset_ += total_len;
        total_bytes_written_ += total_len;

        req.vptr = assigned_vptr;
        req.assigned_seq = hdr.seq;
        pending_requests_.push_back(&req);

        // Group Commit Coordination
        if (flush_in_progress_) {
            // Wait for active flush leader
            while (!req.done && !fatal_error_) {
                req.cv.wait(lock);
            }
            if (req.failed || fatal_error_) {
                throw std::runtime_error("Fatal: F_FULLFSYNC batch commit failed");
            }
            return AppendResult{req.vptr, req.assigned_seq};
        }

        // We are the flush leader
        while (!pending_requests_.empty()) {
            flush_in_progress_ = true;

            std::vector<uint8_t> batch_bytes;
            batch_bytes.swap(staging_buffer_);

            std::vector<Request*> batch_reqs;
            batch_reqs.insert(batch_reqs.end(), pending_requests_.begin(), pending_requests_.end());
            pending_requests_.clear();

            int fd = current_fd_;
            lock.unlock();

            // Perform batch write and F_FULLFSYNC outside the lock
            bool ok = false;
            if (!batch_bytes.empty() && fd >= 0) {
                ssize_t written = write(fd, batch_bytes.data(), batch_bytes.size());
                if (written == static_cast<ssize_t>(batch_bytes.size())) {
                    if (full_fsync(fd) == 0) {
                        ok = true;
                    }
                }
            }

            lock.lock();

            if (!ok) {
                // Rule 2: Fatal path: mark ALL pending requests (current batch + pending_requests_) failed, notify each req->cv, then std::abort()
                fatal_error_ = true;
                for (auto* r : batch_reqs) {
                    r->failed = true;
                    r->done = true;
                    r->cv.notify_one();
                }
                for (auto* r : pending_requests_) {
                    r->failed = true;
                    r->done = true;
                    r->cv.notify_one();
                }
                pending_requests_.clear();
                flush_in_progress_ = false;
                cv_.notify_all();
                std::cerr << "Fatal: F_FULLFSYNC or write failed to commit batch. Aborting process.\n";
                std::abort();
            }

            // Success: ack all requests in batch
            total_fsyncs_++;
            total_ops_ += batch_reqs.size();
            for (auto* r : batch_reqs) {
                r->done = true;
                r->failed = false;
                r->cv.notify_one();
            }

            // If new requests arrived while we were fsyncing, loop again as leader
            if (pending_requests_.empty()) {
                flush_in_progress_ = false;
                cv_.notify_all();
                break;
            }
        }

        return AppendResult{req.vptr, req.assigned_seq};
    }

    uint64_t getCurrentSeq() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return current_seq_;
    }

    uint64_t getTotalOps() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return total_ops_;
    }

    uint64_t getTotalFsyncs() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return total_fsyncs_;
    }

    uint64_t getTotalBytesWritten() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return total_bytes_written_;
    }

    double getAverageBatchSize() const {
        std::unique_lock<std::mutex> lock(mutex_);
        if (total_fsyncs_ == 0) return 0.0;
        return static_cast<double>(total_ops_) / total_fsyncs_;
    }

    // Entries read by the tail discovery scan in the constructor.
    uint64_t getRecoveryEntriesScanned() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return recovery_entries_scanned_;
    }

    uint32_t getCurrentSegmentId() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return current_segment_id_;
    }

    std::string getDirectoryPath() const {
        return dir_path_;
    }
};

} // namespace vlog
