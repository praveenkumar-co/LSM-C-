#include "VLogFormat.hpp"
#include "CRC32.hpp"
#include "VLogWriter.hpp"
#include "VLogReader.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <csignal>
#include <cstring>

// -------------------------------------------------------------
// Temp dir path, printed on any abort (CHECK failure, writer fatal path,
// uncaught exception) so a failed run can be cleaned up by hand.
// -------------------------------------------------------------
static char g_test_dir[1024] = {0};

extern "C" void onAbortSignal(int) {
    if (g_test_dir[0] != '\0') {
        static const char prefix[] = "[Abort] Temp test directory was NOT removed. Delete it manually: rm -rf ";
        // Only async-signal-safe calls here. Write errors are ignored: the process is already aborting.
        (void)!write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
        (void)!write(STDERR_FILENO, g_test_dir, std::strlen(g_test_dir));
        (void)!write(STDERR_FILENO, "\n", 1);
    }
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
}

// -------------------------------------------------------------
// CHECK Macro: Evaluates condition and aborts even under NDEBUG (Rule 7)
// -------------------------------------------------------------
#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "CHECK FAILED at " << __FILE__ << ":" << __LINE__ << ": " #cond \
                      << " (errno " << errno << ": " << std::strerror(errno) << ")\n"; \
            std::abort(); \
        } \
    } while (0)

using namespace vlog;

// -------------------------------------------------------------
// Global write budget: all stages together may write at most 512 MiB.
// -------------------------------------------------------------
constexpr uint64_t kWriteBudgetBytes = 512ULL * 1024 * 1024;
static std::atomic<uint64_t> g_bytes_written{0};

void accountBytes(uint64_t n, const char* what) {
    uint64_t total = g_bytes_written.fetch_add(n) + n;
    if (total > kWriteBudgetBytes) {
        std::cerr << "[Budget Fatal] " << what << " pushed total test writes to " << total
                  << " bytes (limit " << kWriteBudgetBytes << "). Aborting.\n";
        std::abort();
    }
}

uint64_t remainingBudget() {
    uint64_t used = g_bytes_written.load();
    CHECK(used < kWriteBudgetBytes);
    return kWriteBudgetBytes - used;
}

// Writer options whose max_bytes never exceeds what is left of the global budget.
VLogWriter::Options budgetOpts(uint32_t segment_size_limit = 64 * 1024 * 1024, uint64_t local_cap = 0) {
    uint64_t cap = remainingBudget();
    if (local_cap > 0) cap = std::min(cap, local_cap);
    return VLogWriter::Options(segment_size_limit, cap);
}

// VLogWriter that adds its byte count to the global budget when it goes out of scope.
// Derived destructor runs before ~VLogWriter; never deleted through a base pointer.
class TrackedWriter : public VLogWriter {
public:
    using VLogWriter::VLogWriter;
    ~TrackedWriter() { accountBytes(getTotalBytesWritten(), "VLogWriter"); }
};

// Checked syscall wrappers for test-side file manipulation.
void checkedWrite(int fd, const void* buf, size_t n) {
    ssize_t w = write(fd, buf, n);
    CHECK(w == static_cast<ssize_t>(n));
    accountBytes(n, "write()");
}

void checkedPwrite(int fd, const void* buf, size_t n, off_t off) {
    ssize_t w = pwrite(fd, buf, n, off);
    CHECK(w == static_cast<ssize_t>(n));
    accountBytes(n, "pwrite()");
}

void checkedFsync(int fd) { CHECK(full_fsync(fd) == 0); }
void checkedClose(int fd) { CHECK(close(fd) == 0); }

// -------------------------------------------------------------
// Helper: Deterministic payload generation derived strictly from seq
// -------------------------------------------------------------
inline void fill_deterministic_payload(uint64_t seq, uint8_t* buf, size_t len) {
    std::string prefix = "SEQ:" + std::to_string(seq) + ":";
    size_t prefix_len = std::min(prefix.size(), len);
    std::memcpy(buf, prefix.data(), prefix_len);
    for (size_t i = prefix_len; i < len; ++i) {
        buf[i] = static_cast<uint8_t>((seq * 1315423911ULL + i * 2654435761ULL) & 0xFF);
    }
}

// -------------------------------------------------------------
// Hardware Safety & Disk Space Guard (Explicit if + abort, not assert)
// -------------------------------------------------------------
void checkHardwareSafety(const std::string& path) {
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) != 0) {
        std::cerr << "[Safety Fatal] statvfs failed on " << path << " (errno " << errno << ": "
                  << std::strerror(errno) << "). Cannot verify free space. Aborting.\n";
        std::abort();
    }
    uint64_t free_bytes = stat.f_bavail * stat.f_frsize;
    uint64_t ten_gb = 10ULL * 1024 * 1024 * 1024;
    std::cout << "[Safety] Available Disk Space: " << (free_bytes / (1024 * 1024 * 1024)) << " GB\n";
    if (free_bytes < ten_gb) {
        std::cerr << "[Safety Fatal] Less than 10 GB disk space available (" 
                  << (free_bytes / (1024 * 1024)) << " MB). Aborting test to protect hardware.\n";
        std::abort();
    }
}

// -------------------------------------------------------------
// STAGE 1: CRC32C Test Vector Validation
// -------------------------------------------------------------
void testStage1CRC() {
    std::cout << "\n=======================================================\n";
    std::cout << "STAGE 1: CRC32C Test Vector Validation\n";
    std::cout << "=======================================================\n";

    const char* str = "123456789";
    uint32_t t_res = crc32c_table(str, 9);
    std::cout << "Table CRC32C(\"123456789\")       : 0x" << std::hex << t_res << std::dec << "\n";
    CHECK(t_res == 0xE3069283);

    uint32_t hw_res = crc32c_hardware(str, 9);
    std::cout << "ARMv8 CRC32C(\"123456789\")       : 0x" << std::hex << hw_res << std::dec << "\n";
    CHECK(hw_res == 0xE3069283);
    CHECK(t_res == hw_res);

    // Verify 100 arbitrary payloads produce bit-identical results
    std::mt19937 rng(42);
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> payload(100 + (rng() % 1000));
        for (auto& b : payload) b = static_cast<uint8_t>(rng() & 0xFF);

        uint32_t c1 = crc32c_table(payload.data(), payload.size());
        uint32_t c2 = crc32c_hardware(payload.data(), payload.size());
        CHECK(c1 == c2 && "Hardware CRC and table CRC diverged!");
    }
    std::cout << "PASS: ARMv8 CRC and table CRC both give 0xe3069283 for \"123456789\" and match on 100 random payloads (100-1099 bytes).\n";
}

