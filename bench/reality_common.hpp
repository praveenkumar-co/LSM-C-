#pragma once
// reality_common.hpp - helpers shared by the Phase 7 reality-check tools (bench_reality,
// bench_crash): OS counters, RSS, free disk, a latency histogram, the key format.
#include <libproc.h>
#include <mach/mach.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace reality {

using Clock = std::chrono::steady_clock;

// ---- OS counters ----
struct OsStats {
    uint64_t written = 0, read = 0;
    uint64_t footprint = 0, footprint_peak = 0;
};
// OS counters of a process (this one by default). For another process (a child), a failed
// call returns false (the process is gone).
inline bool osStatsOf(pid_t pid, OsStats* out, uint64_t* resident = nullptr) {
    rusage_info_v4 ri{};
    if (proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) != 0)
        return false;
    *out = OsStats{ri.ri_diskio_byteswritten, ri.ri_diskio_bytesread, ri.ri_phys_footprint,
                   ri.ri_lifetime_max_phys_footprint};
    if (resident) *resident = ri.ri_resident_size;
    return true;
}
inline OsStats osStats() {
    OsStats s;
    if (!osStatsOf(getpid(), &s)) {
        std::perror("proc_pid_rusage");
        std::_Exit(4);
    }
    return s;
}
inline uint64_t rssNow() {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &n) != KERN_SUCCESS)
        return 0;
    return info.resident_size;
}
inline uint64_t rssPeak() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<uint64_t>(ru.ru_maxrss); // bytes on macOS
}
inline uint64_t freeBytes(const std::string& path) {
    struct statfs s{};
    if (statfs(path.c_str(), &s) != 0) {
        std::perror("statfs");
        std::_Exit(4);
    }
    return static_cast<uint64_t>(s.f_bavail) * s.f_bsize;
}
inline uint64_t dirBytes(const std::string& d) {
    uint64_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(d, ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    return n;
}


// ---- Latency histogram: buckets of 2% width from 1 us to ~100 s ----
struct Hist {
    static constexpr double kBase = 1.02;
    static constexpr int kN = 950;
    std::vector<uint64_t> c = std::vector<uint64_t>(kN + 1, 0);
    uint64_t n = 0;
    void add(double us) {
        int i = us <= 1 ? 0 : int(std::log(us) / std::log(kBase)) + 1;
        c[std::min(i, kN)]++;
        n++;
    }
    void merge(const Hist& o) {
        for (int i = 0; i <= kN; ++i) c[i] += o.c[i];
        n += o.n;
    }
    double pct(double p) const {
        if (n == 0) return 0;
        const uint64_t want = uint64_t(std::ceil(p * double(n)));
        uint64_t s = 0;
        for (int i = 0; i <= kN; ++i) {
            s += c[i];
            if (s >= want) return i == 0 ? 1.0 : std::pow(kBase, i); // bucket upper edge
        }
        return std::pow(kBase, kN);
    }
};

inline std::string key(uint64_t k) {
    char b[24];
    std::snprintf(b, sizeof(b), "k%09llu", (unsigned long long)k);
    return b;
}

} // namespace reality
