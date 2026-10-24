// vLog tests: CRC32C, reader bounds checks, torn-tail recovery, the redo rule, segment rollover,
// torn segment creation, kill -9 crash recovery with group commit, and a group commit benchmark.
#include "test_common.hpp"

#include "vlog/CRC32.hpp"
#include "vlog/VLogFormat.hpp"
#include "vlog/VLogReader.hpp"
#include "vlog/VLogWriter.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace testutil;
using namespace vlog;

// Writer options whose max_bytes never exceeds what is left of the global budget.
static VLogWriter::Options budgetOpts(uint32_t segment_size_limit = 64 * 1024 * 1024,
                                      uint64_t local_cap = 0) {
    uint64_t cap = remainingBudget();
    if (local_cap > 0) cap = std::min(cap, local_cap);
    return VLogWriter::Options(segment_size_limit, cap);
}

// VLogWriter that adds its byte count to the budget when it goes out of scope.
// Never deleted through a base pointer.
class TrackedWriter : public VLogWriter {
public:
    using VLogWriter::VLogWriter;
    ~TrackedWriter() { accountBytes(getTotalBytesWritten(), "VLogWriter"); }
};

static void checkedWrite(int fd, const void* buf, size_t n) {
    CHECK(write(fd, buf, n) == static_cast<ssize_t>(n));
    accountBytes(n, "write()");
}

static void checkedPwrite(int fd, const void* buf, size_t n, off_t off) {
    CHECK(pwrite(fd, buf, n, off) == static_cast<ssize_t>(n));
    accountBytes(n, "pwrite()");
}

static void checkedFsync(int fd) {
    CHECK(full_fsync(fd) == 0);
}
static void checkedClose(int fd) {
    CHECK(close(fd) == 0);
}

#ifndef TSAN_BUILD // used only by the kill -9 test, which TSan builds skip
// Payload bytes derived from the seq, so recovery can verify every recovered byte.
static void fillDeterministicPayload(uint64_t seq, uint8_t* buf, size_t len) {
    std::string prefix = "SEQ:" + std::to_string(seq) + ":";
    size_t prefix_len = std::min(prefix.size(), len);
    std::memcpy(buf, prefix.data(), prefix_len);
    for (size_t i = prefix_len; i < len; ++i) {
        buf[i] = static_cast<uint8_t>((seq * 1315423911ULL + i * 2654435761ULL) & 0xFF);
    }
}
#endif

static std::vector<uint32_t> segmentIds(const std::string& dir) {
    std::vector<uint32_t> ids;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::string n = e.path().filename().string();
        if (n.rfind("vlog_", 0) == 0 && n.size() == 17)
            ids.push_back(static_cast<uint32_t>(std::stoul(n.substr(5, 8))));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// ------------------------------------------------------------------------------------------
static void testV1Crc() {
    banner("V1 CRC32C test vector");
    const char* str = "123456789";
    uint32_t t_res = crc32c_table(str, 9);
    uint32_t hw_res = crc32c_hardware(str, 9);
    detail() << "table 0x" << std::hex << t_res << ", hardware 0x" << hw_res << std::dec << "\n";
    CHECK(t_res == 0xE3069283 && hw_res == 0xE3069283);
    std::mt19937 rng(42);
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> payload(100 + (rng() % 1000));
        for (auto& b : payload) b = static_cast<uint8_t>(rng() & 0xFF);
        CHECK(crc32c_table(payload.data(), payload.size()) ==
              crc32c_hardware(payload.data(), payload.size()));
    }
    pass("V1 CRC32C: ARMv8 and table versions give 0xe3069283 and agree on 100 random payloads");
}

static void testV2HeaderBounds(const std::string& test_dir) {
    banner("V2 Reader rejects corrupt total_len");
    TrackedWriter writer(test_dir, budgetOpts(1024 * 1024, 10 * 1024 * 1024));
    std::string val = "{\"test\": \"payload\"}";
    VPtr vptr = writer.append(EntryType::Full, "user_test_bounds", val.data(), val.size());
    VLogReader reader(test_dir);
    EntryHeader hdr;
    std::string r_key;
    std::vector<uint8_t> r_val;
    CHECK(reader.readEntry(vptr, hdr, r_key, r_val));

    // The VPtr length is set to the corrupt value too, so the bounds check (not the length
    // comparison) is what must reject the entry.
    int fd = open((test_dir + "/vlog_00000001.log").c_str(), O_RDWR);
    CHECK(fd >= 0);
    auto rejects = [&](uint32_t bad_len) {
        checkedPwrite(fd, &bad_len, sizeof(bad_len), vptr.offset + 4);
        checkedFsync(fd);
        VPtr v = vptr;
        v.length = bad_len;
        CHECK(!reader.readEntry(v, hdr, r_key, r_val));
    };
    rejects(16);               // < sizeof(EntryHeader)
    rejects(45);               // not 8-aligned
    rejects(32 * 1024 * 1024); // > kMaxEntryLen
    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    rejects(static_cast<uint32_t>(align_up(st.st_size - vptr.offset + 8, kEntryAlign))); // past EOF
    checkedPwrite(fd, &vptr.length, sizeof(vptr.length), vptr.offset + 4);
    checkedFsync(fd);
    checkedClose(fd);
    CHECK(reader.readEntry(vptr, hdr, r_key, r_val));
    pass("V2 reader rejects total_len < 32, unaligned, > 16 MiB, past EOF");
}

