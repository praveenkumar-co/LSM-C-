#pragma once
// FdCache.hpp - bounded LRU of read-only file descriptors of vLog segments, so a get() reads an
// entry with one pread instead of open + fstat + 2 x pread + close.
//
// - acquire(seg) returns a Lease that keeps the descriptor open while it is held; eviction (LRU
//   beyond the capacity) closes only descriptors no lease holds.
// - A cached descriptor whose file was unlinked (st_nlink == 0: GC, or a segment deleted from
//   outside) is dropped and the path is opened again, so a deleted segment reads as missing.
// - invalidate(seg): GC calls it before unlinking a segment; the descriptor closes once no lease
//   holds it. Reader pins (ReaderRegistry) still decide when a segment may be unlinked.
// All state is behind one mutex (no lock-free code). Closing a read-only descriptor cannot lose
// data, so a failing close() is not fatal here (as in VLogReader).

#include "index/IndexCommon.hpp"

#include <cstdint>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bpt {

class SegmentFdCache {
    struct Entry {
        uint32_t seg = 0;
        int fd = -1;
        uint32_t refs = 0;
        bool dead = false; // removed from the map; close when refs reaches 0
        std::list<uint32_t>::iterator lru_it;
    };

public:
    class Lease {
    public:
        Lease() = default;
        Lease(Lease&& o) noexcept : cache_(o.cache_), e_(std::move(o.e_)) { o.cache_ = nullptr; }
        Lease& operator=(Lease&& o) noexcept {
            if (this != &o) {
                release();
                cache_ = o.cache_;
                e_ = std::move(o.e_);
                o.cache_ = nullptr;
            }
            return *this;
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() { release(); }
        explicit operator bool() const { return e_ != nullptr; }
        int fd() const { return e_ ? e_->fd : -1; }

    private:
        friend class SegmentFdCache;
        Lease(SegmentFdCache* c, std::shared_ptr<Entry> e) : cache_(c), e_(std::move(e)) {}
        void release() {
            if (cache_ && e_) cache_->unref(e_);
            cache_ = nullptr;
            e_.reset();
        }
        SegmentFdCache* cache_ = nullptr;
        std::shared_ptr<Entry> e_;
    };

    SegmentFdCache(std::string vlog_dir, size_t capacity)
        : dir_(std::move(vlog_dir)), cap_(capacity < 1 ? 1 : capacity) {}
    SegmentFdCache(const SegmentFdCache&) = delete;
    SegmentFdCache& operator=(const SegmentFdCache&) = delete;

    ~SegmentFdCache() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [seg, e] : map_) closeFd(e->fd);
        map_.clear();
    }

    // An empty lease if the segment does not exist.
    Lease acquire(uint32_t seg) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(seg);
        if (it != map_.end()) {
            struct stat st;
            if (::fstat(it->second->fd, &st) == 0 && st.st_nlink > 0) {
                std::shared_ptr<Entry> e = it->second;
                lru_.erase(e->lru_it);
                lru_.push_front(seg);
                e->lru_it = lru_.begin();
                e->refs++;
                hits_++;
                return Lease(this, e);
            }
            dropLocked(it); // unlinked (or fstat failed): open the path again
        }
        misses_++;
        char name[32];
        std::snprintf(name, sizeof(name), "/vlog_%08u.log", seg);
        const int fd = ::open((dir_ + name).c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno == ENOENT) return Lease();
            fatal("open failed for vLog segment " + std::to_string(seg));
        }
        auto e = std::make_shared<Entry>();
        e->seg = seg;
        e->fd = fd;
        e->refs = 1;
        lru_.push_front(seg);
        e->lru_it = lru_.begin();
        map_[seg] = e;
        // Evict least recently used descriptors no lease holds.
        // lit marks the first entry already passed over; dropping the one before it leaves lit
        // valid (std::list iterators survive erasing other elements).
        for (auto lit = lru_.end(); map_.size() > cap_ && lit != lru_.begin();) {
            auto cur = std::prev(lit);
            auto mit = map_.find(*cur);
            if (mit->second->refs != 0) {
                lit = cur;
                continue;
            }
            dropLocked(mit);
        }
        return Lease(this, e);
    }

    // Before a segment is unlinked.
    void invalidate(uint32_t seg) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(seg);
        if (it != map_.end()) dropLocked(it);
    }

    bool contains(uint32_t seg) const {
        std::lock_guard<std::mutex> lk(mu_);
        return map_.count(seg) != 0;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return map_.size();
    }
    uint64_t hits() const {
        std::lock_guard<std::mutex> lk(mu_);
        return hits_;
    }
    uint64_t misses() const {
        std::lock_guard<std::mutex> lk(mu_);
        return misses_;
    }

private:
    using Map = std::unordered_map<uint32_t, std::shared_ptr<Entry>>;

    // Removes the entry from the map and LRU; closes it now if no lease holds it.
    void dropLocked(Map::iterator it) {
        std::shared_ptr<Entry> e = it->second;
        lru_.erase(e->lru_it);
        map_.erase(it);
        e->dead = true;
        if (e->refs == 0) {
            closeFd(e->fd);
            e->fd = -1;
        }
    }

    void unref(const std::shared_ptr<Entry>& e) {
        std::lock_guard<std::mutex> lk(mu_);
        if (e->refs == 0) fatal("SegmentFdCache: release without a lease");
        if (--e->refs == 0 && e->dead) {
            closeFd(e->fd);
            e->fd = -1;
        }
    }

    static void closeFd(int fd) {
        if (fd >= 0) (void)::close(fd); // read-only descriptor: nothing to lose
    }

    std::string dir_;
    size_t cap_;
    mutable std::mutex mu_;
    Map map_;
    std::list<uint32_t> lru_; // front = most recently used
    uint64_t hits_ = 0, misses_ = 0;
};

} // namespace bpt
