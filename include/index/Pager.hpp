#pragma once
// Pager.hpp - fixed-size page cache over index.db.
//
// - N frames, LRU eviction of unpinned frames, all frame state behind one std::mutex.
// - Page 0 is a static header (magic, format version, page size, random nonce), written and
//   fsynced once at creation and never modified.
// - Tree state (root, height, key count, page count) is kept in memory. It becomes durable only
//   through checkpoint(): write back dirty frames, F_FULLFSYNC index.db, then commit a
//   superblock that names the root and page count (SuperblockManager.hpp).
// - Copy-on-write: pages with id < checkpoint_page_count belong to a committed tree and are
//   frozen; marking one dirty aborts. The tree copies a frozen page before changing it
//   (BPTree::cowPath). Pages allocated in the current epoch (new ids, or ids taken from the free
//   list) are modified in place.
// - Page reuse: a frozen page freed while the last committed epoch is E stays reserved until
//   epoch E + 2 commits. Until then one of the two superblock slots can still name a tree that
//   uses it. At open the free list is rebuilt by walking the committed trees (IndexRecovery.hpp).
// - Load: pread 4096 bytes, then check CRC/magic/page_id/type. Any failure aborts.
// - Write-back: recompute CRC, pwrite 4096 bytes. No fsync on eviction.
// - Every syscall return value is checked. A failed fsync aborts and is never retried.

