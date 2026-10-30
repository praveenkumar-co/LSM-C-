#pragma once
// ScanProfile.hpp - benchmark-only counters for the read path. Only `make bench-scan` builds a
// binary with -DSCAN_PROFILE. Without that flag every macro below expands to an empty statement,
// so tests and every other build compile exactly the code they compiled before.
//
// The counters are per thread (thread_local): a benchmark thread reads its own totals after its
// loop, so the counting itself takes no lock.

#if defined(SCAN_PROFILE)

#include <chrono>
#include <cstdint>

namespace vlog {

struct ScanProfile {
    uint64_t tree_ns = 0;          // inside BPTree::get / BPTree::scan (includes the two below)
    uint64_t tree_calls = 0;
    uint64_t lock_tree_ns = 0;     // waiting for the tree's shared lock
    uint64_t lock_pager_ns = 0;    // waiting for the pager mutex (page fetch and unpin)
    uint64_t lock_registry_ns = 0; // waiting for ReaderRegistry locks (pin and unpin)
    uint64_t lock_fdcache_ns = 0;  // waiting for the segment fd cache mutex (acquire and release)
    uint64_t fstat_calls = 0;      // fstat() in the fd cache (one per cached acquire)
    uint64_t fstat_ns = 0;         // time in those fstat() calls
    uint64_t vlog_preads = 0;      // pread() calls on vLog segments
    uint64_t vlog_pread_bytes = 0; // bytes those calls returned
    uint64_t vlog_pread_ns = 0;
    uint64_t index_preads = 0;     // pread() of index.db pages (buffer pool misses)
    uint64_t index_pread_bytes = 0;
    uint64_t crc_ns = 0;           // CRC32C of vLog entries
    uint64_t crc_bytes = 0;
    uint64_t delta_ns = 0;         // Delta decode + apply to the base + serialize
    uint64_t delta_resolves = 0;   // documents that had a Delta chain
};

inline ScanProfile& scanProfile() {
    thread_local ScanProfile p;
    return p;
}

inline uint64_t scanProfileNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// Adds the time from its construction to the end of the enclosing scope to one counter.
class ScanProfileScope {
public:
    explicit ScanProfileScope(uint64_t* field) : field_(field), t0_(scanProfileNowNs()) {}
    ~ScanProfileScope() { *field_ += scanProfileNowNs() - t0_; }
    ScanProfileScope(const ScanProfileScope&) = delete;
    ScanProfileScope& operator=(const ScanProfileScope&) = delete;

private:
    uint64_t* field_;
    uint64_t t0_;
};

} // namespace vlog

#define SCAN_PROF_T0(t) const uint64_t t = ::vlog::scanProfileNowNs()
#define SCAN_PROF_ADD_NS(field, t) (::vlog::scanProfile().field += ::vlog::scanProfileNowNs() - (t))
#define SCAN_PROF_COUNT(field, n) (::vlog::scanProfile().field += static_cast<uint64_t>(n))
#define SCAN_PROF_SCOPE(var, field) ::vlog::ScanProfileScope var(&::vlog::scanProfile().field)

#else

#define SCAN_PROF_T0(t) ((void)0)
#define SCAN_PROF_ADD_NS(field, t) ((void)0)
#define SCAN_PROF_COUNT(field, n) ((void)0)
#define SCAN_PROF_SCOPE(var, field) ((void)0)

#endif
