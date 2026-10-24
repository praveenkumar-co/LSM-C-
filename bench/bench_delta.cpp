// Delta update benchmark (make bench). For 1, 2 and 8 KB documents: bytes written per update of
// one small field with put() of the whole document vs update() with a Delta entry (chain caps 2
// and 4; 4 is kMaxDeltaChain, the maximum), and get() latency at each chain depth.
// Single thread, default checkpoint triggers (4 MiB / 30 s), final checkpoint on close.
#include "../tests/test_common.hpp"

#include "doc/Json.hpp"
#include "doc/Update.hpp"
#include "index/IndexedStore.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace testutil;
using bpt::IndexedStore;
using doc::Value;

namespace {

constexpr int kUpdates = 10000;
constexpr int kGets = 5000;
constexpr uint64_t kBenchBudget = 256ULL * 1024 * 1024;
const std::vector<size_t> kSizes = {1024, 2048, 8192};
const std::vector<uint16_t> kCaps = {2, 4};

std::string shell(const char* cmd) {
    std::string out;
    if (FILE* f = popen(cmd, "r")) {
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) out += buf;
        pclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

void printHardware() {
    struct utsname u;
    uname(&u);
#if defined(__APPLE__)
    const std::string cpu = shell("sysctl -n machdep.cpu.brand_string");
    const std::string mem = shell("sysctl -n hw.memsize");
    const std::string os = "macOS " + shell("sw_vers -productVersion");
#else
    const std::string cpu = shell("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2");
    const std::string mem = shell("awk '/MemTotal/ {print $2 * 1024}' /proc/meminfo");
    const std::string os = std::string(u.sysname) + " " + u.release;
#endif
    std::printf("Hardware: %s, %.0f GB RAM, %s (%s), compiler %s\n", cpu.c_str(),
                mem.empty() ? 0.0 : std::stod(mem) / 1e9, os.c_str(), u.machine, __VERSION__);
}

// {"id":1,"profile":{"score":0,"level":3,"name":"..."},"f00":"...",...} padded to `size` bytes.
std::string makeDocument(size_t size) {
    Value v = Value::object();
    v.set("id", Value::integer(1));
    Value profile = Value::object();
    profile.set("score", Value::integer(0));
    profile.set("level", Value::integer(3));
    profile.set("name", Value::string("benchmark user"));
    v.set("profile", std::move(profile));
    for (int i = 0; doc::serialize(v).size() + 80 < size; ++i) {
        char key[8];
        std::snprintf(key, sizeof(key), "f%02d", i);
        v.set(key, Value::string(std::string(64, static_cast<char>('a' + i % 26))));
    }
    std::string s = doc::serialize(v);
    const size_t missing = size - s.size() - std::strlen(",\"pad\":\"\"");
    v.set("pad", Value::string(std::string(missing, 'p')));
    s = doc::serialize(v);
    CHECK(s.size() == size);
    return s;
}

struct Run {
    uint64_t vlog_bytes = 0;
    uint64_t index_bytes = 0;
    uint64_t deltas = 0, fulls = 0;
    double seconds = 0;
};

uint64_t g_index_bytes = 0;

IndexedStore::Options benchOpts(uint16_t cap) {
    IndexedStore::Options o;
    o.delta_chain_cap = cap;
    o.vlog_opts = vlog::VLogWriter::Options(64 * 1024 * 1024, 256ULL * 1024 * 1024);
    o.on_index_write = [](uint64_t n) {
        g_index_bytes += n;
        accountBytes(n, "bench index writes");
    };
    return o; // default checkpoint triggers: 4 MiB of vLog or 30 s, and close
}

void finish(std::unique_ptr<IndexedStore>& st, const std::string& dir, Run* r) {
    r->vlog_bytes = st->vlogBytesWritten();
    st->close();
    st.reset();
    accountBytes(r->vlog_bytes, "bench vLog writes");
    CHECK(bytesWritten() <= kBenchBudget);
    std::filesystem::remove_all(dir);
}

Run runPuts(const std::string& dir, size_t size) {
    Run r;
    g_index_bytes = 0;
    auto st = IndexedStore::open(dir, benchOpts(vlog::kMaxDeltaChain));
    Value v = doc::parse(makeDocument(size));
    st->put("user:1", doc::serialize(v));
    const uint64_t vbase = st->vlogBytesWritten(), ibase = g_index_bytes;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= kUpdates; ++i) {
        v.find("profile")->set("score", Value::integer(i));
        st->put("user:1", doc::serialize(v));
        r.fulls++;
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    finish(st, dir, &r);
    r.vlog_bytes -= vbase;
    r.index_bytes = g_index_bytes - ibase;
    return r;
}

Run runUpdates(const std::string& dir, size_t size, uint16_t cap) {
    Run r;
    g_index_bytes = 0;
    auto st = IndexedStore::open(dir, benchOpts(cap));
    st->put("user:1", makeDocument(size));
    const uint64_t vbase = st->vlogBytesWritten(), ibase = g_index_bytes;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 1; i <= kUpdates; ++i) {
        bpt::UpdateResult u = st->update("user:1", {doc::inc("profile.score", 1)});
        CHECK(u.written);
        (u.consolidated ? r.fulls : r.deltas)++;
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(doc::parse(*st->get("user:1")).find("profile")->find("score")->asInt() == kUpdates);
    finish(st, dir, &r);
    r.vlog_bytes -= vbase;
    r.index_bytes = g_index_bytes - ibase;
    return r;
}

struct Latency {
    double p50_us, p99_us;
    uint32_t reads;
};

// Keys at chain depth 0..cap, then kGets gets of each.
std::vector<Latency> runGets(const std::string& dir, size_t size, uint16_t cap) {
    g_index_bytes = 0;
    auto st = IndexedStore::open(dir, benchOpts(cap));
    for (uint16_t d = 0; d <= cap; ++d) {
        const std::string key = "depth" + std::to_string(d);
        st->put(key, makeDocument(size));
        for (uint16_t i = 0; i < d; ++i)
            CHECK(st->update(key, {doc::inc("profile.score", 1)}).chain_depth == i + 1);
    }
    std::vector<Latency> out;
    for (uint16_t d = 0; d <= cap; ++d) {
        const std::string key = "depth" + std::to_string(d);
        std::vector<double> us;
        us.reserve(kGets);
        bpt::ReadStats rs;
        for (int i = 0; i < kGets; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            std::optional<std::string> got = st->get(key, nullptr, &rs);
            us.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                    .count());
            CHECK(got && rs.chain_depth == d);
        }
        std::sort(us.begin(), us.end());
        out.push_back(Latency{us[us.size() / 2], us[us.size() * 99 / 100], rs.vlog_reads});
    }
    Run r;
    finish(st, dir, &r);
    return out;
}

} // namespace