// -------------------------------------------------------------
// STAGE 2: Reader Header Bounds & Corrupt Length Check
// -------------------------------------------------------------
void testStage2HeaderBounds(const std::string& test_dir) {
    std::cout << "\n=======================================================\n";
    std::cout << "STAGE 2: Header Bounds & Corrupt Length Rejection\n";
    std::cout << "=======================================================\n";

    VLogWriter::Options opts = budgetOpts(1024 * 1024, 10 * 1024 * 1024);

    TrackedWriter writer(test_dir, opts);
    std::string key = "user_test_bounds";
    std::string val = "{\"test\": \"payload\"}";
    VPtr vptr = writer.append(EntryType::Full, key, val.data(), val.size());

    VLogReader reader(test_dir);
    EntryHeader hdr;
    std::string r_key;
    std::vector<uint8_t> r_val;
    bool ok = reader.readEntry(vptr, hdr, r_key, r_val);
    CHECK(ok && "Failed to read valid entry");

    // Corrupt on-disk total_len and set vptr.length to match it (Rule 5)
    // This guarantees validateHeaderBounds() is what rejects the entry, not a length mismatch.
    std::string seg_path = test_dir + "/vlog_00000001.log";
    int fd = open(seg_path.c_str(), O_RDWR);
    CHECK(fd >= 0);

    auto test_corrupt_bounds = [&](uint32_t bad_len_val, const char* label) {
        // Write bad_len_val to hdr.total_len on disk (offset + 4)
        checkedPwrite(fd, &bad_len_val, sizeof(bad_len_val), vptr.offset + 4);
        checkedFsync(fd);

        VPtr test_vptr = vptr;
        test_vptr.length = bad_len_val; // Matching length so length mismatch passes

        bool res = reader.readEntry(test_vptr, hdr, r_key, r_val);
        CHECK(!res && label);
    };

    // 1. total_len < 32 (< sizeof(EntryHeader))
    test_corrupt_bounds(16, "validateHeaderBounds failed to reject total_len < 32");

    // 2. Non-8-byte aligned total_len
    test_corrupt_bounds(45, "validateHeaderBounds failed to reject non-8-aligned total_len");

    // 3. total_len > 16 MiB (kMaxEntryLen cap)
    test_corrupt_bounds(32 * 1024 * 1024, "validateHeaderBounds failed to reject total_len > 16 MiB");

    // 4. offset + total_len > file_size
    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    uint32_t oversize = static_cast<uint32_t>(align_up(st.st_size - vptr.offset + 8, kEntryAlign));
    test_corrupt_bounds(oversize, "validateHeaderBounds failed to reject offset + total_len > file_size");

    // Restore valid on-disk total_len and verify clean read
    checkedPwrite(fd, &vptr.length, sizeof(vptr.length), vptr.offset + 4);
    checkedFsync(fd);
    checkedClose(fd);

    ok = reader.readEntry(vptr, hdr, r_key, r_val);
    CHECK(ok && "Failed to read restored entry");

    std::cout << "PASS: validateHeaderBounds strictly rejects corrupt total_len (<32, >16MB, unaligned, >file_size) on-disk.\n";
}

