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
// All state is behind one mutex (no lock-free code); the syscalls (fstat, open, close) run with
// the mutex released, so one slow syscall does not make every other reader wait. Closing a
// read-only descriptor cannot lose data, so a failing close() is not fatal here (as in VLogReader).

#include "index/IndexCommon.hpp"
#include "vlog/ScanProfile.hpp"

#include <cstdint>
#include <iterator>
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

    // An empty lease if the segment does not exist. Syscalls (fstat, open, close) run outside the
    // mutex: a lease taken under the mutex keeps the descriptor open while fstat checks it.
    Lease acquire(uint32_t seg) {
        std::shared_ptr<Entry> e;
        {
            SCAN_PROF_T0(prof_lock);
            std::lock_guard<std::mutex> lk(mu_);
            SCAN_PROF_ADD_NS(lock_fdcache_ns, prof_lock);
            auto it = map_.find(seg);
            if (it != map_.end()) {
                e = it->second;
                e->refs++; // keeps e->fd open until this lease is released
                lru_.erase(e->lru_it);
                lru_.push_front(seg);
                e->lru_it = lru_.begin();
                hits_++; // taken back below if the file turns out to be unlinked
            }
        }
        if (e) {
            SCAN_PROF_COUNT(fstat_calls, 1);
            struct stat st;
            SCAN_PROF_T0(prof_fstat);
            const int rc = ::fstat(e->fd, &st);
            SCAN_PROF_ADD_NS(fstat_ns, prof_fstat);
            if (rc == 0 && st.st_nlink > 0) return Lease(this, e);
            // Unlinked (or fstat failed): drop this descriptor and open the path again.
            int to_close = -1;
            {
                std::lock_guard<std::mutex> lk(mu_);
                hits_--;
                auto it = map_.find(seg);
                if (it != map_.end() && it->second == e) to_close = dropLocked(it);
                if (e->refs == 0) fatal("SegmentFdCache: release without a lease");
                if (--e->refs == 0 && e->dead) {
                    to_close = e->fd;
                    e->fd = -1;
                }
            }
            closeFd(to_close);
            e.reset();
        }
        char name[32];
        std::snprintf(name, sizeof(name), "/vlog_%08u.log", seg);
        const int fd = ::open((dir_ + name).c_str(), O_RDONLY);
        if (fd < 0) {
            if (errno != ENOENT) fatal("open failed for vLog segment " + std::to_string(seg));
            std::lock_guard<std::mutex> lk(mu_);
            misses_++;
            return Lease();
        }
        std::vector<int> to_close;
        {
            std::lock_guard<std::mutex> lk(mu_);
            misses_++;
            auto it = map_.find(seg);
            if (it != map_.end()) {
                // Another thread opened it meanwhile: use that descriptor, close ours.
                e = it->second;
                e->refs++;
                lru_.erase(e->lru_it);
                lru_.push_front(seg);
                e->lru_it = lru_.begin();
                to_close.push_back(fd);
            } else {
                e = std::make_shared<Entry>();
                e->seg = seg;
                e->fd = fd;
                e->refs = 1;
                lru_.push_front(seg);
                e->lru_it = lru_.begin();
                map_[seg] = e;
                // Evict least recently used descriptors no lease holds.
                // lit marks the first entry already passed over; dropping the one before it
                // leaves lit valid (std::list iterators survive erasing other elements).
                for (auto lit = lru_.end(); map_.size() > cap_ && lit != lru_.begin();) {
                    auto cur = std::prev(lit);
                    auto mit = map_.find(*cur);
                    if (mit->second->refs != 0) {
                        lit = cur;
                        continue;
                    }
                    to_close.push_back(dropLocked(mit));
                }
            }
        }
        for (int f : to_close) closeFd(f);
        return Lease(this, e);
    }

    // Before a segment is unlinked.
    void invalidate(uint32_t seg) {
        int to_close = -1;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = map_.find(seg);
            if (it != map_.end()) to_close = dropLocked(it);
        }
        closeFd(to_close);
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

    // Removes the entry from the map and LRU. Returns its descriptor if no lease holds it (the
    // caller closes it after releasing the mutex), else -1 (the last lease closes it).
    int dropLocked(Map::iterator it) {
        std::shared_ptr<Entry> e = it->second;
        lru_.erase(e->lru_it);
        map_.erase(it);
        e->dead = true;
        if (e->refs != 0) return -1;
        const int fd = e->fd;
        e->fd = -1;
        return fd;
    }

    void unref(const std::shared_ptr<Entry>& e) {
        int to_close = -1;
        {
            SCAN_PROF_T0(prof_lock);
            std::lock_guard<std::mutex> lk(mu_);
            SCAN_PROF_ADD_NS(lock_fdcache_ns, prof_lock);
            if (e->refs == 0) fatal("SegmentFdCache: release without a lease");
            if (--e->refs == 0 && e->dead) {
                to_close = e->fd;
                e->fd = -1;
            }
        }
        closeFd(to_close);
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
