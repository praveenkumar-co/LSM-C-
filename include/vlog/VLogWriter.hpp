#pragma once
// VLogWriter.hpp - appends entries to the value log with group commit. One thread at a time is
// the flush leader: it writes everything staged so far with one write() and one F_FULLFSYNC,
// then acks every request in that batch. append() returns only after its entry is durable.
// Startup scans existing segments (from an optional start point) to find the highest seq, then
// always opens a new segment. appendRelocated writes GC copies that keep their original seq.

#include "CRC32.hpp"
#include "VLogFormat.hpp"
#include "VLogReader.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vlog {

// fsync of a directory, so a created, renamed or unlinked file name is durable.
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
        uint32_t segment_size_limit; // roll over to a new segment beyond this many bytes
        uint64_t max_bytes;          // 0 = no cap; appends past the cap throw
        // Startup scan start (the index superblock's replay point). Segments below
        // scan_from_segment are not scanned; that segment is scanned from scan_from_offset.
        // 0 = scan every segment.
        uint32_t scan_from_segment = 0;
        uint32_t scan_from_offset = kBlockSize;
        uint64_t min_seq = 0; // seqs up to this were used before the scan start (checkpoint seq)

        Options(uint32_t seg_limit = 64 * 1024 * 1024, uint64_t max_b = 0)
            : segment_size_limit(seg_limit), max_bytes(max_b) {}
    };

    using PayloadGenerator = std::function<void(uint64_t seq, uint8_t* out_buf, size_t len)>;

    VLogWriter(const std::string& dir_path, Options options = Options{})
        : dir_path_(dir_path), options_(options) {
        if (mkdir(dir_path_.c_str(), 0755) != 0 && errno != EEXIST) {
            throw std::runtime_error("Fatal: mkdir failed on " + dir_path_ + ": " +
                                     std::strerror(errno));
        }
        std::unique_lock<std::mutex> lock(mutex_);
        recoverAndInitLocked();
    }

    // A destructor cannot report errors, so a failed fsync/close here aborts.
    ~VLogWriter() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (current_fd_ < 0) return;
        if (!staging_buffer_.empty()) flushStagingBufferLocked();
        if (full_fsync(current_fd_) != 0)
            failAllAndAbortLocked("F_FULLFSYNC failed on segment close");
        if (close(current_fd_) != 0) failAllAndAbortLocked("close failed on segment close");
        current_fd_ = -1;
    }

    // Returns after the entry is written and F_FULLFSYNCed. payload_fn, if given, fills the
    // payload once the seq is known (tests derive payload bytes from the seq).
    AppendResult append(EntryType type, const std::string& key, const void* payload_data,
                        uint32_t payload_len, uint8_t flags = 0, uint32_t raw_len = 0,
                        const DeltaPrefix* delta_prefix = nullptr,
                        PayloadGenerator payload_fn = nullptr) {
        const uint16_t k_len = static_cast<uint16_t>(key.size());
        const uint32_t p_len = payload_len + (delta_prefix ? sizeof(DeltaPrefix) : 0);
        const uint32_t total_len = entry_size(k_len, p_len);
        std::vector<uint8_t> entry_buf(total_len, 0); // zero padding is covered by the CRC

        EntryHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.total_len = total_len;
        hdr.type = static_cast<uint8_t>(type);
        hdr.flags = flags;
        hdr.key_len = k_len;
        hdr.payload_len = p_len;
        hdr.raw_len = (raw_len > 0) ? raw_len : p_len;
        if (k_len > 0) std::memcpy(entry_buf.data() + sizeof(EntryHeader), key.data(), k_len);
        uint32_t p_offset = payload_offset(k_len);
        if (delta_prefix) {
            std::memcpy(entry_buf.data() + p_offset, delta_prefix, sizeof(DeltaPrefix));
            p_offset += sizeof(DeltaPrefix);
        }
        if (payload_data && payload_len > 0)
            std::memcpy(entry_buf.data() + p_offset, payload_data, payload_len);

        Request req;
        std::unique_lock<std::mutex> lock(mutex_);
        if (fatal_error_) throw std::runtime_error("Fatal: VLogWriter in fatal error state");
        if (options_.max_bytes > 0 && total_bytes_written_ + total_len > options_.max_bytes) {
            fatal_error_ = true;
            throw std::runtime_error("Fatal: Hard write cap exceeded (" +
                                     std::to_string(total_bytes_written_ + total_len) + " > " +
                                     std::to_string(options_.max_bytes) + ")");
        }
        rollOverIfNeededLocked(lock, total_len);
        // seq and offset are assigned under the same lock, so file order == seq order for every
        // entry without kFlagRelocated (appendRelocated keeps a GC copy's original seq).
        current_seq_++;
        hdr.seq = current_seq_;
        const VPtr assigned_vptr{current_segment_id_, current_offset_, total_len};
        if (payload_fn && payload_len > 0)
            payload_fn(hdr.seq, entry_buf.data() + p_offset, payload_len);
        std::memcpy(entry_buf.data(), &hdr, sizeof(hdr));
        const uint32_t crc = crc32c(entry_buf.data() + 4, total_len - 4);
        std::memcpy(entry_buf.data(), &crc, sizeof(crc));

        staging_buffer_.insert(staging_buffer_.end(), entry_buf.begin(), entry_buf.end());
        current_offset_ += total_len;
        total_bytes_written_ += total_len;
        req.vptr = assigned_vptr;
        req.assigned_seq = hdr.seq;
        pending_requests_.push_back(&req);
        commitLocked(lock, req);
        return AppendResult{req.vptr, req.assigned_seq};
    }

    // A GC copy of an existing version (see appendRelocated).
    struct RelocatedEntry {
        EntryType type;
        std::string key;
        uint64_t seq;     // the original version's seq, not a new one
        uint8_t flags;    // must include kFlagRelocated
        uint32_t raw_len; // 0 = payload size
        std::vector<uint8_t> payload;
    };

    // Appends GC copies that keep their original seq. current_seq_ does not move, so entries with
    // kFlagRelocated are the only ones whose seq can be lower than an entry before them in the
    // file. The batch shares one write + F_FULLFSYNC with the group commit (a rollover inside the
    // batch adds one). Returns one VPtr per entry once all of them are durable. Throws
    // std::invalid_argument, writing nothing, if an entry lacks kFlagRelocated, has seq 0 or a seq
    // above the last assigned one, an empty or oversize key, or is longer than kMaxEntryLen.
    std::vector<VPtr> appendRelocated(const std::vector<RelocatedEntry>& batch) {
        std::vector<std::vector<uint8_t>> bufs;
        bufs.reserve(batch.size());
        uint64_t batch_bytes = 0;
        for (const RelocatedEntry& r : batch) {
            if (!(r.flags & kFlagRelocated))
                throw std::invalid_argument("appendRelocated: entry without kFlagRelocated");
            if (r.seq == 0) throw std::invalid_argument("appendRelocated: seq 0");
            if (r.key.empty() || r.key.size() > UINT16_MAX)
                throw std::invalid_argument("appendRelocated: bad key length");
            const uint64_t total = entry_size(static_cast<uint16_t>(r.key.size()),
                                              static_cast<uint32_t>(r.payload.size()));
            if (r.payload.size() > kMaxEntryLen || total > kMaxEntryLen)
                throw std::invalid_argument("appendRelocated: entry longer than kMaxEntryLen");
            const uint16_t k_len = static_cast<uint16_t>(r.key.size());
            const uint32_t p_len = static_cast<uint32_t>(r.payload.size());
            std::vector<uint8_t> buf(total, 0);
            EntryHeader hdr;
            std::memset(&hdr, 0, sizeof(hdr));
            hdr.total_len = static_cast<uint32_t>(total);
            hdr.seq = r.seq;
            hdr.type = static_cast<uint8_t>(r.type);
            hdr.flags = r.flags;
            hdr.key_len = k_len;
            hdr.payload_len = p_len;
            hdr.raw_len = r.raw_len > 0 ? r.raw_len : p_len;
            std::memcpy(buf.data() + sizeof(EntryHeader), r.key.data(), k_len);
            if (p_len > 0) std::memcpy(buf.data() + payload_offset(k_len), r.payload.data(), p_len);
            std::memcpy(buf.data(), &hdr, sizeof(hdr));
            const uint32_t crc = crc32c(buf.data() + 4, buf.size() - 4);
            std::memcpy(buf.data(), &crc, sizeof(crc));
            batch_bytes += total;
            bufs.push_back(std::move(buf));
        }
        std::vector<VPtr> out;
        if (batch.empty()) return out;
        out.reserve(batch.size());

        Request req;
        std::unique_lock<std::mutex> lock(mutex_);
        if (fatal_error_) throw std::runtime_error("Fatal: VLogWriter in fatal error state");
        for (const RelocatedEntry& r : batch)
            if (r.seq > current_seq_)
                throw std::invalid_argument("appendRelocated: seq " + std::to_string(r.seq) +
                                            " above the last assigned seq " +
                                            std::to_string(current_seq_));
        if (options_.max_bytes > 0 && total_bytes_written_ + batch_bytes > options_.max_bytes) {
            fatal_error_ = true;
            throw std::runtime_error("Fatal: Hard write cap exceeded (" +
                                     std::to_string(total_bytes_written_ + batch_bytes) + " > " +
                                     std::to_string(options_.max_bytes) + ")");
        }
        for (const std::vector<uint8_t>& buf : bufs) {
            const uint32_t total_len = static_cast<uint32_t>(buf.size());
            rollOverIfNeededLocked(lock, total_len);
            out.push_back(VPtr{current_segment_id_, current_offset_, total_len});
            staging_buffer_.insert(staging_buffer_.end(), buf.begin(), buf.end());
            current_offset_ += total_len;
            total_bytes_written_ += total_len;
        }
        pending_requests_.push_back(&req);
        commitLocked(lock, req);
        return out;
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
        return total_fsyncs_ == 0 ? 0.0 : static_cast<double>(total_ops_) / total_fsyncs_;
    }

    // Entries read by the startup scan.
    uint64_t getRecoveryEntriesScanned() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return recovery_entries_scanned_;
    }

    uint32_t getCurrentSegmentId() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return current_segment_id_;
    }