// -------------------------------------------------------------
// STAGE 3: Torn Tail vs Mid-Log Corruption & Non-Last Segment Rules
// -------------------------------------------------------------
void testStage3TornTailAndCorruption(const std::string& test_dir) {
    std::cout << "\n=======================================================\n";
    std::cout << "STAGE 3: Torn Tail Truncation vs Mid-Log Corruption Rules\n";
    std::cout << "=======================================================\n";

    // Case A: Torn Tail in LAST segment -> Cleanly truncated
    std::string torn_dir = test_dir + "/torn_tail_last";
    std::filesystem::create_directories(torn_dir);
    {
        TrackedWriter writer(torn_dir, budgetOpts(1024 * 1024));
        writer.append(EntryType::Full, "k1", "val1", 4);
        writer.append(EntryType::Full, "k2", "val2", 4);
        writer.append(EntryType::Full, "k3", "val3", 4);

        // Append 17 bytes of torn junk at EOF (simulating partial write during crash)
        std::string seg_path = torn_dir + "/vlog_00000001.log";
        int fd = open(seg_path.c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        const char junk[] = "partial_torn_junk";
        checkedWrite(fd, junk, sizeof(junk) - 1);
        checkedFsync(fd);
        checkedClose(fd);
    }
    {
        VLogReader reader(torn_dir);
        auto sr = reader.scanSegment(1, /*is_last_segment=*/true);
        std::cout << "Recovered entries after last-segment torn tail: " << sr.entries.size() << " (Expected: 3)\n";
        CHECK(sr.entries.size() == 3);
        CHECK(sr.truncated == true);
        CHECK(sr.entries[0].key == "k1");
        CHECK(sr.entries[1].key == "k2");
        CHECK(sr.entries[2].key == "k3");
        std::cout << "PASS: Torn tail in last segment cleanly truncated with ftruncate + F_FULLFSYNC.\n";
    }

    // Case B: Bad tail in NON-LAST segment -> Must FAIL FATALLY (Rule 7)
    std::string non_last_dir = test_dir + "/torn_non_last";
    std::filesystem::create_directories(non_last_dir);
    {
        TrackedWriter writer(non_last_dir, budgetOpts(1024 * 1024));
        writer.append(EntryType::Full, "k1", "val1", 4);

        std::string seg_path = non_last_dir + "/vlog_00000001.log";
        int fd = open(seg_path.c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        const char junk[] = "partial_torn_junk";
        checkedWrite(fd, junk, sizeof(junk) - 1);
        checkedFsync(fd);
        checkedClose(fd);

        VLogReader reader(non_last_dir);
        bool caught_non_last_fatal = false;
        try {
            reader.scanSegment(1, /*is_last_segment=*/false);
        } catch (const std::runtime_error& e) {
            std::cout << "PASS: Non-last segment bad tail correctly rejected fatally: " << e.what() << "\n";
            caught_non_last_fatal = true;
        }
        CHECK(caught_non_last_fatal && "FATAL: Reader did not fail loudly on non-last segment torn tail!");
    }

    // Case C: Mid-Log Bad CRC (Valid entries follow bad CRC -> Must FAIL LOUDLY)
    std::string corrupt_dir = test_dir + "/corrupt_crc_sub";
    std::filesystem::create_directories(corrupt_dir);
    {
        TrackedWriter writer(corrupt_dir, budgetOpts(1024 * 1024));
        writer.append(EntryType::Full, "entry_1", "payload_1", 9);
        VPtr v2 = writer.append(EntryType::Full, "entry_2", "payload_2", 9);
        writer.append(EntryType::Full, "entry_3", "payload_3", 9);

        // Corrupt 1 byte in entry_2 payload
        std::string seg_path = corrupt_dir + "/vlog_00000001.log";
        int fd = open(seg_path.c_str(), O_RDWR);
        CHECK(fd >= 0);
        uint8_t corrupt_byte = 0xFF;
        checkedPwrite(fd, &corrupt_byte, 1, v2.offset + sizeof(EntryHeader) + 2);
        checkedFsync(fd);
        checkedClose(fd);

        VLogReader reader(corrupt_dir);
        bool caught_fatal = false;
        try {
            reader.scanSegment(1, /*is_last_segment=*/true);
        } catch (const std::runtime_error& e) {
            std::cout << "Caught expected fatal exception: " << e.what() << "\n";
            caught_fatal = true;
        }
        CHECK(caught_fatal && "FATAL: Reader did not fail loudly on mid-log CRC corruption!");
        std::cout << "PASS: Mid-log CRC corruption correctly identified and failed loudly without dropping data.\n";
    }

    // Case D: Mid-File Bounds Failure with valid entries ahead (Rule 8)
    std::string bounds_corrupt_dir = test_dir + "/corrupt_bounds_sub";
    std::filesystem::create_directories(bounds_corrupt_dir);
    {
        TrackedWriter writer(bounds_corrupt_dir, budgetOpts(1024 * 1024));
        writer.append(EntryType::Full, "e1", "p1", 2);
        VPtr v2 = writer.append(EntryType::Full, "e2", "p2", 2);
        writer.append(EntryType::Full, "e3", "p3", 2);

        // Corrupt total_len of entry_2 to be 15 (< 32, failing bounds)
        std::string seg_path = bounds_corrupt_dir + "/vlog_00000001.log";
        int fd = open(seg_path.c_str(), O_RDWR);
        CHECK(fd >= 0);
        uint32_t bad_total_len = 15;
        checkedPwrite(fd, &bad_total_len, sizeof(bad_total_len), v2.offset + 4);
        checkedFsync(fd);
        checkedClose(fd);

        VLogReader reader(bounds_corrupt_dir);
        bool caught_bounds_fatal = false;
        try {
            reader.scanSegment(1, /*is_last_segment=*/true);
        } catch (const std::runtime_error& e) {
            std::cout << "PASS: Mid-file bounds failure with entries ahead failed fatally: " << e.what() << "\n";
            caught_bounds_fatal = true;
        }
        CHECK(caught_bounds_fatal && "FATAL: Reader did not forward scan on bounds failure!");
    }
}

// -------------------------------------------------------------
// STAGE 4: Redo Rule Verification (should_apply & Idempotent Delta Replay)
// -------------------------------------------------------------
void testStage4RedoRule(const std::string& test_dir) {
    std::cout << "\n=======================================================\n";
    std::cout << "STAGE 4: Redo Rule (should_apply) & Idempotent Delta Replay\n";
    std::cout << "=======================================================\n";

    // 1. Basic Redo Rule Assertions
    std::map<std::string, IndexValueRef> test_index;
    auto apply_entry = [&](const std::string& key, uint64_t entry_seq, uint8_t flags, bool is_tombstone, VPtr vptr) {
        auto it = test_index.find(key);
        bool has_key = (it != test_index.end());
        uint64_t idx_seq = has_key ? it->second.seq : 0;

        if (should_apply(has_key, idx_seq, entry_seq, flags)) {
            if (is_tombstone) {
                test_index.erase(key);
            } else {
                test_index[key] = IndexValueRef{entry_seq, 1, {0,0,0}, vptr};
            }
            return true;
        }
        return false;
    };

    CHECK(apply_entry("doc_1", 100, kFlagNone, false, VPtr{1, 4096, 64}));
    CHECK(test_index["doc_1"].seq == 100);
    CHECK(apply_entry("doc_1", 105, kFlagNone, false, VPtr{1, 4160, 64}));
    CHECK(test_index["doc_1"].seq == 105);
    CHECK(!apply_entry("doc_1", 102, kFlagNone, false, VPtr{1, 4224, 64}));
    CHECK(test_index["doc_1"].seq == 105);
    CHECK(apply_entry("doc_1", 110, kFlagNone, true, VPtr{1, 4288, 32}));
    CHECK(test_index.find("doc_1") == test_index.end());

    // 2. Idempotent Double Replay with IncI64 Delta (Rule 6)
    std::string replay_dir = test_dir + "/redo_delta_replay";
    std::filesystem::create_directories(replay_dir);

    {
        TrackedWriter writer(replay_dir, budgetOpts(1024 * 1024));

        // Step 1: Base Full Document
        std::string init_val = "{\"balance\": 1000}";
        VPtr v1 = writer.append(EntryType::Full, "acct_99", init_val.data(), init_val.size());

        // Step 2: Delta IncI64 +50
        DeltaPrefix dp1{1, v1, 1, 1};
        DeltaOpHeader dop1{static_cast<uint8_t>(DeltaOpCode::IncI64), static_cast<uint8_t>(ValueEncoding::I64), 7, 8};
        std::string path = "balance";
        int64_t inc1 = 50;
        std::vector<uint8_t> payload1(sizeof(DeltaOpHeader) + dop1.path_len + sizeof(int64_t));
        std::memcpy(payload1.data(), &dop1, sizeof(dop1));
        std::memcpy(payload1.data() + sizeof(dop1), path.data(), dop1.path_len);
        std::memcpy(payload1.data() + sizeof(dop1) + dop1.path_len, &inc1, sizeof(int64_t));
        VPtr v2 = writer.append(EntryType::Delta, "acct_99", payload1.data(), payload1.size(), 0, 0, &dp1);

        // Step 3: Delta IncI64 +25
        DeltaPrefix dp2{2, v2, 2, 1};
        int64_t inc2 = 25;
        std::vector<uint8_t> payload2(sizeof(DeltaOpHeader) + dop1.path_len + sizeof(int64_t));
        std::memcpy(payload2.data(), &dop1, sizeof(dop1));
        std::memcpy(payload2.data() + sizeof(dop1), path.data(), dop1.path_len);
        std::memcpy(payload2.data() + sizeof(dop1) + dop1.path_len, &inc2, sizeof(int64_t));
        writer.append(EntryType::Delta, "acct_99", payload2.data(), payload2.size(), 0, 0, &dp2);
    }

    VLogReader reader(replay_dir);
    ScanResult sr = reader.scanSegment(1, true);
    CHECK(sr.entries.size() == 3);

    struct AccountState {
        uint64_t seq{0};
        int64_t balance{0};
    };
    std::map<std::string, AccountState> db_state;

    auto apply_log_entry = [&](const RecoveredEntry& rec) {
        bool has_key = (db_state.find(rec.key) != db_state.end());
        uint64_t cur_seq = has_key ? db_state[rec.key].seq : 0;
        uint8_t flags = rec.header.flags;
        uint64_t entry_seq = rec.header.seq;

        if (!should_apply(has_key, cur_seq, entry_seq, flags)) {
            return false; // Skip
        }

        if (rec.header.type == static_cast<uint8_t>(EntryType::Full)) {
            db_state[rec.key].balance = 1000;
            db_state[rec.key].seq = entry_seq;
        } else if (rec.header.type == static_cast<uint8_t>(EntryType::Delta)) {
            const uint8_t* p = rec.payload.data() + sizeof(DeltaPrefix);
            DeltaOpHeader dop;
            std::memcpy(&dop, p, sizeof(dop));
            if (dop.opcode == static_cast<uint8_t>(DeltaOpCode::IncI64)) {
                int64_t inc = 0;
                std::memcpy(&inc, p + sizeof(DeltaOpHeader) + dop.path_len, sizeof(int64_t));
                db_state[rec.key].balance += inc;
            }
            db_state[rec.key].seq = entry_seq;
        }
        return true;
    };

    // Pass 1: Initial Log Replay
    for (const auto& rec : sr.entries) {
        apply_log_entry(rec);
    }
    CHECK(db_state["acct_99"].balance == 1075);
    CHECK(db_state["acct_99"].seq == 3);

    // Pass 2: Replay Log Again (Must be strictly idempotent no-op)
    for (const auto& rec : sr.entries) {
        bool applied_again = apply_log_entry(rec);
        CHECK(!applied_again && "Second replay pass failed: entry was incorrectly re-applied!");
    }
    CHECK(db_state["acct_99"].balance == 1075);
    CHECK(db_state["acct_99"].seq == 3);

    std::cout << "PASS: should_apply() handles IncI64 Delta entries; second replay applied 0 of 3 entries; balance=1075, seq=3 after both passes.\n";
}

// -------------------------------------------------------------
// STAGE 5: Process-Crash Recovery Under kill -9 (50 Iterations)
// -------------------------------------------------------------
void testStage5ProcessCrash(const std::string& base_dir) {
    std::cout << "\n=======================================================\n";
    std::cout << "STAGE 5: Process-Crash Recovery Under kill -9 (50 Iterations)\n";
    std::cout << "=======================================================\n";

#if defined(TSAN_BUILD)
#define VLOG_SKIP_FORK_KILL 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define VLOG_SKIP_FORK_KILL 1
#endif
#endif

#if defined(VLOG_SKIP_FORK_KILL)
    std::cout << "[SKIP] Multi-threaded fork/kill -9 test skipped under ThreadSanitizer as required.\n";
    return;
#endif

    const int NUM_ITERATIONS = 50;
    const size_t MIN_ACKS_BEFORE_KILL = 200;
    std::mt19937 rng(1337);
    // Kill 5-400 ms after the 200th ack, so writes are in flight at kill time.
    // kill -9 cannot tear a single write() call, so torn tails may still be 0/50;
    // torn tails are covered by Test B.
    std::uniform_int_distribution<int> kill_delay_ms_dist(5, 400);

    size_t total_iterations_with_truncation = 0;
    uint64_t total_acked = 0;
    uint64_t total_recovered = 0;
    uint64_t min_unacked = UINT64_MAX;
    uint64_t max_unacked = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; ++iter) {
        std::string crash_dir = base_dir + "/crash_iter_" + std::to_string(iter);
        std::filesystem::create_directories(crash_dir);

        // Each child may use at most what is left of the global budget (and 100 MB).
        VLogWriter::Options child_opts = budgetOpts(1024 * 1024, 100 * 1024 * 1024); // 1 MB segments

        int pipefd[2];
        CHECK(pipe(pipefd) == 0);

        pid_t pid = fork();
        CHECK(pid >= 0);

        if (pid == 0) {
            // Child Worker Process. Plain VLogWriter: the parent accounts bytes from file sizes.
            CHECK(close(pipefd[0]) == 0);

            try {
                VLogWriter writer(crash_dir, child_opts);

                // Multi-threaded writers streaming through Group Commit
                const int WORKERS = 4;
                std::vector<std::thread> threads;

                for (int w = 0; w < WORKERS; ++w) {
                    threads.emplace_back([&, w]() {
                        for (int i = 0; i < 2000; ++i) {
                            std::string key = "c_t" + std::to_string(w) + "_k" + std::to_string(i);
                            uint32_t payload_len = 128;

                            // Rule 9: Populate payload deterministically derived from assigned seq
                            auto res = writer.append(EntryType::Full, key, nullptr, payload_len, 0, 0, nullptr,
                                [](uint64_t seq, uint8_t* buf, size_t len) {
                                    fill_deterministic_payload(seq, buf, len);
                                });

                            // Pipe acknowledged seq to parent ONLY after F_FULLFSYNC returns!
                            // 8-byte writes are below PIPE_BUF, so each one is atomic.
                            uint64_t ack_seq = res.seq;
                            CHECK(write(pipefd[1], &ack_seq, sizeof(ack_seq)) == static_cast<ssize_t>(sizeof(ack_seq)));
                        }
                    });
                }

                for (auto& t : threads) t.join();
            } catch (const std::exception& e) {
                std::cerr << "[Child] append failed: " << e.what() << "\n";
                _exit(2);  // parent treats any non-zero exit as a failure
            }
            CHECK(close(pipefd[1]) == 0);
            _exit(0);
        }

        // Parent Supervisor Process
        CHECK(close(pipefd[1]) == 0);

        // Rule 9: Kill ONLY after >= 200 acks are received from the child!
        std::vector<uint64_t> acked_seqs;
        while (acked_seqs.size() < MIN_ACKS_BEFORE_KILL) {
            uint64_t seq = 0;
            ssize_t r = read(pipefd[0], &seq, sizeof(seq));
            if (r == static_cast<ssize_t>(sizeof(seq))) {
                acked_seqs.push_back(seq);
            } else if (r == 0) {
                break;  // child exited early; checked via waitpid status below
            } else {
                CHECK(r < 0 && errno == EINTR);  // a short read of an 8-byte ack is a bug
            }
        }

        int kill_delay_ms = kill_delay_ms_dist(rng);
        std::this_thread::sleep_for(std::chrono::milliseconds(kill_delay_ms));

        // kill() also succeeds on an exited-but-unreaped (zombie) child.
        CHECK(kill(pid, SIGKILL) == 0);

        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        bool killed_by_us = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        bool finished_cleanly = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        CHECK((killed_by_us || finished_cleanly) && "Child failed before kill (abort or append error)");

        // Drain acks the child wrote before it died. Child is dead, so read() hits EOF after the data.
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        CHECK(flags != -1);
        CHECK(fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != -1);
        while (true) {
            uint64_t seq = 0;
            ssize_t r = read(pipefd[0], &seq, sizeof(seq));
            if (r == static_cast<ssize_t>(sizeof(seq))) {
                acked_seqs.push_back(seq);
            } else if (r == 0) {
                break;
            } else {
                CHECK(r < 0 && (errno == EAGAIN || errno == EINTR));
                if (errno == EAGAIN) break;
            }
        }
        CHECK(close(pipefd[0]) == 0);
        CHECK(acked_seqs.size() >= MIN_ACKS_BEFORE_KILL);

        // Account the bytes the child put on disk (before recovery truncates anything).
        std::vector<uint32_t> seg_ids;
        uint64_t child_bytes = 0;
        for (const auto& entry : std::filesystem::directory_iterator(crash_dir)) {
            std::string filename = entry.path().filename().string();
            if (filename.rfind("vlog_", 0) == 0 && filename.size() == 17) {
                uint32_t sid = static_cast<uint32_t>(std::stoul(filename.substr(5, 8)));
                seg_ids.push_back(sid);
                child_bytes += std::filesystem::file_size(entry.path());
            }
        }
        accountBytes(child_bytes, "Stage 5 child process");
        std::sort(seg_ids.begin(), seg_ids.end());

        // Run recovery across all generated segments
        VLogReader reader(crash_dir);
        std::vector<RecoveredEntry> all_recovered;

        bool iter_had_truncation = false;
        for (size_t idx = 0; idx < seg_ids.size(); ++idx) {
            uint32_t sid = seg_ids[idx];
            bool is_last = (idx == seg_ids.size() - 1);
            ScanResult sr = reader.scanSegment(sid, is_last);
            if (is_last && sr.truncated) {
                iter_had_truncation = true;
            }
            all_recovered.insert(all_recovered.end(), sr.entries.begin(), sr.entries.end());
        }

        if (iter_had_truncation) {
            total_iterations_with_truncation++;
        }

        // Assert 1: Recovered sequences must be strictly increasing
        for (size_t s = 1; s < all_recovered.size(); ++s) {
            uint64_t cur_s = all_recovered[s].header.seq;
            uint64_t prev_s = all_recovered[s - 1].header.seq;
            CHECK(cur_s > prev_s && "Recovered seqs are not strictly increasing!");
        }

        // Build set of recovered sequences and verify payload bytes for each (Rule 9)
        std::map<uint64_t, bool> recovered_map;
        for (const auto& rec : all_recovered) {
            uint64_t rec_seq = rec.header.seq;
            recovered_map[rec_seq] = true;

            // Rule 9: Verify payload bytes derived from seq
            std::vector<uint8_t> expected_payload(rec.header.payload_len);
            fill_deterministic_payload(rec_seq, expected_payload.data(), expected_payload.size());
            CHECK(rec.payload == expected_payload && "FATAL: Recovered payload bytes do not match expected deterministic sequence!");
        }

        // Assert 2: Every single acked seq received through the pipe MUST be recovered
        for (uint64_t acked : acked_seqs) {
            CHECK(recovered_map.find(acked) != recovered_map.end() &&
                  "FATAL: Acknowledged write was lost after kill -9!");
        }

        // Recovered but never acked: written (and possibly fsynced) when the kill landed.
        uint64_t unacked = all_recovered.size() - acked_seqs.size();
        total_acked += acked_seqs.size();
        total_recovered += all_recovered.size();
        min_unacked = std::min(min_unacked, unacked);
        max_unacked = std::max(max_unacked, unacked);

        std::printf("Iter %2d/%d: kill at +%3d ms, %s, acked %5zu, recovered %5zu, recovered-but-unacked %3llu, segments %zu%s\n",
                    iter + 1, NUM_ITERATIONS, kill_delay_ms,
                    killed_by_us ? "SIGKILL" : "child finished before kill",
                    acked_seqs.size(), all_recovered.size(),
                    (unsigned long long)unacked, seg_ids.size(),
                    iter_had_truncation ? " [torn tail truncated]" : "");
    }

    std::cout << "PASS: " << NUM_ITERATIONS << " kill -9 iterations; all " << total_acked
              << " acked seqs were recovered (total recovered " << total_recovered << ").\n";
    std::cout << "Report: recovered-but-unacked per iteration: min " << min_unacked
              << ", max " << max_unacked << ".\n";
    std::cout << "Report: Torn-tail truncation was triggered in "
              << total_iterations_with_truncation << " / " << NUM_ITERATIONS << " crash iterations.\n";
}

// -------------------------------------------------------------
// TEST A: Reopen No Truncation & O_CREAT|O_EXCL Rollover (Rule 1)
// -------------------------------------------------------------
void testReopenNoTruncation(const std::string& test_dir) {
    std::cout << "\n=======================================================\n";
    std::cout << "TEST A: Reopen No Truncation & O_CREAT|O_EXCL Rollover (Rule 1)\n";
    std::cout << "=======================================================\n";

    std::filesystem::create_directories(test_dir);
    {
        TrackedWriter writer(test_dir, budgetOpts(10 * 1024 * 1024));
        for (int i = 1; i <= 100; ++i) {
            std::string k = "k_" + std::to_string(i);
            std::string v = "payload_" + std::to_string(i);
            auto res = writer.append(EntryType::Full, k, v.data(), v.size());
            CHECK(res.seq == static_cast<uint64_t>(i));
        }
        CHECK(writer.getCurrentSeq() == 100);
        CHECK(writer.getCurrentSegmentId() == 1);
        // Destructor closes segment 1
    }

    {
        // Reopen writer: must scan existing segments via VLogReader, find max_seq=100, max_seg=1,
        // and open NEW segment 2 with O_CREAT | O_EXCL (never O_TRUNC).
        TrackedWriter writer(test_dir, budgetOpts(10 * 1024 * 1024));
        CHECK(writer.getCurrentSegmentId() == 2);
        CHECK(writer.getCurrentSeq() == 100);

        auto res = writer.append(EntryType::Full, "k_101", "payload_101", 11);
        CHECK(res.seq == 101);
        CHECK(res.vptr.segment_id == 2);
        CHECK(writer.getCurrentSeq() == 101);
    }

    // Now verify recovery: scan both segment 1 and segment 2
    {
        VLogReader reader(test_dir);
        auto sr1 = reader.scanSegment(1, /*is_last_segment=*/false);
        CHECK(sr1.entries.size() == 100);

        auto sr2 = reader.scanSegment(2, /*is_last_segment=*/true);
        CHECK(sr2.entries.size() == 1);
        CHECK(sr2.entries[0].header.seq == 101);
        CHECK(sr2.entries[0].key == "k_101");

        size_t total_recovered = sr1.entries.size() + sr2.entries.size();
        CHECK(total_recovered == 101);
        std::cout << "PASS: Recovered 101 entries across segments 1 and 2, new seq = 101. Zero truncation.\n";
    }
}

// -------------------------------------------------------------
// TEST B: Torn-Write Fault Injection Matrix (Rule 6, 7, 8)
// -------------------------------------------------------------
void testTornWriteFaultInjection(const std::string& test_dir) {
    std::cout << "\n=======================================================\n";
    std::cout << "TEST B: Torn-Write Fault Injection Matrix (Rule 6, 7, 8)\n";
    std::cout << "=======================================================\n";

    // Subcase 1: Torn SegmentHeader creation (< 4096 bytes)
    {
        std::string sub_dir = test_dir + "/torn_seg_hdr";
        std::filesystem::create_directories(sub_dir);
        std::string seg_file = sub_dir + "/vlog_00000001.log";
        int fd = open(seg_file.c_str(), O_RDWR | O_CREAT, 0644);
        CHECK(fd >= 0);
        char partial[128];
        std::memset(partial, 0xAA, sizeof(partial));
        checkedWrite(fd, partial, sizeof(partial));
        checkedFsync(fd);
        checkedClose(fd);

        VLogReader reader(sub_dir);
        // Last segment: truncated to 0, returns empty result
        auto sr = reader.scanSegment(1, /*is_last=*/true);
        CHECK(sr.entries.empty());
        CHECK(sr.truncated);
        struct stat st;
        CHECK(stat(seg_file.c_str(), &st) == 0);
        CHECK(st.st_size == 0);

        // Non-last segment: fatal exception
        fd = open(seg_file.c_str(), O_RDWR | O_CREAT, 0644);
        CHECK(fd >= 0);
        checkedWrite(fd, partial, sizeof(partial));
        checkedClose(fd);
        bool caught = false;
        try {
            reader.scanSegment(1, /*is_last=*/false);
        } catch (const std::runtime_error& e) {
            caught = true;
        }
        CHECK(caught && "Fatal error expected for torn header in non-last segment");
    }

    // Subcase 2: Corrupted SegmentHeader magic/CRC in last vs non-last segment (Rule 6)
    {
        std::string sub_dir = test_dir + "/corrupt_seg_hdr";
        std::filesystem::create_directories(sub_dir);
        std::string seg_file = sub_dir + "/vlog_00000001.log";
        int fd = open(seg_file.c_str(), O_RDWR | O_CREAT, 0644);
        CHECK(fd >= 0);
        SegmentHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.magic = 0xDEADBEEF; // Corrupt magic
        hdr.crc32c = 0x1234;
        char block[kBlockSize];
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &hdr, sizeof(hdr));
        checkedWrite(fd, block, kBlockSize);
        checkedFsync(fd);
        checkedClose(fd);

        VLogReader reader(sub_dir);
        // Rule 6: Last segment with bad magic -> truncated to 0, halt scan immediately
        auto sr = reader.scanSegment(1, /*is_last=*/true);
        CHECK(sr.entries.empty());
        CHECK(sr.truncated);
        struct stat st;
        CHECK(stat(seg_file.c_str(), &st) == 0);
        CHECK(st.st_size == 0);

        // Non-last segment: throws fatal exception
        fd = open(seg_file.c_str(), O_RDWR | O_CREAT, 0644);
        CHECK(fd >= 0);
        checkedWrite(fd, block, kBlockSize);
        checkedClose(fd);
        bool caught = false;
        try {
            reader.scanSegment(1, /*is_last=*/false);
        } catch (const std::runtime_error& e) {
            caught = true;
        }
        CHECK(caught && "Fatal error expected for corrupt header in non-last segment");
    }

    // Subcase 3: Partial EntryHeader write at EOF
    {
        std::string sub_dir = test_dir + "/torn_entry_hdr";
        std::filesystem::create_directories(sub_dir);
        {
            TrackedWriter writer(sub_dir, budgetOpts());
            writer.append(EntryType::Full, "valid1", "p1", 2);
            writer.append(EntryType::Full, "valid2", "p2", 2);
        }
        std::string seg_file = sub_dir + "/vlog_00000001.log";
        int fd = open(seg_file.c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        char partial_hdr[16]; // Half of 32-byte header
        std::memset(partial_hdr, 0x55, sizeof(partial_hdr));
        checkedWrite(fd, partial_hdr, sizeof(partial_hdr));
        checkedFsync(fd);
        checkedClose(fd);

        VLogReader reader(sub_dir);
        auto sr = reader.scanSegment(1, /*is_last=*/true);
        CHECK(sr.entries.size() == 2);
        CHECK(sr.truncated);
    }

    // Subcase 4: Incomplete entry payload write at EOF
    {
        std::string sub_dir = test_dir + "/torn_payload";
        std::filesystem::create_directories(sub_dir);
        {
            TrackedWriter writer(sub_dir, budgetOpts());
            writer.append(EntryType::Full, "validA", "pA", 2);
        }
        std::string seg_file = sub_dir + "/vlog_00000001.log";
        int fd = open(seg_file.c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        EntryHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.total_len = 128;
        hdr.type = static_cast<uint8_t>(EntryType::Full);
        hdr.payload_len = 80;
        // Write header + only 10 bytes of payload (out of 128 total)
        checkedWrite(fd, &hdr, sizeof(hdr));
        char half_p[10];
        std::memset(half_p, 0x11, sizeof(half_p));
        checkedWrite(fd, half_p, sizeof(half_p));
        checkedFsync(fd);
        checkedClose(fd);

        VLogReader reader(sub_dir);
        auto sr = reader.scanSegment(1, /*is_last=*/true);
        CHECK(sr.entries.size() == 1);
        CHECK(sr.truncated);
        CHECK(sr.entries[0].key == "validA");
    }

    std::cout << "PASS: Torn-Write Fault Injection Matrix validated for headers, tails, and non-last segment rules.\n";
}

// -------------------------------------------------------------
// TEST C: Mid-Checkpoint Redo & Tombstone State Machine (Rule 6)
// -------------------------------------------------------------
void testMidCheckpointRedoTombstone(const std::string& test_dir) {
    std::cout << "\n=======================================================\n";
    std::cout << "TEST C: Mid-Checkpoint Redo & Tombstone State Machine (Rule 6)\n";
    std::cout << "=======================================================\n";

    std::filesystem::create_directories(test_dir);
    {
        TrackedWriter writer(test_dir, budgetOpts());
        // Write log entries simulating operations before, during, and after a checkpoint
        writer.append(EntryType::Full, "key_A", "val_A_v1", 8);      // seq 1
        writer.append(EntryType::Full, "key_B", "val_B_v1", 8);      // seq 2
        writer.append(EntryType::Full, "key_C", "val_C_v1", 8);      // seq 3
        writer.append(EntryType::Full, "key_D", "val_D_v1", 8);      // seq 4
        writer.append(EntryType::Full, "key_A", "val_A_v2", 8);      // seq 5
        writer.append(EntryType::Tombstone, "key_C", nullptr, 0);    // seq 6: delete key_C
        writer.append(EntryType::Full, "key_B", "val_B_v2", 8);      // seq 7
        writer.append(EntryType::Full, "key_E", "val_E_v1", 8);      // seq 8: new key
        writer.append(EntryType::Full, "key_C", "val_C_v2", 8);      // seq 9: resurrected key_C
        writer.append(EntryType::Tombstone, "key_D", nullptr, 0);    // seq 10: delete key_D
    }

    struct DbRecord {
        uint64_t seq{0};
        std::string val;
        bool deleted{false};
    };
    std::map<std::string, DbRecord> simulated_checkpoint;
    simulated_checkpoint["key_A"] = {5, "val_A_v2", false};
    simulated_checkpoint["key_B"] = {2, "val_B_v1", false};
    simulated_checkpoint["key_C"] = {3, "val_C_v1", false};
    simulated_checkpoint["key_D"] = {4, "val_D_v1", false};

    VLogReader reader(test_dir);
    auto sr = reader.scanSegment(1, /*is_last=*/true);
    CHECK(sr.entries.size() == 10);

    auto apply_entry = [&](const RecoveredEntry& e, std::map<std::string, DbRecord>& db) -> bool {
        bool has_key = (db.find(e.key) != db.end());
        uint64_t cur_seq = has_key ? db[e.key].seq : 0;
        uint8_t flags = e.header.flags;
        uint64_t entry_seq = e.header.seq;

        if (!should_apply(has_key, cur_seq, entry_seq, flags)) {
            return false;
        }

        if (e.header.type == static_cast<uint8_t>(EntryType::Tombstone)) {
            db[e.key].seq = entry_seq;
            db[e.key].deleted = true;
            db[e.key].val.clear();
        } else {
            db[e.key].seq = entry_seq;
            db[e.key].deleted = false;
            db[e.key].val.assign(reinterpret_cast<const char*>(e.payload.data()), e.payload.size());
        }
        return true;
    };

    // Redo recovery scan
    int applied_count = 0;
    for (const auto& e : sr.entries) {
        if (apply_entry(e, simulated_checkpoint)) {
            applied_count++;
        }
    }

    // Seqs applied: 6, 7, 8, 9, 10 -> exactly 5 entries applied
    CHECK(applied_count == 5);

    // Verify final database state
    CHECK(simulated_checkpoint["key_A"].seq == 5 && simulated_checkpoint["key_A"].val == "val_A_v2");
    CHECK(simulated_checkpoint["key_B"].seq == 7 && simulated_checkpoint["key_B"].val == "val_B_v2");
    CHECK(simulated_checkpoint["key_C"].seq == 9 && simulated_checkpoint["key_C"].val == "val_C_v2" && !simulated_checkpoint["key_C"].deleted);
    CHECK(simulated_checkpoint["key_D"].seq == 10 && simulated_checkpoint["key_D"].deleted);
    CHECK(simulated_checkpoint["key_E"].seq == 8 && simulated_checkpoint["key_E"].val == "val_E_v1");

    // Idempotence: replay the entire log a second time -> 0 entries applied, state unchanged!
    for (const auto& e : sr.entries) {
        bool re_applied = apply_entry(e, simulated_checkpoint);
        CHECK(!re_applied && "Replay pass 2 must not re-apply any entry!");
    }

    CHECK(simulated_checkpoint["key_A"].seq == 5);
    CHECK(simulated_checkpoint["key_B"].seq == 7);
    CHECK(simulated_checkpoint["key_C"].seq == 9);
    CHECK(simulated_checkpoint["key_D"].seq == 10 && simulated_checkpoint["key_D"].deleted);
    CHECK(simulated_checkpoint["key_E"].seq == 8);

    std::cout << "PASS: Mid-checkpoint redo applied 5 of 10 entries (seqs 6-10); second replay applied 0; final state of key_A..key_E matches expected.\n";
}

// -------------------------------------------------------------
// STAGE 6: Multi-Threaded Group Commit Benchmark (1, 4, 16, 64 Threads)
// -------------------------------------------------------------
void testStage6Benchmark(const std::string& test_dir) {
    std::cout << "\n=======================================================\n";
    std::cout << "STAGE 6: Multi-Threaded Group Commit Benchmark (1, 4, 16, 64 Threads)\n";
    std::cout << "=======================================================\n";

    std::vector<int> thread_counts = {1, 4, 16, 64};

    std::cout << "\n+---------+---------+------------------+-----------------------+------------+-------------+-----------------+\n";
    std::cout << "| Threads | Runtime | Committed Writes | Physical F_FULLFSYNCs |  Writes/s  | Barriers/s  | Avg Batch Size  |\n";
    std::cout << "+---------+---------+------------------+-----------------------+------------+-------------+-----------------+\n";

    for (int num_threads : thread_counts) {
        std::string bench_dir = test_dir + "/bench_threads_" + std::to_string(num_threads);
        std::filesystem::create_directories(bench_dir);

        // 256 MB local cap, further limited by what is left of the global 512 MiB budget.
        TrackedWriter writer(bench_dir, budgetOpts(64 * 1024 * 1024, 256 * 1024 * 1024));
        std::atomic<bool> stop_flag{false};
        std::atomic<bool> append_failed{false};

        auto worker = [&](int tid) {
            size_t local_idx = 0;
            while (!stop_flag.load(std::memory_order_relaxed)) {
                std::string key = "b_t" + std::to_string(tid) + "_k" + std::to_string(local_idx++);
                std::string payload = "{\"t\": " + std::to_string(tid) + ", \"idx\": " + std::to_string(local_idx) + "}";
                try {
                    writer.append(EntryType::Full, key, payload.data(), payload.size());
                } catch (const std::exception& e) {
                    // Includes hitting the byte cap: the benchmark must not pass silently on that.
                    std::cerr << "[Stage 6] append failed: " << e.what() << "\n";
                    append_failed.store(true);
                    break;
                }
            }
        };

        auto start_time = std::chrono::high_resolution_clock::now();
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back(worker, t);
        }

        // Run each benchmark configuration for 1.5 seconds (~6.0s total runtime, within 10s cap)
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        stop_flag.store(true);

        for (auto& th : threads) {
            th.join();
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        CHECK(!append_failed.load() && "Stage 6 append failed (see message above)");

        std::chrono::duration<double> duration = end_time - start_time;
        double elapsed_sec = duration.count();

        uint64_t total_ops = writer.getTotalOps();
        uint64_t total_fsyncs = writer.getTotalFsyncs();
        double avg_batch_size = writer.getAverageBatchSize();

        std::printf("| %7d | %6.2fs | %16llu | %21llu | %10.1f | %11.1f | %15.2f |\n",
                    num_threads, elapsed_sec,
                    (unsigned long long)total_ops,
                    (unsigned long long)total_fsyncs,
                    total_ops / elapsed_sec,
                    total_fsyncs / elapsed_sec,
                    avg_batch_size);
    }
    std::cout << "+---------+---------+------------------+-----------------------+------------+-------------+-----------------+\n";
    std::cout << "PASS: Group Commit scaling benchmark completed across 1, 4, 16, and 64 threads.\n";
}

int main() {
    std::cout << "====================================================================\n";
    std::cout << "  VLOG ENGINE COMPREHENSIVE CRASH RECOVERY & GROUP COMMIT TEST      \n";
    std::cout << "====================================================================\n";

    // Setup isolated temp directory
    char tmp_template[] = "/tmp/vlog_test_XXXXXX";
    char* temp_dir_path = mkdtemp(tmp_template);
    CHECK(temp_dir_path != nullptr);
    std::string test_dir = temp_dir_path;
    std::cout << "[Setup] Test Directory: " << test_dir << "\n";

    // On any abort, print the temp dir so it can be removed by hand
    // (abort skips the Cleaner destructor below).
    CHECK(test_dir.size() < sizeof(g_test_dir));
    std::memcpy(g_test_dir, test_dir.c_str(), test_dir.size() + 1);
    CHECK(std::signal(SIGABRT, onAbortSignal) != SIG_ERR);

    // Hardware free space check (explicit if + abort)
    checkHardwareSafety(test_dir);

    // RAII Cleanup on normal exit
    struct Cleaner {
        std::string dir;
        ~Cleaner() {
            std::cout << "[Cleanup] Removing temp test directory: " << dir << "\n";
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            if (ec) {
                std::cerr << "[Cleanup] remove_all failed (" << ec.message() << "). Delete manually: rm -rf " << dir << "\n";
            }
        }
    } cleaner{test_dir};

    // Execute all stages
    testStage1CRC();
    testStage2HeaderBounds(test_dir + "/bounds_test");
    testStage3TornTailAndCorruption(test_dir + "/torn_test");
    testStage4RedoRule(test_dir + "/redo_test");
    testReopenNoTruncation(test_dir + "/reopen_test");
    testTornWriteFaultInjection(test_dir + "/fault_test");
    testMidCheckpointRedoTombstone(test_dir + "/mid_checkpoint_test");
    testStage5ProcessCrash(test_dir + "/crash_50");
    testStage6Benchmark(test_dir + "/bench");

    std::cout << "\n[Budget] Total bytes written by all stages: " << g_bytes_written.load()
              << " of " << kWriteBudgetBytes << " allowed (512 MiB).\n";

    std::cout << "\n====================================================================\n";
    std::cout << "  ALL VLOG ENGINE DURABILITY & RECOVERY TESTS PASSED!               \n";
    std::cout << "====================================================================\n";

    return 0;
}
