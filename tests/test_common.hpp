#pragma once
// Shared safety helpers for the Phase 2 tests (same rules as tests/test_vlog_crash.cpp):
//  - one mkdtemp dir per run, removed on normal exit, path printed on any abort
//  - a global byte budget of 512 MiB for all writes, enforced as writes happen
//  - abort if statvfs fails or free disk space is below 10 GB
//  - CHECK aborts even under NDEBUG
//
// The byte counter lives in MAP_SHARED anonymous memory, so writes done by forked children
// (including children killed with SIGKILL) are counted in the same total.

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include <sys/mman.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::cerr << "CHECK FAILED at " << __FILE__ << ":" << __LINE__ << ": " #cond         \
                      << " (errno " << errno << ": " << std::strerror(errno) << ")\n";           \
            std::abort();                                                                        \
        }                                                                                        \
    } while (0)

namespace testutil {

inline char g_test_dir[1024] = {0};

extern "C" inline void onAbortSignal(int) {
    if (g_test_dir[0] != '\0') {
        static const char prefix[] = "[Abort] Temp test directory was NOT removed. Delete it manually: rm -rf ";
        // Only async-signal-safe calls. Write errors are ignored: the process is already aborting.
        (void)!write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
        (void)!write(STDERR_FILENO, g_test_dir, std::strlen(g_test_dir));
        (void)!write(STDERR_FILENO, "\n", 1);
    }
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
}

// In a forked child that is expected to abort: no "[Abort] ... rm -rf" message from it.
inline void childExpectsAbort() { CHECK(std::signal(SIGABRT, SIG_DFL) != SIG_ERR); }

constexpr uint64_t kWriteBudgetBytes = 512ULL * 1024 * 1024;

inline std::atomic<uint64_t>* g_bytes_written = nullptr;

inline void initBudget() {
    void* p = mmap(nullptr, sizeof(std::atomic<uint64_t>), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED);
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared-memory counter must be lock-free");
    g_bytes_written = new (p) std::atomic<uint64_t>(0);
}

inline uint64_t bytesWritten() { return g_bytes_written->load(); }

inline void accountBytes(uint64_t n, const char* what) {
    uint64_t total = g_bytes_written->fetch_add(n) + n;
    if (total > kWriteBudgetBytes) {
        std::cerr << "[Budget Fatal] " << what << " pushed total test writes to " << total << " bytes (limit "
                  << kWriteBudgetBytes << "). Aborting.\n";
        std::abort();
    }
}

inline uint64_t remainingBudget() {
    uint64_t used = g_bytes_written->load();
    CHECK(used < kWriteBudgetBytes);
    return kWriteBudgetBytes - used;
}

inline void checkFreeSpace(const std::string& path) {
    struct statvfs st;
    if (statvfs(path.c_str(), &st) != 0) {
        std::cerr << "[Safety Fatal] statvfs failed on " << path << " (errno " << errno << ": "
                  << std::strerror(errno) << "). Cannot verify free space. Aborting.\n";
        std::abort();
    }
    uint64_t free_bytes = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    std::cout << "[Safety] Available Disk Space: " << (free_bytes >> 30) << " GB\n";
    if (free_bytes < 10ULL * 1024 * 1024 * 1024) {
        std::cerr << "[Safety Fatal] Less than 10 GB free (" << (free_bytes >> 20) << " MB). Aborting.\n";
        std::abort();
    }
}

// Creates the run's temp dir, installs the abort handler, checks free space, sets up the budget.
inline std::string setupRun(const char* prefix) {
    std::string tmpl = std::string("/tmp/") + prefix + "_XXXXXX";
    CHECK(tmpl.size() < 256);
    char buf[256];
    std::memcpy(buf, tmpl.c_str(), tmpl.size() + 1);
    char* dir = mkdtemp(buf);
    CHECK(dir != nullptr);
    std::string test_dir = dir;
    std::cout << "[Setup] Test Directory: " << test_dir << "\n";
    CHECK(test_dir.size() < sizeof(g_test_dir));
    std::memcpy(g_test_dir, test_dir.c_str(), test_dir.size() + 1);
    CHECK(std::signal(SIGABRT, onAbortSignal) != SIG_ERR);
    checkFreeSpace(test_dir);
    initBudget();
    return test_dir;
}

struct Cleaner {
    std::string dir;
    ~Cleaner() {
        std::cout << "[Cleanup] Removing temp test directory: " << dir << "\n";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        if (ec) std::cerr << "[Cleanup] remove_all failed (" << ec.message() << "). Delete manually: rm -rf " << dir << "\n";
    }
};

inline void printBudget() {
    std::cout << "\n[Budget] Total bytes written by all stages: " << bytesWritten() << " of " << kWriteBudgetBytes
              << " allowed (512 MiB).\n";
}

inline void banner(const std::string& title) {
    std::cout << "\n=======================================================\n"
              << title << "\n=======================================================\n";
}

}  // namespace testutil