private:
    struct Request {
        VPtr vptr{0, 0, 0};
        uint64_t assigned_seq{0};
        bool done{false};
        bool failed{false};
        std::condition_variable cv;
    };

    // Rolls over to a new segment if an entry of total_len bytes does not fit in the current one.
    // An in-flight flush must land in the old segment before it is closed, so this waits for it
    // first, then checks again: another thread may have rolled over while this one waited, and
    // then the entry fits in that new segment (without the re-check, every waiting thread would
    // open one more, almost empty segment).
    void rollOverIfNeededLocked(std::unique_lock<std::mutex>& lock, uint32_t total_len) {
        if (current_offset_ + total_len <= options_.segment_size_limit) return;
        while (flush_in_progress_) cv_.wait(lock);
        if (current_offset_ + total_len <= options_.segment_size_limit) return;
        flushStagingBufferLocked();
        openNewSegmentLocked();
    }

    // Waits until req is durable: as a follower while another thread leads, or as the leader,
    // which flushes batches until no request is waiting. Called with req already queued.
    void commitLocked(std::unique_lock<std::mutex>& lock, Request& req) {
        if (flush_in_progress_) {
            // A leader is writing; it (or the next leader) will include this request.
            while (!req.done && !fatal_error_) req.cv.wait(lock);
            if (req.failed || fatal_error_)
                throw std::runtime_error("Fatal: F_FULLFSYNC batch commit failed");
            return;
        }

        // This thread is the leader: flush batches until no request is waiting.
        while (!pending_requests_.empty()) {
            flush_in_progress_ = true;
            std::vector<uint8_t> batch_bytes;
            batch_bytes.swap(staging_buffer_);
            std::vector<Request*> batch(pending_requests_.begin(), pending_requests_.end());
            pending_requests_.clear();
            const int fd = current_fd_;
            lock.unlock(); // other threads keep staging entries during the write + fsync
            bool ok = false;
            if (!batch_bytes.empty() && fd >= 0) {
                ok = write(fd, batch_bytes.data(), batch_bytes.size()) ==
                         static_cast<ssize_t>(batch_bytes.size()) &&
                     full_fsync(fd) == 0;
            }
            lock.lock();
            if (!ok) {
                // Never retried: after a failed fsync the page cache state is unknown.
                for (Request* r : batch) pending_requests_.push_back(r);
                failAllAndAbortLocked("write or F_FULLFSYNC failed for a commit batch");
            }
            total_fsyncs_++;
            total_ops_ += batch.size();
            for (Request* r : batch) {
                r->done = true;
                r->failed = false;
                r->cv.notify_one();
            }
            if (pending_requests_.empty()) {
                flush_in_progress_ = false;
                cv_.notify_all();
                break;
            }
        }
    }

    std::string getSegmentPath(uint32_t seg_id) const {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/vlog_%08u.log", seg_id);
        return dir_path_ + buf;
    }

    // Fails every waiting request and aborts. A failed write/fsync/close is never retried.
    [[noreturn]] void failAllAndAbortLocked(const std::string& msg) {
        const int e = errno;
        fatal_error_ = true;
        for (Request* r : pending_requests_) {
            r->failed = true;
            r->done = true;
            r->cv.notify_one();
        }
        pending_requests_.clear();
        std::cerr << "Fatal: " << msg << " (errno " << e << ": " << std::strerror(e)
                  << "). Aborting.\n";
        std::abort();
    }

    // Writes and fsyncs the staged entries synchronously (rollover and close).
    void flushStagingBufferLocked() {
        if (staging_buffer_.empty() || current_fd_ < 0) return;
        if (write(current_fd_, staging_buffer_.data(), staging_buffer_.size()) !=
                static_cast<ssize_t>(staging_buffer_.size()) ||
            full_fsync(current_fd_) != 0) {
            failAllAndAbortLocked("write or F_FULLFSYNC failed for the staging buffer");
        }
        total_fsyncs_++;
        total_ops_ += pending_requests_.size();
        for (Request* r : pending_requests_) {
            r->done = true;
            r->failed = false;
            r->cv.notify_one();
        }
        staging_buffer_.clear();
        pending_requests_.clear();
    }

    // Opens the next segment with O_CREAT | O_EXCL (never O_TRUNC: an existing file is never
    // overwritten). The header block is fsynced, then the directory, before any entry is written.
    void openNewSegmentLocked() {
        if (current_fd_ >= 0) {
            if (full_fsync(current_fd_) != 0)
                failAllAndAbortLocked("F_FULLFSYNC failed on old segment during rollover");
            if (close(current_fd_) != 0)
                failAllAndAbortLocked("close failed on old segment during rollover");
            current_fd_ = -1;
        }
        current_segment_id_++;
        const std::string path = getSegmentPath(current_segment_id_);
        current_fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (current_fd_ < 0) failAllAndAbortLocked("open(O_CREAT|O_EXCL) failed for " + path);

        SegmentHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.magic = kSegmentMagic;
        hdr.format_version = kFormatVersion;
        hdr.segment_id = current_segment_id_;
        hdr.first_seq = current_seq_ + 1;
        hdr.created_unix_ns =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
        hdr.flags = 0;
        hdr.crc32c = crc32c(&hdr, offsetof(SegmentHeader, crc32c));
        // The header is padded to kBlockSize, so entries start 4 KiB aligned.
        uint8_t block[kBlockSize];
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &hdr, sizeof(hdr));
        if (write(current_fd_, block, kBlockSize) != static_cast<ssize_t>(kBlockSize) ||
            full_fsync(current_fd_) != 0 || sync_directory(dir_path_) != 0) {
            failAllAndAbortLocked("writing or syncing the header of new segment " + path +
                                  " failed");
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
        const std::string path = getSegmentPath(sid);
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            throw std::runtime_error("Fatal: stat failed on " + path + ": " + std::strerror(errno));
        }
        if (st.st_size != 0) return;
        if (unlink(path.c_str()) != 0) {
            throw std::runtime_error("Fatal: unlink of empty segment failed on " + path + ": " +
                                     std::strerror(errno));
        }
        if (sync_directory(dir_path_) != 0) {
            throw std::runtime_error("Fatal: directory fsync failed after unlinking " + path +
                                     ": " + std::strerror(errno));
        }
    }

    // Finds the highest seq and segment id, truncating a torn tail of the last segment (via
    // VLogReader), then opens a new segment. Existing segments are never appended to.
    void recoverAndInitLocked() {
        DIR* dir = opendir(dir_path_.c_str());
        if (!dir)
            throw std::runtime_error("Fatal: opendir failed on " + dir_path_ + ": " +
                                     std::strerror(errno));
        std::vector<uint32_t> seg_ids;
        errno = 0;
        while (struct dirent* entry = readdir(dir)) {
            std::string name = entry->d_name;
            if (name.rfind("vlog_", 0) == 0 && name.size() == 17 && name.substr(13) == ".log") {
                try {
                    seg_ids.push_back(static_cast<uint32_t>(std::stoul(name.substr(5, 8))));
                } catch (...) {
                    // not a segment name after all
                }
            }
            errno = 0;
        }
        const int readdir_errno = errno; // readdir returns nullptr on both end-of-dir and error
        if (closedir(dir) != 0)
            throw std::runtime_error("Fatal: closedir failed on " + dir_path_ + ": " +
                                     std::strerror(errno));
        if (readdir_errno != 0) {
            throw std::runtime_error("Fatal: readdir failed on " + dir_path_ + ": " +
                                     std::strerror(readdir_errno));
        }
        std::sort(seg_ids.begin(), seg_ids.end());

        uint64_t max_seq = 0;
        uint32_t max_seg = 0;
        VLogReader reader(dir_path_);
        for (size_t i = 0; i < seg_ids.size(); ++i) {
            const uint32_t sid = seg_ids[i];
            const bool is_last = i + 1 == seg_ids.size();
            max_seg = std::max(max_seg, sid);
            if (sid < options_.scan_from_segment) continue; // before the replay point
            const uint32_t start =
                (sid == options_.scan_from_segment) ? options_.scan_from_offset : kBlockSize;
            ScanResult res = reader.scanSegment(sid, is_last, start);
            recovery_entries_scanned_ += res.entries.size();
            for (const RecoveredEntry& e : res.entries) {
                const uint64_t seq =
                    e.header.seq; // EntryHeader is packed: copy, never bind a reference
                max_seq = std::max(max_seq, seq);
            }
            if (is_last && res.truncated && res.entries.empty()) removeGhostSegmentLocked(sid);
        }
        current_seq_ = std::max(max_seq, options_.min_seq);
        current_segment_id_ = max_seg;
        openNewSegmentLocked();
    }

    std::string dir_path_;
    Options options_;

    mutable std::mutex mutex_;
    std::condition_variable cv_; // signalled when a leader finishes (rollover waits on it)

    uint32_t current_segment_id_{0};
    uint64_t current_seq_{0};
    uint32_t current_offset_{0};
    int current_fd_{-1};

    uint64_t total_bytes_written_{0};
    uint64_t total_ops_{0};
    uint64_t total_fsyncs_{0};
    uint64_t recovery_entries_scanned_{0};
    bool fatal_error_{false};

    std::vector<uint8_t> staging_buffer_;
    std::deque<Request*> pending_requests_;
    bool flush_in_progress_{false};
};

} // namespace vlog
