// SuperblockManager tests: slot CRCs and bounds, ping-pong writes, corruption of one or both
// slots, torn slot writes, and full rebuild when no valid superblock exists.
#include "test_common.hpp"

#include "index/IndexRecovery.hpp"
#include "index/IndexedStore.hpp"
#include "index/SuperblockManager.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

using namespace testutil;
using bpt::SlotState;
using bpt::SuperblockManager;
using bpt::SuperblockRecord;
using LoadStatus = SuperblockManager::LoadResult::Status;

static SuperblockManager::Options sbOpts() {
    SuperblockManager::Options o;
    o.on_write = [](uint64_t n) { accountBytes(n, "superblock.db write"); };
    return o;
}

static SuperblockRecord makeRec(uint64_t epoch) {
    SuperblockRecord r;
    r.sb.epoch = epoch;
    r.sb.checkpoint_seq = 1000 * epoch;
    r.sb.replay_segment_id = static_cast<uint32_t>(epoch);
    r.sb.replay_offset = 4096 + 8 * static_cast<uint32_t>(epoch);
    r.sb.index_root_page = 1 + epoch;
    r.sb.index_page_count = 100 + epoch;
    r.sb.oldest_live_segment = 1;
    r.ext.index_nonce = 0x1234567890ABCDEFULL;
    r.ext.vlog_first_segment = 1;
    r.ext.vlog_created_ns = 1700000000000000000ULL;
    return r;
}

static void xorByte(const std::string& path, uint64_t off, uint8_t mask) {
    int fd = open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    uint8_t b = 0;
    CHECK(pread(fd, &b, 1, static_cast<off_t>(off)) == 1);
    b ^= mask;
    CHECK(pwrite(fd, &b, 1, static_cast<off_t>(off)) == 1);
    accountBytes(1, "test pwrite (bit flip)");
    CHECK(vlog::full_fsync(fd) == 0);
    CHECK(close(fd) == 0);
}

static void pwriteBytes(const std::string& path, uint64_t off, const uint8_t* p, size_t n) {
    int fd = open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, p, n, static_cast<off_t>(off)) == static_cast<ssize_t>(n));
    accountBytes(n, "test pwrite (torn write)");
    CHECK(vlog::full_fsync(fd) == 0);
    CHECK(close(fd) == 0);
}

static void expectLoad(SuperblockManager& m, LoadStatus st, int slot, uint64_t epoch,
                       bool has_older, uint64_t older_epoch, const char* what) {
    SuperblockManager::LoadResult r = m.load();
    detail() << "  " << what << ": slot0 " << bpt::toString(r.slot[0]) << ", slot1 "
             << bpt::toString(r.slot[1]);
    if (r.status == LoadStatus::Ok) {
        detail() << " -> current slot " << r.current_slot << " epoch " << r.current.sb.epoch;
        if (r.has_older) detail() << ", older epoch " << r.older.sb.epoch;
    } else {
        detail() << " -> " << (r.status == LoadStatus::Missing ? "Missing" : "Corrupt");
    }
    detail() << "\n";
    CHECK(r.status == st);
    if (st != LoadStatus::Ok) return;
    CHECK(r.current_slot == slot && r.current.sb.epoch == epoch);
    CHECK(r.has_older == has_older);
    if (has_older) CHECK(r.older.sb.epoch == older_epoch);
    SuperblockRecord want = makeRec(epoch);
    CHECK(r.current.sb.checkpoint_seq == want.sb.checkpoint_seq &&
          r.current.sb.index_root_page == want.sb.index_root_page &&
          r.current.sb.index_page_count == want.sb.index_page_count &&
          r.current.ext.index_nonce == want.ext.index_nonce);
}

