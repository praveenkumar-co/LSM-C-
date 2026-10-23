#pragma once
// Pager.hpp - fixed-size page cache over index.db. See docs/PHASE2_PLAN.md sections 1.6 and 1.7.
//
// - N frames, LRU eviction of unpinned frames, all frame state behind one std::mutex.
// - Load: pread 4096 bytes, then check CRC/magic/page_id/type. Any failure aborts: after a
//   clean open the meta page already says clean_shutdown = 0, so the next open rebuilds.
// - Write-back: recompute CRC, pwrite 4096 bytes. No fsync on eviction (the file is only
//   trusted after a clean close).
// - Every syscall return value is checked. A failed fsync aborts and is never retried.

#include "index/BPTreeFormat.hpp"
#include "vlog/VLogReader.hpp"  // vlog::full_fsync

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bpt {

[[noreturn]] inline void fatal(const std::string& msg) {
    int e = errno;
    if (e != 0) {
        std::fprintf(stderr, "Fatal (index): %s (errno %d: %s). Aborting.\n", msg.c_str(), e, std::strerror(e));
    } else {
        std::fprintf(stderr, "Fatal (index): %s. Aborting.\n", msg.c_str());
    }
    std::abort();
}

enum class OpenStatus { Ok, Missing, BadMeta, NotClean, SizeMismatch };

inline const char* toString(OpenStatus s) {
    switch (s) {
        case OpenStatus::Ok:           return "Ok";
        case OpenStatus::Missing:      return "Missing";
        case OpenStatus::BadMeta:      return "BadMeta";
        case OpenStatus::NotClean:     return "NotClean";
        case OpenStatus::SizeMismatch: return "SizeMismatch";
    }
    return "?";
}

class Pager;

// RAII pin on one frame. A pinned frame is never evicted.
class PageGuard {
public:
    PageGuard() = default;
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&& o) noexcept { moveFrom(o); }
    PageGuard& operator=(PageGuard&& o) noexcept {
        if (this != &o) { release(); moveFrom(o); }
        return *this;
    }
    ~PageGuard() { release(); }

    uint8_t* data() const { return data_; }
    page_id_t id() const { return id_; }
    bool valid() const { return pager_ != nullptr; }
    inline void markDirty();
    inline void release();

private:
    friend class Pager;
    PageGuard(Pager* p, uint32_t frame, uint8_t* data, page_id_t id)
        : pager_(p), frame_(frame), data_(data), id_(id) {}
    void moveFrom(PageGuard& o) {
        pager_ = o.pager_; frame_ = o.frame_; data_ = o.data_; id_ = o.id_;
        o.pager_ = nullptr; o.data_ = nullptr; o.id_ = kNoPage;
    }
    Pager* pager_{nullptr};
    uint32_t frame_{0};
    uint8_t* data_{nullptr};
    page_id_t id_{kNoPage};
};

class Pager {
public:
    static constexpr size_t kMinFrames = 16;

    struct Options {
        size_t num_frames = 1024;                  // 4 MiB of cache
        std::function<void(uint64_t)> on_write;    // called with 4096 after every page pwrite (tests count bytes)
    };

    struct Stats {
        uint64_t page_reads = 0;
        uint64_t page_writes = 0;  // includes meta page writes
        uint64_t evictions = 0;
        uint64_t fsyncs = 0;
    };