#include "index/BPTreeFormat.hpp"
#include "index/IndexCommon.hpp"
#include "index/SuperblockManager.hpp"
#include "vlog/VLogReader.hpp" // vlog::full_fsync

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bpt {

// What opening the committed index found. Anything but Ok leads to a full rebuild from the vLog
// (VLogBehindError is the one exception: see IndexRecovery.hpp).
enum class OpenStatus {
    Ok,        // superblock, header, nonce, vLog identity, file size and tree pages all check out
    Missing,   // index.db does not exist
    BadHeader, // index.db header page fails its checks
    NoSuperblock,      // index.db exists but superblock.db does not
    SuperblockCorrupt, // both superblock slots invalid
    IndexMismatch,     // superblock describes a different index.db (nonce differs)
    VLogMismatch,      // superblock was written for a different vLog
    SizeMismatch,      // index.db is shorter than the committed page count
    TreeCorrupt,       // a page of the committed tree fails its CRC or structure checks
};

inline const char* toString(OpenStatus s) {
    switch (s) {
    case OpenStatus::Ok:
        return "Ok";
    case OpenStatus::Missing:
        return "Missing";
    case OpenStatus::BadHeader:
        return "BadHeader";
    case OpenStatus::NoSuperblock:
        return "NoSuperblock";
    case OpenStatus::SuperblockCorrupt:
        return "SuperblockCorrupt";
    case OpenStatus::IndexMismatch:
        return "IndexMismatch";
    case OpenStatus::VLogMismatch:
        return "VLogMismatch";
    case OpenStatus::SizeMismatch:
        return "SizeMismatch";
    case OpenStatus::TreeCorrupt:
        return "TreeCorrupt";
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
        if (this != &o) {
            release();
            moveFrom(o);
        }
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
        pager_ = o.pager_;
        frame_ = o.frame_;
        data_ = o.data_;
        id_ = o.id_;
        o.pager_ = nullptr;
        o.data_ = nullptr;
        o.id_ = kNoPage;
    }
    Pager* pager_{nullptr};
    uint32_t frame_{0};
    uint8_t* data_{nullptr};
    page_id_t id_{kNoPage};
};

class Pager {
public:
    static constexpr size_t kMinFrames = 16;

    enum class IoOp { PageWrite, Fsync };

    struct Options {
        size_t num_frames = 1024; // 4 MiB of cache
        std::function<void(uint64_t)>
            on_write; // called with 4096 after every page pwrite (tests count bytes)
        // Test-only fault injection: called before each page pwrite / fsync of index.db. Returning
        // true makes that call behave as if the syscall failed with EIO (the pager then aborts).
        std::function<bool(IoOp, page_id_t)> inject_failure;
    };

    struct Stats {
        uint64_t page_reads = 0;
        uint64_t page_writes = 0;
        uint64_t evictions = 0;
        uint64_t fsyncs = 0;
        uint64_t frees = 0;  // freePage calls
        uint64_t reuses = 0; // allocations served from the free list
    };

    // A verified committed tree (IndexRecovery.hpp checks it before calling openCommitted).
    struct Committed {
        page_id_t root = kNoPage;
        uint32_t page_count = 0;
        uint32_t height = 0;
        uint64_t key_count = 0;
        uint64_t epoch = 0;
        std::vector<page_id_t> free_now;   // not referenced by either superblock's tree
        std::vector<page_id_t> free_older; // referenced only by the previous superblock's tree
    };

    // What the next superblock records besides the tree itself.
    struct CommitInfo {
        uint64_t checkpoint_seq = 0; // every vLog entry with seq <= this is reflected in the tree
        uint32_t replay_segment = 0; // redo scan starts here (0: from the first segment)
        uint32_t replay_offset = vlog::kBlockSize;
        uint32_t vlog_first_segment = 0; // vLog identity: anchor segment (IndexRecovery.hpp)
        uint64_t vlog_created_ns = 0;
        uint32_t oldest_live_segment = 0; // lowest segment id present (0: use vlog_first_segment)
        std::vector<uint32_t> condemned;  // segments GC decided to unlink (<= kMaxCondemned)
    };

    struct CheckpointStats {
        uint64_t epoch = 0;
        uint64_t pages_written = 0; // dirty pages written back by this checkpoint
        uint64_t bytes_written = 0; // pages + the 4096-byte superblock slot
        uint32_t page_count = 0;
        page_id_t root = kNoPage;
    };

    // Creates a new file (O_CREAT | O_EXCL): header page (fsynced) and an empty root leaf
    // (dirty in the cache; written at the first checkpoint or eviction).
    static std::unique_ptr<Pager> create(const std::string& path, Options opts) {
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd < 0) fatal("open(O_CREAT|O_EXCL) failed for " + path);
        std::unique_ptr<Pager> p(new Pager(path, fd, opts));
        p->nonce_ = newNonce();
        p->page_count_ = 1;            // page 0 (header)
        p->checkpoint_page_count_ = 1; // only the header is frozen
        p->height_ = 1;
        p->writeHeaderLocked();
        p->fsyncOrDie("F_FULLFSYNC after create of " + path);
        {
            PageGuard root = p->allocate(PageType::Leaf, 0);
            p->root_ = root.id();
        }
        return p;
    }

    // Reads and checks the header page. Does not modify the file.
    static OpenStatus readHeader(const std::string& path, MetaBody* out) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) return OpenStatus::Missing;
            fatal("open failed for " + path);
        }
        OpenStatus st = readHeaderFd(fd, out);
        if (::close(fd) != 0) fatal("close failed for " + path);
        return st;
    }

    // Opens a file whose committed tree was already verified. The header must still match.
    static std::unique_ptr<Pager> openCommitted(const std::string& path, Options opts,
                                                const Committed& c) {
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd < 0) fatal("open failed for " + path);
        MetaBody m{};
        if (readHeaderFd(fd, &m) != OpenStatus::Ok) {
            errno = 0;
            fatal("index.db header changed after verification: " + path);
        }
        // Pages past the committed page count were written after the last checkpoint (or by a
        // crashed rebuild of the tree): no superblock names them. Discard them.
        struct stat st;
        if (::fstat(fd, &st) != 0) fatal("fstat failed for " + path);
        const uint64_t committed_bytes = static_cast<uint64_t>(c.page_count) * kPageSize;
        if (static_cast<uint64_t>(st.st_size) < committed_bytes) {
            errno = 0;
            fatal("index.db is shorter than the committed page count: " + path);
        }
        std::unique_ptr<Pager> p(new Pager(path, fd, opts));
        if (static_cast<uint64_t>(st.st_size) > committed_bytes) {
            if (::ftruncate(fd, static_cast<off_t>(committed_bytes)) != 0)
                fatal("ftruncate failed for " + path);
            p->fsyncOrDie("F_FULLFSYNC after ftruncate of " + path);
            p->truncated_pages_ = (static_cast<uint64_t>(st.st_size) - committed_bytes) / kPageSize;
        }
        p->nonce_ = m.index_nonce;
        p->root_ = c.root;
        p->height_ = c.height;
        p->key_count_ = c.key_count;
        p->page_count_ = c.page_count;
        p->checkpoint_page_count_ = c.page_count;
        p->epoch_ = c.epoch;
        for (page_id_t id : c.free_now) {
            if (id == kMetaPageId || id >= c.page_count) fatal("free list page id out of range");
            p->free_now_.push_back(id);
        }
        // Pages only the previous superblock's tree uses: freed in epoch (epoch - 1) from the
        // point of view of reuse, so they become reusable when epoch + 1 commits.
        for (page_id_t id : c.free_older) {
            if (id == kMetaPageId || id >= c.page_count) fatal("free list page id out of range");
            p->pending_.push_back({c.epoch == 0 ? 0 : c.epoch - 1, id});
        }
        return p;
    }

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    ~Pager() {
        if (fd_ >= 0 && ::close(fd_) != 0) fatal("close failed for " + path_);
    }

    PageGuard fetch(page_id_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        if (id == kMetaPageId || id >= page_count_) {
            fatal("fetch of invalid page id " + std::to_string(id) + " (page_count " +
                  std::to_string(page_count_) + ")");
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

    // New page: an id from the free list if one is reusable, else page_count++. Zeroed frame
    // with an empty header, pinned and dirty. The page is writable for the rest of the epoch.
    PageGuard allocate(PageType type, uint8_t level) {
        std::lock_guard<std::mutex> lk(mu_);
        page_id_t id;
        uint32_t f;
        if (!free_now_.empty()) {
            id = free_now_.back();
            free_now_.pop_back();
            reused_.insert(id);
            stats_.reuses++;
            auto it = table_.find(id);
            if (it != table_.end()) {
                // A stale copy of the old page is still cached: reuse its frame.
                f = it->second;
                Frame& fr = frames_[f];
                if (fr.pins != 0) fatal("reused page " + std::to_string(id) + " is pinned");
                if (fr.in_lru) {
                    lru_.erase(fr.lru_it);
                    fr.in_lru = false;
                }
                table_.erase(it);
                fr.page_id = kNoPage;
            } else {
                f = takeVictimLocked();
            }
        } else {
            if (page_count_ == UINT32_MAX) fatal("index.db page id space exhausted");
            id = page_count_++;
            f = takeVictimLocked();
        }
        initPage(frameData(f), id, type, level);
        installLocked(f, id, /*dirty=*/true);
        return PageGuard(this, f, frameData(f), id);
    }

    // True if the page belongs to a committed tree and must not be modified in place.
    bool isFrozen(page_id_t id) const {
        std::lock_guard<std::mutex> lk(mu_);
        return isFrozenLocked(id);
    }

    // The tree no longer references this page. A frozen page is reserved until two more epochs
    // commit; a page allocated in this epoch was never committed and is reusable at once.
    void freePage(page_id_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        if (id == kMetaPageId || id >= page_count_)
            fatal("freePage of invalid page id " + std::to_string(id));
        stats_.frees++;
        if (isFrozenLocked(id)) {
            pending_.push_back({epoch_, id});
        } else {
            reused_.erase(id);
            free_now_.push_back(id);
        }
    }

    uint64_t truncatedPagesAtOpen() const { return truncated_pages_; }

    uint32_t checkpointPageCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return checkpoint_page_count_;
    }
    size_t freeNowCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return free_now_.size();
    }
    size_t pendingFreeCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_.size();
    }
    // Test helper: ids of pages that may be handed out by allocate() right now.
    std::vector<page_id_t> freeNowForTest() const {
        std::lock_guard<std::mutex> lk(mu_);
        return free_now_;
    }

    // Writes back every dirty frame (no fsync).
    void flushDirty() {
        std::lock_guard<std::mutex> lk(mu_);
        flushDirtyLocked();
    }

    // Writes back every dirty frame, then F_FULLFSYNC.
    void flushAndSync() {
        std::lock_guard<std::mutex> lk(mu_);
        flushDirtyLocked();
        fsyncOrDie("F_FULLFSYNC of " + path_);
    }

    // Makes the current tree durable: write back dirty frames, F_FULLFSYNC index.db,
    // (after_index_fsync hook), then commit superblock epoch+1. The caller must hold the tree's
    // exclusive lock (or be the only user). No page may be pinned.
    CheckpointStats checkpoint(SuperblockManager& sbm, const CommitInfo& ci,
                               const std::function<void()>& after_index_fsync = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        if (fd_ < 0) fatal("checkpoint on a closed pager");
        for (const Frame& fr : frames_) {
            if (fr.pins != 0) fatal("checkpoint with a pinned page " + std::to_string(fr.page_id));
        }
        const uint64_t writes0 = stats_.page_writes;
        flushDirtyLocked();
        fsyncOrDie("F_FULLFSYNC of index pages during checkpoint of " + path_);
        if (after_index_fsync) after_index_fsync();
        SuperblockRecord rec;
        rec.sb.epoch = epoch_ + 1;
        rec.sb.checkpoint_seq = ci.checkpoint_seq;
        rec.sb.replay_segment_id = ci.replay_segment;
        rec.sb.replay_offset = ci.replay_offset;
        rec.sb.index_root_page = root_;
        rec.sb.index_page_count = page_count_;
        rec.sb.oldest_live_segment =
            ci.oldest_live_segment ? ci.oldest_live_segment : ci.vlog_first_segment;
        rec.ext.index_nonce = nonce_;
        rec.ext.vlog_first_segment = ci.vlog_first_segment;
        rec.ext.vlog_created_ns = ci.vlog_created_ns;
        if (ci.condemned.size() > kMaxCondemned) {
            errno = 0;
            fatal("checkpoint with more than " + std::to_string(kMaxCondemned) +
                  " condemned segments");
        }
        rec.gc.count = static_cast<uint32_t>(ci.condemned.size());
        for (size_t i = 0; i < ci.condemned.size(); ++i) rec.gc.ids[i] = ci.condemned[i];
        sbm.commit(rec);
        epoch_++;
        // Everything allocated so far is now part of a committed tree.
        checkpoint_page_count_ = page_count_;
        reused_.clear();
        // Pages freed in epoch e are no longer referenced by either slot once epoch e + 2 commits.
        size_t keep = 0;
        for (size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].first + 2 <= epoch_)
                free_now_.push_back(pending_[i].second);
            else
                pending_[keep++] = pending_[i];
        }
        pending_.resize(keep);
        CheckpointStats cs;
        cs.epoch = epoch_;
        cs.pages_written = stats_.page_writes - writes0;
        cs.bytes_written = cs.pages_written * kPageSize + kSuperSlotSize;
        cs.page_count = page_count_;
        cs.root = root_;
        return cs;
    }

    // Test helper: writes back dirty frames, then preads every page and runs checkPage on it.
    // Returns "" if all pages pass, else the first problem.
    std::string verifyPagesOnDisk() {
        std::lock_guard<std::mutex> lk(mu_);
        flushDirtyLocked();
        std::vector<uint8_t> buf(kPageSize);
        for (page_id_t id = 0; id < page_count_; ++id) {
            ssize_t r = ::pread(fd_, buf.data(), kPageSize, static_cast<off_t>(id) * kPageSize);
            if (r != static_cast<ssize_t>(kPageSize))
                return "short pread of page " + std::to_string(id);
            std::string why;
            if (!checkPage(buf.data(), id, &why)) return why;
        }
        return "";
    }

    // Tree state. The tree changes root/height/key_count/max_applied_seq under its exclusive lock.
    page_id_t rootPageId() const { return root_; }
    uint32_t treeHeight() const { return height_; }
    uint64_t keyCount() const { return key_count_; }
    uint64_t maxAppliedSeq() const { return max_applied_seq_; }
    uint32_t pageCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return page_count_;
    }
    void setRoot(page_id_t root, uint32_t height) {
        root_ = root;
        height_ = height;
    }
    void setKeyCount(uint64_t n) { key_count_ = n; }
    void setMaxAppliedSeq(uint64_t s) { max_applied_seq_ = s; }
    uint64_t nonce() const { return nonce_; }
    uint64_t committedEpoch() const {
        std::lock_guard<std::mutex> lk(mu_);
        return epoch_;
    }
    // After index.db.rebuild is renamed to index.db (the open fd stays valid).
    void setPath(const std::string& p) { path_ = p; }

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
        for (size_t i = opts_.num_frames; i-- > 0;)
            free_frames_.push_back(static_cast<uint32_t>(i));
    }

    static uint64_t newNonce() {
        std::random_device rd;
        uint64_t a = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        uint64_t t =
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        uint64_t n = a ^ (t * 0x9E3779B97F4A7C15ULL) ^ static_cast<uint64_t>(::getpid());
        return n == 0 ? 1 : n;
    }

    static OpenStatus readHeaderFd(int fd, MetaBody* out) {
        uint8_t page[kPageSize];
        ssize_t r = ::pread(fd, page, kPageSize, 0);
        if (r < 0) fatal("pread of header page failed");
        if (r != static_cast<ssize_t>(kPageSize)) return OpenStatus::BadHeader;
        if (!checkPage(page, kMetaPageId, nullptr)) return OpenStatus::BadHeader;
        MetaBody m;
        std::memcpy(&m, page + kHeaderSize, sizeof(m));
        if (m.magic != kMetaMagic || m.format_version != kIndexFormatVersion ||
            m.page_size != kPageSize || m.index_nonce == 0 || m.reserved1 != 0) {
            return OpenStatus::BadHeader;
        }
        *out = m;
        return OpenStatus::Ok;
    }

    uint8_t* frameData(uint32_t f) const {
        return buffer_.get() + static_cast<size_t>(f) * kPageSize;
    }

    void fsyncOrDie(const std::string& what) {
        if (opts_.inject_failure && opts_.inject_failure(IoOp::Fsync, kNoPage)) {
            errno = EIO;
            fatal(what + " [injected failure]");
        }
        if (vlog::full_fsync(fd_) != 0) fatal(what); // never retried
        stats_.fsyncs++;
    }

    void writeHeaderLocked() {
        uint8_t page[kPageSize];
        initPage(page, kMetaPageId, PageType::Meta, 0);
        MetaBody m{};
        m.magic = kMetaMagic;
        m.format_version = kIndexFormatVersion;
        m.page_size = kPageSize;
        m.index_nonce = nonce_;
        std::memcpy(page + kHeaderSize, &m, sizeof(m));
        uint32_t crc = computePageCrc(page);
        std::memcpy(page, &crc, sizeof(crc));
        writePageBytesLocked(page, kMetaPageId);
    }

    void writePageBytesLocked(const uint8_t* page, page_id_t id) {
        if (opts_.inject_failure && opts_.inject_failure(IoOp::PageWrite, id)) {
            errno = EIO;
            fatal("pwrite of page " + std::to_string(id) + " failed [injected failure]");
        }
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
        if (fr.in_lru) {
            lru_.erase(fr.lru_it);
            fr.in_lru = false;
        }
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
        if (isFrozenLocked(frames_[f].page_id)) {
            errno = 0;
            fatal("write to frozen page " + std::to_string(frames_[f].page_id) +
                  " (copy-on-write violated)");
        }
        frames_[f].dirty = true;
    }

    bool isFrozenLocked(page_id_t id) const {
        return id < checkpoint_page_count_ && reused_.count(id) == 0;
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
    uint64_t nonce_ = 0;
    uint64_t truncated_pages_ = 0;

    // Tree state (in memory; durable only through checkpoint()).
    page_id_t root_ = kNoPage;
    uint32_t height_ = 0;
    uint64_t key_count_ = 0;
    uint64_t max_applied_seq_ = 0;

    mutable std::mutex mu_;
    uint32_t page_count_ = 0; // guarded by mu_
    uint64_t epoch_ = 0;      // last committed superblock epoch; guarded by mu_
    uint32_t checkpoint_page_count_ =
        0; // ids below this are frozen (unless in reused_); guarded by mu_
    std::unordered_set<page_id_t> reused_; // free-list ids handed out in this epoch (writable)
    std::vector<page_id_t> free_now_;      // reusable now
    std::vector<std::pair<uint64_t, page_id_t>> pending_; // (epoch freed in, id)
    std::vector<Frame> frames_;
    std::unique_ptr<uint8_t[]> buffer_;
    std::vector<uint32_t> free_frames_;
    std::list<uint32_t> lru_; // front = most recently unpinned
    std::unordered_map<page_id_t, uint32_t> table_;
    Stats stats_;
};

inline void PageGuard::markDirty() {
    pager_->markDirty(frame_);
}

inline void PageGuard::release() {
    if (pager_) {
        pager_->unpin(frame_);
        pager_ = nullptr;
        data_ = nullptr;
        id_ = kNoPage;
    }
}

} // namespace bpt