static void testSB1BitFlipsInMemory() {
    banner("SB1 Every single-bit flip in the 96 checked bytes of a slot is rejected");
    std::vector<uint8_t> slot(bpt::kSuperSlotSize);
    bpt::encodeSlot(makeRec(7), slot.data());
    SuperblockRecord got;
    CHECK(bpt::decodeSlot(slot.data(), slot.size(), &got) == SlotState::Valid && got.sb.epoch == 7);
    uint32_t detected = 0, total = 96 * 8;
    for (uint32_t bit = 0; bit < total; ++bit) {
        slot[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
        if (bpt::decodeSlot(slot.data(), slot.size(), nullptr) != SlotState::Valid) detected++;
        slot[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    }
    detail() << "single-bit flips rejected: " << detected << " of " << total << "\n";
    CHECK(detected == total);
    // Bounds: page ids are uint32_t, the superblock fields are uint64_t.
    int rejected = 0;
    for (int c = 0; c < 5; ++c) {
        SuperblockRecord r = makeRec(3);
        if (c == 0) r.sb.index_page_count = uint64_t(UINT32_MAX) + 1;
        if (c == 1) r.sb.index_root_page = r.sb.index_page_count;
        if (c == 2) r.sb.index_root_page = 0;
        if (c == 3) r.sb.epoch = 0;
        if (c == 4) r.sb.replay_offset = 100;
        bpt::encodeSlot(r, slot.data()); // valid CRC, bad field
        if (bpt::decodeSlot(slot.data(), slot.size(), nullptr) == SlotState::BadFields) rejected++;
    }
    CHECK(rejected == 5);
    CHECK(bpt::decodeSlot(slot.data(), 95, nullptr) == SlotState::Short);
    pass("SB1 slot CRCs reject every single-bit flip in the 96 checked bytes",
         std::to_string(total) + " flips; 5 out-of-range fields rejected");
}

static void testSB2PingPong(const std::string& dir) {
    banner("SB2 Ping-pong writes: epochs alternate slots, read picks the highest valid epoch");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string path = dir + "/superblock.db";
    SuperblockManager m(path, sbOpts());
    expectLoad(m, LoadStatus::Missing, -1, 0, false, 0, "no file");
    for (uint64_t e = 1; e <= 4; ++e) {
        m.commit(makeRec(e));
        CHECK(m.currentSlot() == static_cast<int>((e - 1) % 2));
        SuperblockManager fresh(path, sbOpts());
        expectLoad(fresh, LoadStatus::Ok, static_cast<int>((e - 1) % 2), e, e > 1, e - 1,
                   ("after commit of epoch " + std::to_string(e)).c_str());
    }
    struct stat st;
    CHECK(stat(path.c_str(), &st) == 0 && st.st_size == 2 * bpt::kSuperSlotSize);
    pass("SB2 ping-pong: epochs alternate slots, read picks the highest valid epoch");
}

static void testSB3Corruption(const std::string& dir) {
    banner("SB3 Bit flips in slot 0, slot 1, both; torn write of one slot");
    CHECK(mkdir(dir.c_str(), 0755) == 0);
    const std::string path = dir + "/superblock.db";
    auto fresh4 = [&]() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        SuperblockManager m(path, sbOpts());
        m.load();
        for (uint64_t e = 1; e <= 4; ++e) m.commit(makeRec(e)); // slot 0: epoch 3, slot 1: epoch 4
    };

    detail() << "(a) bit flip in slot 1 (newest, epoch 4):\n";
    fresh4();
    xorByte(path, 4096 + 20, 0x10); // inside checkpoint_seq
    {
        SuperblockManager m(path, sbOpts());
        expectLoad(m, LoadStatus::Ok, 0, 3, false, 0, "after flip");
        m.commit(makeRec(4)); // must go to slot 1 (the damaged one) and keep slot 0
        CHECK(m.currentSlot() == 1);
        SuperblockManager m2(path, sbOpts());
        expectLoad(m2, LoadStatus::Ok, 1, 4, true, 3, "after the next commit");
    }

    detail() << "(b) bit flip in slot 0 (older, epoch 3):\n";
    fresh4();
    xorByte(path, 70, 0x01); // inside the extension block
    {
        SuperblockManager m(path, sbOpts());
        expectLoad(m, LoadStatus::Ok, 1, 4, false, 0, "after flip");
        m.commit(makeRec(5));
        CHECK(m.currentSlot() == 0);
        SuperblockManager m2(path, sbOpts());
        expectLoad(m2, LoadStatus::Ok, 0, 5, true, 4, "after the next commit");
    }

    detail() << "(c) bit flips in both slots:\n";
    fresh4();
    xorByte(path, 8, 0x80);
    xorByte(path, 4096 + 59, 0x02);
    {
        SuperblockManager m(path, sbOpts());
        expectLoad(m, LoadStatus::Corrupt, -1, 0, false, 0, "after flips");
    }

    detail() << "(d) torn write: epoch 5 was being written to slot 0; only its first 30 bytes "
                "reached disk:\n";
    fresh4();
    {
        std::vector<uint8_t> img(bpt::kSuperSlotSize);
        bpt::encodeSlot(makeRec(5), img.data());
        pwriteBytes(path, 0, img.data(), 30);
        SuperblockManager m(path, sbOpts());
        expectLoad(m, LoadStatus::Ok, 1, 4, false, 0, "after torn write");
        // Torn the other way: first 2048 bytes of slot 0 are zero (sector of zeros).
        std::vector<uint8_t> zeros(2048, 0);
        pwriteBytes(path, 0, zeros.data(), zeros.size());
        expectLoad(m, LoadStatus::Ok, 1, 4, false, 0, "after zeroed first half of slot 0");
    }

    detail() << "(e) file truncated to one slot:\n";
    fresh4();
    CHECK(truncate(path.c_str(), bpt::kSuperSlotSize) == 0);
    {
        SuperblockManager m(path, sbOpts());
        expectLoad(m, LoadStatus::Ok, 0, 3, false, 0, "after truncate to 4096 bytes");
    }
    pass("SB3 bit flip in slot 0, slot 1, both; torn write; short file: the valid slot is used, "
         "both bad -> Corrupt");
}

// ------------------------------------------------------------------------------------------
// Store level: invalid superblock -> full rebuild from the vLog.
// ------------------------------------------------------------------------------------------
using bpt::IndexedStore;

static IndexedStore::Options storeOpts() {
    IndexedStore::Options o;
    o.cache_frames = 64;
    o.vlog_opts = vlog::VLogWriter::Options(
        64 * 1024 * 1024, std::min<uint64_t>(64ULL * 1024 * 1024, remainingBudget()));
    o.on_index_write = [](uint64_t n) { accountBytes(n, "index.db / superblock.db write"); };
    return o;
}

static void finishStore(std::unique_ptr<IndexedStore>& st, bool clean) {
    uint64_t vb = st->vlogBytesWritten();
    if (clean) st->close();
    st.reset();
    accountBytes(vb, "IndexedStore vLog writes");
}

static void testSB4StoreRebuild(const std::string& dir) {
    banner("SB4 Store: both slots invalid, or superblock.db missing -> full rebuild from the vLog");
    std::map<std::string, std::string> want;
    {
        auto st = IndexedStore::open(dir, storeOpts());
        for (int i = 0; i < 300; ++i) {
            std::string k = "key" + std::to_string(i % 120), v = "value-" + std::to_string(i);
            st->put(k, v);
            want[k] = v;
        }
        for (int i = 0; i < 120; i += 7) {
            st->erase("key" + std::to_string(i));
            want.erase("key" + std::to_string(i));
        }
        finishStore(st, true);
    }
    auto verify = [&](bpt::OpenStatus expect, bool expect_rebuild, const char* what) {
        auto st = IndexedStore::open(dir, storeOpts());
        const bpt::OpenIndexResult& info = st->openInfo();
        detail() << "  " << what << ": first check " << bpt::toString(info.initial_status)
                 << (info.why.empty() ? "" : " (" + info.why + ")") << ", rebuilt "
                 << (info.rebuilt ? "yes" : "no") << ", vLog entries scanned "
                 << info.vlog_entries_scanned << "\n";
        CHECK(info.initial_status == expect && info.rebuilt == expect_rebuild);
        CHECK(st->tree().size() == want.size());
        for (const auto& [k, v] : want) CHECK(st->get(k) == std::optional<std::string>(v));
        for (int i = 0; i < 120; i += 7) CHECK(!st->get("key" + std::to_string(i)).has_value());
        CHECK(st->tree().checkInvariants().empty());
        finishStore(st, true);
    };
    verify(bpt::OpenStatus::Ok, false, "clean reopen");
    const std::string sb = dir + "/superblock.db";
    // Flip a bit in every valid slot.
    {
        SuperblockManager m(sb, sbOpts());
        SuperblockManager::LoadResult r = m.load();
        CHECK(r.status == LoadStatus::Ok);
        for (int s = 0; s < 2; ++s) {
            if (r.slot[s] == SlotState::Valid)
                xorByte(sb, static_cast<uint64_t>(s) * 4096 + 33, 0x04);
        }
        CHECK(m.load().status == LoadStatus::Corrupt);
    }
    verify(bpt::OpenStatus::SuperblockCorrupt, true, "both slots corrupt");
    CHECK(unlink(sb.c_str()) == 0);
    verify(bpt::OpenStatus::NoSuperblock, true, "superblock.db deleted (index.db present)");
    pass("SB4 store: both slots invalid or superblock.db missing -> full rebuild",
         std::to_string(want.size()) + " keys correct");
}

int main() {
    std::string dir = setupRun("superblock_test");
    Cleaner cleaner{dir};
    testSB1BitFlipsInMemory();
    testSB2PingPong(dir + "/sb2");
    testSB3Corruption(dir + "/sb3");
    testSB4StoreRebuild(dir + "/sb4");
    printBudget();
    return 0;
}