    // Creates a new file (O_CREAT | O_EXCL): meta page (clean_shutdown = 0) and an empty root leaf.
    // The meta page is written and fsynced before returning; the root leaf is written at flush/close.
    static std::unique_ptr<Pager> create(const std::string& path, Options opts) {
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd < 0) fatal("open(O_CREAT|O_EXCL) failed for " + path);
        std::unique_ptr<Pager> p(new Pager(path, fd, opts));
        p->meta_ = MetaBody{};
        p->meta_.magic = kMetaMagic;
        p->meta_.format_version = kIndexFormatVersion;
        p->meta_.page_size = kPageSize;
        p->meta_.page_count = 1;  // page 0 (meta)
        p->meta_.tree_height = 1;
        p->meta_.clean_shutdown = 0;
        p->meta_.open_count = 1;
        {
            PageGuard root = p->allocate(PageType::Leaf, 0);
            p->meta_.root_page_id = root.id();
        }
        p->writeMetaLocked();
        p->fsyncOrDie("F_FULLFSYNC after create of " + path);
        return p;
    }

    // Opens an existing file for use. Returns nullptr (and sets *status) if the file is missing
    // or not trusted. On success the meta page is rewritten with clean_shutdown = 0 and fsynced
    // before any page can be modified.
    static std::unique_ptr<Pager> open(const std::string& path, Options opts, OpenStatus* status) {
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0) {
            if (errno == ENOENT) { *status = OpenStatus::Missing; return nullptr; }
            fatal("open failed for " + path);
        }
        MetaBody m{};
        OpenStatus st = readAndCheckMeta(fd, &m);
        if (st != OpenStatus::Ok) {
            if (::close(fd) != 0) fatal("close failed for " + path);
            *status = st;
            return nullptr;
        }
        std::unique_ptr<Pager> p(new Pager(path, fd, opts));
        p->meta_ = m;
        p->meta_.clean_shutdown = 0;
        p->meta_.open_count++;
        p->writeMetaLocked();
        p->fsyncOrDie("F_FULLFSYNC after open of " + path);
        *status = OpenStatus::Ok;
        return p;
    }

    // Reads and checks the meta page of a file without opening it for use (tests, diagnostics).
    static OpenStatus inspect(const std::string& path, MetaBody* out) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) return OpenStatus::Missing;
            fatal("open failed for " + path);
        }
        OpenStatus st = readAndCheckMeta(fd, out);
        if (::close(fd) != 0) fatal("close failed for " + path);
        return st;
    }

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    // Without closeClean(), the file keeps clean_shutdown = 0 on disk, so the next open rebuilds.
    ~Pager() {
        if (fd_ >= 0 && ::close(fd_) != 0) fatal("close failed for " + path_);
    }

    PageGuard fetch(page_id_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        if (id == kMetaPageId || id >= meta_.page_count) {
            fatal("fetch of invalid page id " + std::to_string(id) + " (page_count " +
                  std::to_string(meta_.page_count) + ")");
        }
        auto it = table_.find(id);
        if (it != table_.end()) {
            uint32_t f = it->second;
            pinLocked(f);
            return PageGuard(this, f, frameData(f), id);
        }
        uint32_t f = takeVictimLocked();
        uint8_t* buf = frameData(f);
        ssize_t r = ::pread(fd_, buf, kPageSize, static_cast<off_t>(id) * kPageSize);
        if (r != static_cast<ssize_t>(kPageSize)) {
            fatal("pread of page " + std::to_string(id) + " returned " + std::to_string(r));
        }
        std::string why;
        if (!checkPage(buf, id, &why)) {
            errno = 0;
            fatal("index.db page check failed: " + why);
        }
        stats_.page_reads++;
        installLocked(f, id, /*dirty=*/false);
        return PageGuard(this, f, buf, id);
    }

    // New page: id = page_count++, zeroed frame with an empty header, pinned and dirty.
    PageGuard allocate(PageType type, uint8_t level) {
        std::lock_guard<std::mutex> lk(mu_);
        if (meta_.page_count == UINT32_MAX) fatal("index.db page id space exhausted");
        page_id_t id = meta_.page_count++;
        uint32_t f = takeVictimLocked();
        initPage(frameData(f), id, type, level);
        installLocked(f, id, /*dirty=*/true);
        return PageGuard(this, f, frameData(f), id);
    }

    // Writes back every dirty frame (no fsync).
    void flushDirty() {
        std::lock_guard<std::mutex> lk(mu_);
        flushDirtyLocked();
    }

    // Clean close (plan 1.7): write back dirty frames, fsync, write meta with clean_shutdown = 1,
    // fsync, close. `after_flush` runs between the first fsync and the meta write (crash tests).
    void closeClean(const std::function<void()>& after_flush = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ < 0) fatal("closeClean on a closed pager");
        for (const Frame& fr : frames_) {
            if (fr.pins != 0) fatal("closeClean with a pinned page " + std::to_string(fr.page_id));
        }
        flushDirtyLocked();
        fsyncOrDie("F_FULLFSYNC of pages during clean close of " + path_);
        if (after_flush) after_flush();
        meta_.clean_shutdown = 1;
        writeMetaLocked();
        fsyncOrDie("F_FULLFSYNC of meta page during clean close of " + path_);
        if (::close(fd_) != 0) fatal("close failed for " + path_);
        fd_ = -1;
    }

    // Test helper: writes back dirty frames, then preads every page and runs checkPage on it.
    // Returns "" if all pages pass, else the first problem.
    std::string verifyPagesOnDisk() {
        std::lock_guard<std::mutex> lk(mu_);
        flushDirtyLocked();
        std::vector<uint8_t> buf(kPageSize);
        for (page_id_t id = 0; id < meta_.page_count; ++id) {
            ssize_t r = ::pread(fd_, buf.data(), kPageSize, static_cast<off_t>(id) * kPageSize);
            if (r != static_cast<ssize_t>(kPageSize)) return "short pread of page " + std::to_string(id);
            std::string why;
            if (!checkPage(buf.data(), id, &why)) return why;
        }
        return "";
    }

    // Meta fields. The tree changes root/height/key_count/max_applied_seq under its exclusive lock.
    page_id_t rootPageId() const { return meta_.root_page_id; }
    uint32_t treeHeight() const { return meta_.tree_height; }
    uint64_t keyCount() const { return meta_.key_count; }
    uint64_t maxAppliedSeq() const { return meta_.max_applied_seq; }
    uint64_t openCount() const { return meta_.open_count; }
    uint32_t pageCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return meta_.page_count;
    }
    void setRoot(page_id_t root, uint32_t height) { meta_.root_page_id = root; meta_.tree_height = height; }
    void setKeyCount(uint64_t n) { meta_.key_count = n; }
    void setMaxAppliedSeq(uint64_t s) { meta_.max_applied_seq = s; }

    Stats stats() const {
        std::lock_guard<std::mutex> lk(mu_);
        return stats_;
    }
    size_t numFrames() const { return frames_.size(); }
    const std::string& path() const { return path_; }