static void testV3TornTailAndCorruption(const std::string& test_dir) {
    banner("V3 Torn tail vs mid-log corruption");
    const char junk[] = "partial_torn_junk";
    // A: torn tail in the last segment is truncated.
    std::string torn_dir = test_dir + "/torn_tail_last";
    std::filesystem::create_directories(torn_dir);
    {
        TrackedWriter writer(torn_dir, budgetOpts(1024 * 1024));
        writer.append(EntryType::Full, "k1", "val1", 4);
        writer.append(EntryType::Full, "k2", "val2", 4);
        writer.append(EntryType::Full, "k3", "val3", 4);
        int fd = open((torn_dir + "/vlog_00000001.log").c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        checkedWrite(fd, junk, sizeof(junk) - 1);
        checkedFsync(fd);
        checkedClose(fd);
    }
    {
        VLogReader reader(torn_dir);
        auto sr = reader.scanSegment(1, /*is_last_segment=*/true);
        CHECK(sr.entries.size() == 3 && sr.truncated);
        CHECK(sr.entries[0].key == "k1" && sr.entries[1].key == "k2" && sr.entries[2].key == "k3");
    }
    // B: the same bad tail in a non-last segment is fatal.
    std::string non_last_dir = test_dir + "/torn_non_last";
    std::filesystem::create_directories(non_last_dir);
    {
        TrackedWriter writer(non_last_dir, budgetOpts(1024 * 1024));
        writer.append(EntryType::Full, "k1", "val1", 4);
        int fd = open((non_last_dir + "/vlog_00000001.log").c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        checkedWrite(fd, junk, sizeof(junk) - 1);
        checkedFsync(fd);
        checkedClose(fd);
        VLogReader reader(non_last_dir);
        bool caught = false;
        try {
            reader.scanSegment(1, /*is_last_segment=*/false);
        } catch (const std::runtime_error& e) {
            detail() << "non-last segment: " << e.what() << "\n";
            caught = true;
        }
        CHECK(caught);
    }
    // C: bad CRC with valid entries after it is mid-log corruption: fatal, nothing dropped.
    std::string corrupt_dir = test_dir + "/corrupt_crc_sub";
    std::filesystem::create_directories(corrupt_dir);
    {
        TrackedWriter writer(corrupt_dir, budgetOpts(1024 * 1024));
        writer.append(EntryType::Full, "entry_1", "payload_1", 9);
        VPtr v2 = writer.append(EntryType::Full, "entry_2", "payload_2", 9);
        writer.append(EntryType::Full, "entry_3", "payload_3", 9);
        int fd = open((corrupt_dir + "/vlog_00000001.log").c_str(), O_RDWR);
        CHECK(fd >= 0);
        uint8_t corrupt_byte = 0xFF;
        checkedPwrite(fd, &corrupt_byte, 1, v2.offset + sizeof(EntryHeader) + 2);
        checkedFsync(fd);
        checkedClose(fd);
        VLogReader reader(corrupt_dir);
        bool caught = false;
        try {
            reader.scanSegment(1, /*is_last_segment=*/true);
        } catch (const std::runtime_error& e) {
            detail() << "mid-log CRC: " << e.what() << "\n";
            caught = true;
        }
        CHECK(caught);
    }
    // D: a bounds failure with valid entries after it uses the same forward scan: fatal.
    std::string bounds_dir = test_dir + "/corrupt_bounds_sub";
    std::filesystem::create_directories(bounds_dir);
    {
        TrackedWriter writer(bounds_dir, budgetOpts(1024 * 1024));
        writer.append(EntryType::Full, "e1", "p1", 2);
        VPtr v2 = writer.append(EntryType::Full, "e2", "p2", 2);
        writer.append(EntryType::Full, "e3", "p3", 2);
        int fd = open((bounds_dir + "/vlog_00000001.log").c_str(), O_RDWR);
        CHECK(fd >= 0);
        uint32_t bad_total_len = 15;
        checkedPwrite(fd, &bad_total_len, sizeof(bad_total_len), v2.offset + 4);
        checkedFsync(fd);
        checkedClose(fd);
        VLogReader reader(bounds_dir);
        bool caught = false;
        try {
            reader.scanSegment(1, /*is_last_segment=*/true);
        } catch (const std::runtime_error& e) {
            detail() << "mid-log bounds: " << e.what() << "\n";
            caught = true;
        }
        CHECK(caught);
    }
    pass("V3 torn tail truncated in the last segment; fatal in a non-last segment; mid-log CRC and "
         "bounds "
         "corruption fatal");
}

static void testV4RedoRule(const std::string& test_dir) {
    banner("V4 Redo rule and idempotent delta replay");
    std::map<std::string, IndexValueRef> index;
    auto apply = [&](const std::string& key, uint64_t seq, bool tombstone, VPtr vptr) {
        auto it = index.find(key);
        bool has = it != index.end();
        if (!should_apply(has, has ? it->second.seq : 0, seq, kFlagNone)) return false;
        if (tombstone)
            index.erase(key);
        else
            index[key] = IndexValueRef{seq, 1, {0, 0, 0}, vptr};
        return true;
    };
    CHECK(apply("doc_1", 100, false, VPtr{1, 4096, 64}) && index["doc_1"].seq == 100);
    CHECK(apply("doc_1", 105, false, VPtr{1, 4160, 64}) && index["doc_1"].seq == 105);
    CHECK(!apply("doc_1", 102, false, VPtr{1, 4224, 64}) && index["doc_1"].seq == 105);
    CHECK(apply("doc_1", 110, true, VPtr{1, 4288, 32}) && index.find("doc_1") == index.end());

    std::string replay_dir = test_dir + "/redo_delta_replay";
    std::filesystem::create_directories(replay_dir);
    {
        TrackedWriter writer(replay_dir, budgetOpts(1024 * 1024));
        std::string init_val = "{\"balance\": 1000}";
        VPtr v1 = writer.append(EntryType::Full, "acct_99", init_val.data(), init_val.size());
        DeltaOpHeader dop{static_cast<uint8_t>(DeltaOpCode::IncI64),
                          static_cast<uint8_t>(ValueEncoding::I64), 7, 8};
        const std::string path = "balance";
        auto deltaPayload = [&](int64_t inc) {
            std::vector<uint8_t> p(sizeof(DeltaOpHeader) + dop.path_len + sizeof(int64_t));
            std::memcpy(p.data(), &dop, sizeof(dop));
            std::memcpy(p.data() + sizeof(dop), path.data(), dop.path_len);
            std::memcpy(p.data() + sizeof(dop) + dop.path_len, &inc, sizeof(int64_t));
            return p;
        };
        DeltaPrefix dp1{1, v1, 1, 1};
        std::vector<uint8_t> p1 = deltaPayload(50);
        VPtr v2 = writer.append(EntryType::Delta, "acct_99", p1.data(), p1.size(), 0, 0, &dp1);
        DeltaPrefix dp2{2, v2, 2, 1};
        std::vector<uint8_t> p2 = deltaPayload(25);
        writer.append(EntryType::Delta, "acct_99", p2.data(), p2.size(), 0, 0, &dp2);
    }
    VLogReader reader(replay_dir);
    ScanResult sr = reader.scanSegment(1, true);
    CHECK(sr.entries.size() == 3);

    struct Account {
        uint64_t seq{0};
        int64_t balance{0};
    };
    std::map<std::string, Account> db;
    auto applyEntry = [&](const RecoveredEntry& rec) {
        bool has = db.find(rec.key) != db.end();
        uint64_t entry_seq = rec.header.seq;
        if (!should_apply(has, has ? db[rec.key].seq : 0, entry_seq, rec.header.flags))
            return false;
        if (rec.header.type == static_cast<uint8_t>(EntryType::Full)) {
            db[rec.key].balance = 1000;
        } else if (rec.header.type == static_cast<uint8_t>(EntryType::Delta)) {
            const uint8_t* p = rec.payload.data() + sizeof(DeltaPrefix);
            DeltaOpHeader op;
            std::memcpy(&op, p, sizeof(op));
            if (op.opcode == static_cast<uint8_t>(DeltaOpCode::IncI64)) {
                int64_t inc = 0;
                std::memcpy(&inc, p + sizeof(DeltaOpHeader) + op.path_len, sizeof(int64_t));
                db[rec.key].balance += inc;
            }
        }
        db[rec.key].seq = entry_seq;
        return true;
    };
    for (const auto& rec : sr.entries) applyEntry(rec);
    CHECK(db["acct_99"].balance == 1075 && db["acct_99"].seq == 3);
    for (const auto& rec : sr.entries) CHECK(!applyEntry(rec)); // second replay is a no-op
    CHECK(db["acct_99"].balance == 1075 && db["acct_99"].seq == 3);
    pass("V4 should_apply: older seqs skipped, tombstones erase; a second replay of 3 delta "
         "entries applies 0");
}

static void testV5ReopenNoTruncation(const std::string& test_dir) {
    banner("V5 Reopen opens a new segment (O_CREAT|O_EXCL) and keeps old entries");
    std::filesystem::create_directories(test_dir);
    {
        TrackedWriter writer(test_dir, budgetOpts(10 * 1024 * 1024));
        for (int i = 1; i <= 100; ++i) {
            std::string k = "k_" + std::to_string(i), v = "payload_" + std::to_string(i);
            CHECK(writer.append(EntryType::Full, k, v.data(), v.size()).seq ==
                  static_cast<uint64_t>(i));
        }
        CHECK(writer.getCurrentSeq() == 100 && writer.getCurrentSegmentId() == 1);
    }
    {
        TrackedWriter writer(test_dir, budgetOpts(10 * 1024 * 1024));
        CHECK(writer.getCurrentSegmentId() == 2 && writer.getCurrentSeq() == 100);
        auto res = writer.append(EntryType::Full, "k_101", "payload_101", 11);
        CHECK(res.seq == 101 && res.vptr.segment_id == 2);
    }
    VLogReader reader(test_dir);
    auto sr1 = reader.scanSegment(1, /*is_last_segment=*/false);
    auto sr2 = reader.scanSegment(2, /*is_last_segment=*/true);
    CHECK(sr1.entries.size() == 100 && sr2.entries.size() == 1);
    CHECK(sr2.entries[0].header.seq == 101 && sr2.entries[0].key == "k_101");
    pass("V5 reopen continues at seq 101 in segment 2; all 101 entries readable");
}

static void testV6TornWriteMatrix(const std::string& test_dir) {
    banner("V6 Torn-write matrix: segment header, entry header, entry payload");
    char partial[128];
    std::memset(partial, 0xAA, sizeof(partial));
    // Short segment header: truncated to 0 if last, fatal otherwise.
    {
        std::string sub = test_dir + "/torn_seg_hdr";
        std::filesystem::create_directories(sub);
        std::string seg = sub + "/vlog_00000001.log";
        int fd = open(seg.c_str(), O_RDWR | O_CREAT, 0644);
        CHECK(fd >= 0);
        checkedWrite(fd, partial, sizeof(partial));
        checkedFsync(fd);
        checkedClose(fd);
        VLogReader reader(sub);
        auto sr = reader.scanSegment(1, /*is_last=*/true);
        struct stat st;
        CHECK(sr.entries.empty() && sr.truncated && stat(seg.c_str(), &st) == 0 && st.st_size == 0);
        fd = open(seg.c_str(), O_RDWR | O_CREAT, 0644);
        CHECK(fd >= 0);
        checkedWrite(fd, partial, sizeof(partial));
        checkedClose(fd);
        bool caught = false;
        try {
            reader.scanSegment(1, /*is_last=*/false);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught);
    }
    // Full-size segment header with bad magic/CRC: same rules.
    {
        std::string sub = test_dir + "/corrupt_seg_hdr";
        std::filesystem::create_directories(sub);
        std::string seg = sub + "/vlog_00000001.log";
        SegmentHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.magic = 0xDEADBEEF;
        hdr.crc32c = 0x1234;
        char block[kBlockSize];
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &hdr, sizeof(hdr));
        int fd = open(seg.c_str(), O_RDWR | O_CREAT, 0644);
        CHECK(fd >= 0);
        checkedWrite(fd, block, kBlockSize);
        checkedFsync(fd);
        checkedClose(fd);
        VLogReader reader(sub);
        auto sr = reader.scanSegment(1, /*is_last=*/true);
        struct stat st;
        CHECK(sr.entries.empty() && sr.truncated && stat(seg.c_str(), &st) == 0 && st.st_size == 0);
        fd = open(seg.c_str(), O_RDWR | O_CREAT, 0644);
        CHECK(fd >= 0);
        checkedWrite(fd, block, kBlockSize);
        checkedClose(fd);
        bool caught = false;
        try {
            reader.scanSegment(1, /*is_last=*/false);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught);
    }
    // Half an entry header at EOF.
    {
        std::string sub = test_dir + "/torn_entry_hdr";
        std::filesystem::create_directories(sub);
        {
            TrackedWriter writer(sub, budgetOpts());
            writer.append(EntryType::Full, "valid1", "p1", 2);
            writer.append(EntryType::Full, "valid2", "p2", 2);
        }
        int fd = open((sub + "/vlog_00000001.log").c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        char half_hdr[16];
        std::memset(half_hdr, 0x55, sizeof(half_hdr));
        checkedWrite(fd, half_hdr, sizeof(half_hdr));
        checkedFsync(fd);
        checkedClose(fd);
        VLogReader reader(sub);
        auto sr = reader.scanSegment(1, /*is_last=*/true);
        CHECK(sr.entries.size() == 2 && sr.truncated);
    }
    // Entry header plus 10 of its 96 payload bytes at EOF.
    {
        std::string sub = test_dir + "/torn_payload";
        std::filesystem::create_directories(sub);
        {
            TrackedWriter writer(sub, budgetOpts());
            writer.append(EntryType::Full, "validA", "pA", 2);
        }
        int fd = open((sub + "/vlog_00000001.log").c_str(), O_WRONLY | O_APPEND);
        CHECK(fd >= 0);
        EntryHeader hdr;
        std::memset(&hdr, 0, sizeof(hdr));
        hdr.total_len = 128;
        hdr.type = static_cast<uint8_t>(EntryType::Full);
        hdr.payload_len = 80;
        checkedWrite(fd, &hdr, sizeof(hdr));
        char half_p[10];
        std::memset(half_p, 0x11, sizeof(half_p));
        checkedWrite(fd, half_p, sizeof(half_p));
        checkedFsync(fd);
        checkedClose(fd);
        VLogReader reader(sub);
        auto sr = reader.scanSegment(1, /*is_last=*/true);
        CHECK(sr.entries.size() == 1 && sr.truncated && sr.entries[0].key == "validA");
    }
    pass("V6 torn segment header, entry header and payload: truncated when last, fatal when not "
         "last");
}

static void testV7RedoAfterCheckpoint(const std::string& test_dir) {
    banner("V7 Redo on top of a checkpointed state, with tombstones");
    std::filesystem::create_directories(test_dir);
    {
        TrackedWriter writer(test_dir, budgetOpts());
        writer.append(EntryType::Full, "key_A", "val_A_v1", 8);   // seq 1
        writer.append(EntryType::Full, "key_B", "val_B_v1", 8);   // seq 2
        writer.append(EntryType::Full, "key_C", "val_C_v1", 8);   // seq 3
        writer.append(EntryType::Full, "key_D", "val_D_v1", 8);   // seq 4
        writer.append(EntryType::Full, "key_A", "val_A_v2", 8);   // seq 5
        writer.append(EntryType::Tombstone, "key_C", nullptr, 0); // seq 6
        writer.append(EntryType::Full, "key_B", "val_B_v2", 8);   // seq 7
        writer.append(EntryType::Full, "key_E", "val_E_v1", 8);   // seq 8
        writer.append(EntryType::Full, "key_C", "val_C_v2", 8);   // seq 9: key_C re-created
        writer.append(EntryType::Tombstone, "key_D", nullptr, 0); // seq 10
    }
    struct Rec {
        uint64_t seq{0};
        std::string val;
        bool deleted{false};
    };
    // State as a checkpoint might have captured it: key_A at seq 5, the others older.
    std::map<std::string, Rec> db;
    db["key_A"] = {5, "val_A_v2", false};
    db["key_B"] = {2, "val_B_v1", false};
    db["key_C"] = {3, "val_C_v1", false};
    db["key_D"] = {4, "val_D_v1", false};
    VLogReader reader(test_dir);
    auto sr = reader.scanSegment(1, /*is_last=*/true);
    CHECK(sr.entries.size() == 10);
    auto applyEntry = [&](const RecoveredEntry& e) {
        bool has = db.find(e.key) != db.end();
        uint64_t entry_seq = e.header.seq;
        if (!should_apply(has, has ? db[e.key].seq : 0, entry_seq, e.header.flags)) return false;
        db[e.key].seq = entry_seq;
        db[e.key].deleted = e.header.type == static_cast<uint8_t>(EntryType::Tombstone);
        db[e.key].val = db[e.key].deleted ? "" : std::string(e.payload.begin(), e.payload.end());
        return true;
    };
    int applied = 0;
    for (const auto& e : sr.entries) applied += applyEntry(e) ? 1 : 0;
    CHECK(applied == 5); // seqs 6..10
    CHECK(db["key_A"].seq == 5 && db["key_A"].val == "val_A_v2");
    CHECK(db["key_B"].seq == 7 && db["key_B"].val == "val_B_v2");
    CHECK(db["key_C"].seq == 9 && db["key_C"].val == "val_C_v2" && !db["key_C"].deleted);
    CHECK(db["key_D"].seq == 10 && db["key_D"].deleted);
    CHECK(db["key_E"].seq == 8 && db["key_E"].val == "val_E_v1");
    for (const auto& e : sr.entries) CHECK(!applyEntry(e));
    pass("V7 redo over a checkpointed state applies seqs 6-10 only; a second pass applies 0");
}

// A crash between creating vlog_N.log (O_CREAT|O_EXCL) and its header fsync leaves a short or
// corrupt last segment. Recovery truncates it to 0 bytes and must also unlink it; otherwise the
// next restart sees a 0-byte non-last segment and refuses to open.
static void testV8TornSegmentCreation(const std::string& test_dir) {
    banner("V8 Torn segment creation is unlinked on recovery");
    struct Case {
        const char* name;
        size_t bytes;
        bool valid_size_bad_crc;
    };
    const Case cases[] = {
        {"short file (100 bytes < 4096)", 100, false},
        {"empty file (0 bytes)", 0, false},
        {"full 4096-byte block, bad header CRC", kBlockSize, true},
    };
    int case_no = 0;
    for (const Case& c : cases) {
        std::string dir = test_dir + "/case_" + std::to_string(case_no++);
        std::filesystem::create_directories(dir);
        {
            TrackedWriter w(dir, budgetOpts());
            for (int i = 1; i <= 5; ++i) {
                CHECK(w.append(EntryType::Full, "k" + std::to_string(i), "v", 1).seq ==
                      static_cast<uint64_t>(i));
            }
        }
        std::string ghost = dir + "/vlog_00000002.log";
        {
            int fd = open(ghost.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
            CHECK(fd >= 0);
            if (c.bytes > 0) {
                std::vector<uint8_t> junk(c.bytes, 0);
                if (c.valid_size_bad_crc) {
                    SegmentHeader h{};
                    h.magic = kSegmentMagic;
                    h.format_version = kFormatVersion;
                    h.segment_id = 2;
                    h.crc32c = 0xDEADBEEF;
                    std::memcpy(junk.data(), &h, sizeof(h));
                }
                checkedWrite(fd, junk.data(), junk.size());
            }
            checkedFsync(fd);
            checkedClose(fd);
        }
        {
            TrackedWriter w(dir, budgetOpts());
            CHECK(w.getCurrentSeq() == 5 && !std::filesystem::exists(ghost));
            CHECK(w.append(EntryType::Full, "k6", "v", 1).seq == 6);
        }
        {
            TrackedWriter w(dir, budgetOpts());
            CHECK(w.getCurrentSeq() == 6);
            CHECK(w.append(EntryType::Full, "k7", "v", 1).seq == 7);
        }
        std::vector<uint32_t> ids = segmentIds(dir);
        VLogReader reader(dir);
        std::vector<uint64_t> seqs;
        for (size_t i = 0; i < ids.size(); ++i) {
            ScanResult sr = reader.scanSegment(ids[i], i + 1 == ids.size());
            CHECK(!sr.truncated);
            for (const auto& e : sr.entries) {
                const uint64_t seq =
                    e.header.seq; // EntryHeader is packed: copy, never bind a reference
                seqs.push_back(seq);
            }
        }
        CHECK((seqs == std::vector<uint64_t>{1, 2, 3, 4, 5, 6, 7}));
        detail() << c.name << ": ghost segment removed; two restarts OK; seqs 1-7 recovered\n";
    }
    pass("V8 torn segment creation (3 shapes) is unlinked; two later restarts open normally");
}

static void testV9ProcessCrash(const std::string& base_dir) {
    banner("V9 kill -9 during multi-threaded group commit");
#if defined(TSAN_BUILD)
    skip("V9 kill -9 crash recovery", "fork + kill -9 not run under ThreadSanitizer");
    (void)base_dir;
    return;
#else
    const int kIterations = kQuick ? 10 : 50;
    const size_t kMinAcksBeforeKill = 200;
    const int kChildOpsPerThread = kQuick ? 100 : 2000;
    std::mt19937 rng(1337);
    // Full mode: kill 5-400 ms after the 200th ack, so writes are in flight. Quick mode: kill as
    // soon as a random ack count in [200, 350) is reached (the child is still writing then), so
    // the bytes written depend on the op count, not on disk speed. kill -9 cannot tear a single
    // write() call, so torn tails rarely appear here; V3/V6 cover them.
    std::uniform_int_distribution<int> kill_delay_ms_dist(5, 400);
    std::uniform_int_distribution<size_t> kill_at_ack_dist(kMinAcksBeforeKill, 349);
    size_t truncated_iters = 0;
    uint64_t total_acked = 0, total_recovered = 0, min_unacked = UINT64_MAX, max_unacked = 0;

    for (int iter = 0; iter < kIterations; ++iter) {
        std::string crash_dir = base_dir + "/crash_iter_" + std::to_string(iter);
        std::filesystem::create_directories(crash_dir);
        VLogWriter::Options child_opts = budgetOpts(1024 * 1024, 100 * 1024 * 1024);
        int pipefd[2];
        CHECK(pipe(pipefd) == 0);
        std::cout.flush();
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            // The parent accounts the child's bytes from file sizes.
            CHECK(close(pipefd[0]) == 0);
            try {
                VLogWriter writer(crash_dir, child_opts);
                std::vector<std::thread> threads;
                for (int w = 0; w < 4; ++w) {
                    threads.emplace_back([&, w]() {
                        for (int i = 0; i < kChildOpsPerThread; ++i) {
                            std::string key = "c_t" + std::to_string(w) + "_k" + std::to_string(i);
                            auto res =
                                writer.append(EntryType::Full, key, nullptr, 128, 0, 0, nullptr,
                                              [](uint64_t seq, uint8_t* buf, size_t len) {
                                                  fillDeterministicPayload(seq, buf, len);
                                              });
                            // append() returns after F_FULLFSYNC. 8-byte pipe writes are atomic.
                            uint64_t ack_seq = res.seq;
                            CHECK(write(pipefd[1], &ack_seq, sizeof(ack_seq)) ==
                                  static_cast<ssize_t>(sizeof(ack_seq)));
                        }
                    });
                }
                for (auto& t : threads) t.join();
                if (kQuick) {
                    while (true) pause(); // cap reached before the kill arrived
                }
            } catch (const std::exception& e) {
                std::cerr << "[Child] append failed: " << e.what() << "\n";
                _exit(2);
            }
            CHECK(close(pipefd[1]) == 0);
            _exit(0);
        }
        CHECK(close(pipefd[1]) == 0);
        std::vector<uint64_t> acked;
        const size_t kill_at = kQuick ? kill_at_ack_dist(rng) : kMinAcksBeforeKill;
        while (acked.size() < kill_at) {
            uint64_t seq = 0;
            ssize_t r = read(pipefd[0], &seq, sizeof(seq));
            if (r == static_cast<ssize_t>(sizeof(seq)))
                acked.push_back(seq);
            else if (r == 0)
                break; // child exited early; checked via waitpid below
            else
                CHECK(r < 0 && errno == EINTR);
        }
        const int kill_delay_ms = kQuick ? 0 : kill_delay_ms_dist(rng);
        if (kill_delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(kill_delay_ms));
        CHECK(kill(pid, SIGKILL) == 0); // also succeeds on an unreaped zombie
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid);
        bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        CHECK(killed || (WIFEXITED(status) && WEXITSTATUS(status) == 0));
        // Drain the acks written before the child died.
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        CHECK(flags != -1 && fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != -1);
        while (true) {
            uint64_t seq = 0;
            ssize_t r = read(pipefd[0], &seq, sizeof(seq));
            if (r == static_cast<ssize_t>(sizeof(seq))) {
                acked.push_back(seq);
                continue;
            }
            if (r == 0) break;
            CHECK(r < 0 && (errno == EAGAIN || errno == EINTR));
            if (errno == EAGAIN) break;
        }
        CHECK(close(pipefd[0]) == 0);
        CHECK(acked.size() >= kMinAcksBeforeKill);

        std::vector<uint32_t> seg_ids = segmentIds(crash_dir);
        uint64_t child_bytes = 0;
        for (uint32_t id : seg_ids) {
            char name[32];
            std::snprintf(name, sizeof(name), "/vlog_%08u.log", id);
            child_bytes += std::filesystem::file_size(crash_dir + name);
        }
        accountBytes(child_bytes, "V9 child process");

        VLogReader reader(crash_dir);
        std::vector<RecoveredEntry> recovered;
        bool truncated = false;
        for (size_t i = 0; i < seg_ids.size(); ++i) {
            bool is_last = i + 1 == seg_ids.size();
            ScanResult sr = reader.scanSegment(seg_ids[i], is_last);
            if (is_last && sr.truncated) truncated = true;
            recovered.insert(recovered.end(), sr.entries.begin(), sr.entries.end());
        }
        truncated_iters += truncated ? 1 : 0;
        std::set<uint64_t> recovered_seqs;
        uint64_t prev = 0;
        for (const auto& rec : recovered) {
            uint64_t seq = rec.header.seq;
            CHECK(seq > prev);
            prev = seq;
            recovered_seqs.insert(seq);
            std::vector<uint8_t> expected(rec.header.payload_len);
            fillDeterministicPayload(seq, expected.data(), expected.size());
            CHECK(rec.payload == expected);
        }
        for (uint64_t a : acked) CHECK(recovered_seqs.count(a) == 1); // no acked write lost
        uint64_t unacked = recovered.size() - acked.size();
        total_acked += acked.size();
        total_recovered += recovered.size();
        min_unacked = std::min(min_unacked, unacked);
        max_unacked = std::max(max_unacked, unacked);
        detailf("iter %2d/%d: kill at +%3d ms, %s, acked %5zu, recovered %5zu, unacked %3llu, "
                "segments %zu%s\n",
                iter + 1, kIterations, kill_delay_ms, killed ? "SIGKILL" : "child finished first",
                acked.size(), recovered.size(), (unsigned long long)unacked, seg_ids.size(),
                truncated ? ", torn tail truncated" : "");
    }
    pass("V9 kill -9 x" + std::to_string(kIterations) + " during group commit",
         "all " + std::to_string(total_acked) + " acked seqs among " +
             std::to_string(total_recovered) +
             " recovered, bytes verified; recovered-but-unacked " + std::to_string(min_unacked) +
             "-" + std::to_string(max_unacked) + " per run; torn tail in " +
             std::to_string(truncated_iters) + " runs");
#endif
}

static void testV10GroupCommitBenchmark(const std::string& test_dir) {
    banner("V10 Group commit benchmark (1, 4, 16, 64 threads)");
    detailf("%8s %8s %10s %12s %10s %10s %10s\n", "threads", "runtime", "writes", "F_FULLFSYNC",
            "writes/s", "fsyncs/s", "batch");
    std::string summary;
    for (int num_threads : {1, 4, 16, 64}) {
        std::string bench_dir = test_dir + "/bench_threads_" + std::to_string(num_threads);
        std::filesystem::create_directories(bench_dir);
        TrackedWriter writer(bench_dir, budgetOpts(64 * 1024 * 1024, 256 * 1024 * 1024));
        std::atomic<bool> stop{false}, append_failed{false};
        // Quick mode runs a fixed number of appends per thread instead of a fixed time, so the
        // bytes written do not depend on disk speed.
        const size_t ops_per_thread = kQuick ? 64 : SIZE_MAX;
        auto worker = [&](int tid) {
            size_t idx = 0;
            while (!stop.load(std::memory_order_relaxed) && idx < ops_per_thread) {
                std::string key = "b_t" + std::to_string(tid) + "_k" + std::to_string(idx++);
                std::string payload =
                    "{\"t\": " + std::to_string(tid) + ", \"idx\": " + std::to_string(idx) + "}";
                try {
                    writer.append(EntryType::Full, key, payload.data(), payload.size());
                } catch (const std::exception& e) {
                    // Includes hitting the byte cap: the benchmark must not pass silently on that.
                    std::cerr << "[V10] append failed: " << e.what() << "\n";
                    append_failed.store(true);
                    break;
                }
            }
        };
        auto start = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < num_threads; ++t) threads.emplace_back(worker, t);
        if (!kQuick) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            stop.store(true);
        }
        for (auto& th : threads) th.join();
        double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        CHECK(!append_failed.load());
        uint64_t ops = writer.getTotalOps(), fsyncs = writer.getTotalFsyncs();
        detailf("%8d %7.2fs %10llu %12llu %10.1f %10.1f %10.2f\n", num_threads, secs,
                (unsigned long long)ops, (unsigned long long)fsyncs, ops / secs, fsyncs / secs,
                writer.getAverageBatchSize());
        summary += (summary.empty() ? "" : ", ") + std::to_string(num_threads) +
                   "t: " + std::to_string(static_cast<long>(ops / secs)) + " w/s";
    }
    pass("V10 group commit benchmark", summary);
}

int main() {
    std::string test_dir = setupRun("vlog_test");
    Cleaner cleaner{test_dir};
    testV1Crc();
    testV2HeaderBounds(test_dir + "/bounds_test");
    testV3TornTailAndCorruption(test_dir + "/torn_test");
    testV4RedoRule(test_dir + "/redo_test");
    testV5ReopenNoTruncation(test_dir + "/reopen_test");
    testV6TornWriteMatrix(test_dir + "/fault_test");
    testV7RedoAfterCheckpoint(test_dir + "/mid_checkpoint_test");
    testV8TornSegmentCreation(test_dir + "/torn_creation_test");
    testV9ProcessCrash(test_dir + "/crash_50");
    testV10GroupCommitBenchmark(test_dir + "/bench");
    printBudget();
    return 0;
}