int main() {
    std::string dir = setupRun("bench_delta");
    Cleaner cleaner{dir};
    printHardware();
    // Planned vLog bytes: puts write the whole entry each time; updates write a Delta (about 100
    // bytes) except every (cap+1)-th update, which writes a consolidated Full entry.
    uint64_t planned = 0;
    for (size_t s : kSizes) {
        const uint64_t full = 32 + 8 + s, delta = 104;
        planned += kUpdates * full;
        for (uint16_t cap : kCaps)
            planned += kUpdates / (cap + 1) * full + (kUpdates - kUpdates / (cap + 1)) * delta;
    }
    std::printf("Planned: about %.0f MB of vLog writes (plus index.db checkpoints), budget %.0f "
                "MiB; files are deleted after each run.\n\n",
                planned / 1e6, kBenchBudget / 1048576.0);

    std::printf("Bytes written per update of one field (%d updates, one key):\n", kUpdates);
    std::printf("%-6s %-14s %12s %12s %12s %10s %10s %10s\n", "doc", "method", "vLog B/op",
                "index B/op", "total B/op", "ratio", "deltas", "fulls");
    for (size_t s : kSizes) {
        Run p = runPuts(dir + "/put", s);
        const double put_total = double(p.vlog_bytes + p.index_bytes) / kUpdates;
        std::printf("%-6s %-14s %12.0f %12.1f %12.0f %10s %10llu %10llu\n",
                    (std::to_string(s / 1024) + " KB").c_str(), "put (full)",
                    double(p.vlog_bytes) / kUpdates, double(p.index_bytes) / kUpdates, put_total,
                    "1.00x", 0ULL, (unsigned long long)p.fulls);
        for (uint16_t cap : kCaps) {
            Run u = runUpdates(dir + "/upd", s, cap);
            const double total = double(u.vlog_bytes + u.index_bytes) / kUpdates;
            char ratio[16];
            std::snprintf(ratio, sizeof(ratio), "%.2fx", put_total / total);
            std::printf("%-6s %-14s %12.0f %12.1f %12.0f %10s %10llu %10llu\n", "",
                        ("update cap " + std::to_string(cap)).c_str(),
                        double(u.vlog_bytes) / kUpdates, double(u.index_bytes) / kUpdates, total,
                        ratio, (unsigned long long)u.deltas, (unsigned long long)u.fulls);
        }
        std::fflush(stdout);
    }

    std::printf("\nget() latency by chain depth (%d gets per depth, page cache warm):\n", kGets);
    std::printf("%-6s %-5s %-6s %10s %10s %11s\n", "doc", "cap", "depth", "p50 us", "p99 us",
                "vLog reads");
    for (size_t s : kSizes) {
        for (uint16_t cap : kCaps) {
            std::vector<Latency> l = runGets(dir + "/get", s, cap);
            for (uint16_t d = 0; d <= cap; ++d)
                std::printf("%-6s %-5u %-6u %10.1f %10.1f %11u\n",
                            d == 0 ? (std::to_string(s / 1024) + " KB").c_str() : "", cap, d,
                            l[d].p50_us, l[d].p99_us, l[d].reads);
        }
    }
    std::printf("\n[Budget] bytes written: %llu of %llu\n", (unsigned long long)bytesWritten(),
                (unsigned long long)kBenchBudget);
    return 0;
}