private:
    friend class PageGuard;

    struct Frame {
        page_id_t page_id{kNoPage};
        uint32_t pins{0};
        bool dirty{false};
        bool in_lru{false};
        std::list<uint32_t>::iterator lru_it;
    };

    Pager(const std::string& path, int fd, const Options& opts)
        : path_(path), fd_(fd), opts_(opts) {
        if (opts_.num_frames < kMinFrames) {
            errno = 0;
            fatal("Pager needs at least " + std::to_string(kMinFrames) + " frames");
        }
        frames_.resize(opts_.num_frames);
        buffer_.reset(new uint8_t[opts_.num_frames * kPageSize]);
        for (size_t i = opts_.num_frames; i-- > 0;) free_frames_.push_back(static_cast<uint32_t>(i));
    }

    // Validates the meta page and the file size. Does not modify the file.
    static OpenStatus readAndCheckMeta(int fd, MetaBody* out) {
        uint8_t page[kPageSize];
        ssize_t r = ::pread(fd, page, kPageSize, 0);
        if (r < 0) fatal("pread of meta page failed");
        if (r != static_cast<ssize_t>(kPageSize)) return OpenStatus::BadMeta;
        if (!checkPage(page, kMetaPageId, nullptr)) return OpenStatus::BadMeta;
        MetaBody m;
        std::memcpy(&m, page + kHeaderSize, sizeof(m));
        if (m.magic != kMetaMagic || m.format_version != kIndexFormatVersion || m.page_size != kPageSize ||
            m.page_count < 2 || m.root_page_id == kNoPage || m.root_page_id >= m.page_count ||
            m.tree_height == 0) {
            return OpenStatus::BadMeta;
        }
        *out = m;
        if (m.clean_shutdown != 1) return OpenStatus::NotClean;
        struct stat st;
        if (::fstat(fd, &st) != 0) fatal("fstat of index file failed");
        if (static_cast<uint64_t>(st.st_size) != static_cast<uint64_t>(m.page_count) * kPageSize) {
            return OpenStatus::SizeMismatch;
        }
        return OpenStatus::Ok;
    }

    uint8_t* frameData(uint32_t f) const { return buffer_.get() + static_cast<size_t>(f) * kPageSize; }

    void fsyncOrDie(const std::string& what) {
        if (vlog::full_fsync(fd_) != 0) fatal(what);  // never retried
        stats_.fsyncs++;
    }

    void writeMetaLocked() {
        uint8_t page[kPageSize];
        initPage(page, kMetaPageId, PageType::Meta, 0);
        std::memcpy(page + kHeaderSize, &meta_, sizeof(meta_));
        uint32_t crc = computePageCrc(page);
        std::memcpy(page, &crc, sizeof(crc));
        writePageBytesLocked(page, kMetaPageId);
    }

    void writePageBytesLocked(const uint8_t* page, page_id_t id) {
        ssize_t w = ::pwrite(fd_, page, kPageSize, static_cast<off_t>(id) * kPageSize);
        if (w != static_cast<ssize_t>(kPageSize)) {
            fatal("pwrite of page " + std::to_string(id) + " returned " + std::to_string(w));
        }
        stats_.page_writes++;
        if (opts_.on_write) opts_.on_write(kPageSize);
    }

    void writeBackLocked(uint32_t f) {
        uint8_t* buf = frameData(f);
        uint32_t crc = computePageCrc(buf);
        std::memcpy(buf, &crc, sizeof(crc));
        writePageBytesLocked(buf, frames_[f].page_id);
        frames_[f].dirty = false;
    }

    void flushDirtyLocked() {
        for (uint32_t f = 0; f < frames_.size(); ++f) {
            if (frames_[f].page_id != kNoPage && frames_[f].dirty) writeBackLocked(f);
        }
    }

    void pinLocked(uint32_t f) {
        Frame& fr = frames_[f];
        if (fr.in_lru) { lru_.erase(fr.lru_it); fr.in_lru = false; }
        fr.pins++;
    }

    void unpin(uint32_t f) {
        std::lock_guard<std::mutex> lk(mu_);
        Frame& fr = frames_[f];
        if (fr.pins == 0) fatal("unpin of an unpinned frame");
        if (--fr.pins == 0) {
            lru_.push_front(f);
            fr.lru_it = lru_.begin();
            fr.in_lru = true;
        }
    }

    void markDirty(uint32_t f) {
        std::lock_guard<std::mutex> lk(mu_);
        frames_[f].dirty = true;
    }

    // Returns an unmapped frame: a free one, else the least recently used unpinned one
    // (written back first if dirty).
    uint32_t takeVictimLocked() {
        if (!free_frames_.empty()) {
            uint32_t f = free_frames_.back();
            free_frames_.pop_back();
            return f;
        }
        if (lru_.empty()) {
            errno = 0;
            fatal("all " + std::to_string(frames_.size()) + " page frames are pinned");
        }
        uint32_t f = lru_.back();
        lru_.pop_back();
        Frame& fr = frames_[f];
        fr.in_lru = false;
        if (fr.dirty) writeBackLocked(f);
        table_.erase(fr.page_id);
        fr.page_id = kNoPage;
        stats_.evictions++;
        return f;
    }

    void installLocked(uint32_t f, page_id_t id, bool dirty) {
        Frame& fr = frames_[f];
        fr.page_id = id;
        fr.pins = 1;
        fr.dirty = dirty;
        fr.in_lru = false;
        table_[id] = f;
    }

    std::string path_;
    int fd_{-1};
    Options opts_;
    MetaBody meta_{};

    mutable std::mutex mu_;
    std::vector<Frame> frames_;
    std::unique_ptr<uint8_t[]> buffer_;
    std::vector<uint32_t> free_frames_;
    std::list<uint32_t> lru_;  // front = most recently unpinned
    std::unordered_map<page_id_t, uint32_t> table_;
    Stats stats_;
};

inline void PageGuard::markDirty() { pager_->markDirty(frame_); }

inline void PageGuard::release() {
    if (pager_) {
        pager_->unpin(frame_);
        pager_ = nullptr;
        data_ = nullptr;
        id_ = kNoPage;
    }
}

} // namespace bpt
