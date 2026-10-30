#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

/* ── mmap helper ──────────────────────────────────────────────────────────── */
struct Mapping {
    int fd = -1;
    size_t size = 0;
    const char* data = nullptr;

    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
        size = static_cast<size_t>(st.st_size);
        if (size) {
            data = static_cast<const char*>(
                mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
            if (data == MAP_FAILED) _exit(2);
            madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        }
    }
    ~Mapping() {
        if (data && data != MAP_FAILED) munmap(const_cast<char*>(data), size);
        if (fd >= 0) close(fd);
    }
};

/* ── Fast Hash tables for interning ───────────────────────────────────────── */
struct HourTable {
    uint64_t keys[256];
    uint16_t ids[256];
    uint8_t  occupied[256] = {};

    static inline uint64_t hash_hour(const char* p) noexcept {
        uint64_t k1; std::memcpy(&k1, p, 8);
        uint64_t k2 = 0; std::memcpy(&k2, p + 8, 5);
        return k1 ^ (k2 * 0x517cc1b727220a95ULL);
    }

    void insert(std::string_view h, uint16_t id) noexcept {
        const uint64_t hval = hash_hour(h.data());
        uint32_t idx = hval & 255;
        while (occupied[idx]) idx = (idx + 1) & 255;
        keys[idx] = hval;
        ids[idx] = id;
        occupied[idx] = 1;
    }

    inline uint16_t lookup(const char* p) const noexcept {
        const uint64_t hval = hash_hour(p);
        uint32_t idx = hval & 255;
        while (occupied[idx]) {
            if (keys[idx] == hval) return ids[idx];
            idx = (idx + 1) & 255;
        }
        return 0;
    }
};

struct EndpointTable {
    static constexpr uint32_t CAP = 8192;
    static constexpr uint32_t MASK = CAP - 1;
    std::string_view keys[CAP];
    uint16_t ids[CAP];
    uint8_t  occupied[CAP] = {};

    static inline uint64_t hash_sv(const char* p, size_t len) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i) {
            h ^= static_cast<unsigned char>(p[i]);
            h *= 1099511628211ULL;
        }
        return h;
    }

    void insert(std::string_view sv, uint16_t id) noexcept {
        const uint64_t h = hash_sv(sv.data(), sv.size());
        uint32_t idx = h & MASK;
        while (occupied[idx]) idx = (idx + 1) & MASK;
        keys[idx] = sv;
        ids[idx] = id;
        occupied[idx] = 1;
    }

    inline uint16_t lookup(const char* ptr, size_t len) const noexcept {
        const uint64_t h = hash_sv(ptr, len);
        uint32_t idx = h & MASK;
        while (occupied[idx]) {
            if (keys[idx].size() == len && std::memcmp(keys[idx].data(), ptr, len) == 0) return ids[idx];
            idx = (idx + 1) & MASK;
        }
        return 0;
    }
};

struct ParsedRecord {
    uint32_t gid;
    uint32_t latency;
    bool is_5xx;
};

/* ── fast integer formatting ─────────────────────────────────────────────── */
static inline void append_u64(std::string& out, uint64_t n) {
    char buf[24];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, p);
}

int main() {
    Mapping input("/data/requests.log");
    const char* begin  = input.data;
    const char* finish = input.data + input.size;

    constexpr unsigned WORKERS = 12;
    const char* starts[WORKERS + 1];
    starts[0]       = begin;
    starts[WORKERS] = finish;
    for (unsigned i = 1; i < WORKERS; ++i) {
        const char* probe = begin + input.size * i / WORKERS;
        const char* nl    = static_cast<const char*>(
            std::memchr(probe, '\n', static_cast<size_t>(finish - probe)));
        starts[i] = nl ? nl + 1 : finish;
    }

    /* ── Pass 1: Collect unique hours and endpoints ─────────────────────── */
    std::vector<std::unordered_set<std::string>> local_hours(WORKERS);
    std::vector<std::unordered_set<std::string_view>> local_eps(WORKERS);

    auto collect_uniques = [&](unsigned w) {
        auto& hours = local_hours[w];
        auto& eps   = local_eps[w];
        const char* p     = starts[w];
        const char* limit = starts[w + 1];

        while (p < limit) {
            const char* line = p;
            const char* nl = static_cast<const char*>(
                std::memchr(line, '\n', static_cast<size_t>(limit - line)));
            if (!nl) nl = limit;
            p = nl < limit ? nl + 1 : limit;

            if (nl - line < 35) continue;
            hours.emplace(line, 13);

            const char* q = line + 27;
            if (*q != ' ') { while (q < nl && *q != ' ') ++q; }
            while (q < nl && *q == ' ') ++q;
            while (q < nl && *q != ' ') ++q; // skip method
            while (q < nl && *q == ' ') ++q;
            const char* ep_start = q;
            while (q < nl && *q != ' ') ++q;
            eps.emplace(ep_start, static_cast<size_t>(q - ep_start));
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(collect_uniques, i);
        for (auto& t : pool) t.join();
    }

    std::vector<std::string> sorted_hours;
    {
        std::unordered_set<std::string> all_h;
        for (auto& s : local_hours) all_h.insert(s.begin(), s.end());
        sorted_hours.assign(all_h.begin(), all_h.end());
        std::sort(sorted_hours.begin(), sorted_hours.end());
    }

    std::vector<std::string> sorted_eps;
    {
        std::unordered_set<std::string_view> all_e;
        for (auto& s : local_eps) all_e.insert(s.begin(), s.end());
        for (std::string_view sv : all_e) sorted_eps.emplace_back(sv);
        std::sort(sorted_eps.begin(), sorted_eps.end());
    }

    HourTable hour_table;
    for (size_t i = 0; i < sorted_hours.size(); ++i) {
        hour_table.insert(sorted_hours[i], static_cast<uint16_t>(i));
    }

    EndpointTable ep_table;
    for (size_t i = 0; i < sorted_eps.size(); ++i) {
        ep_table.insert(sorted_eps[i], static_cast<uint16_t>(i));
    }

    const uint32_t num_endpoints = static_cast<uint32_t>(sorted_eps.size());
    const uint32_t num_hours     = static_cast<uint32_t>(sorted_hours.size());
    const uint32_t num_groups    = num_hours * num_endpoints;

    /* ── Pass 2: Parse into local worker memory buffers + count groups ───── */
    std::vector<std::vector<ParsedRecord>> worker_records(WORKERS);
    std::vector<std::vector<uint32_t>> thread_counts(WORKERS, std::vector<uint32_t>(num_groups, 0));
    std::vector<std::vector<uint32_t>> thread_errors(WORKERS, std::vector<uint32_t>(num_groups, 0));

    auto parse_worker = [&](unsigned w) {
        auto& records = worker_records[w];
        records.reserve(270'000);
        auto& counts = thread_counts[w];
        auto& errors = thread_errors[w];

        const char* p     = starts[w];
        const char* limit = starts[w + 1];

        while (p < limit) {
            const char* line = p;
            const char* nl = static_cast<const char*>(
                std::memchr(line, '\n', static_cast<size_t>(limit - line)));
            if (!nl) nl = limit;
            p = nl < limit ? nl + 1 : limit;
            if (nl - line < 35) continue;

            const uint16_t hid = hour_table.lookup(line);
            const char* q = line + 27;
            if (*q != ' ') { while (q < nl && *q != ' ') ++q; }
            while (q < nl && *q == ' ') ++q;
            while (q < nl && *q != ' ') ++q;
            while (q < nl && *q == ' ') ++q;
            const char* ep_start = q;
            while (q < nl && *q != ' ') ++q;
            const size_t ep_len = static_cast<size_t>(q - ep_start);

            const uint16_t eid = ep_table.lookup(ep_start, ep_len);
            const uint32_t gid = static_cast<uint32_t>(hid) * num_endpoints + eid;

            while (q < nl && *q == ' ') ++q;
            const bool is_5xx = (*q == '5');
            while (q < nl && *q != ' ') ++q;
            while (q < nl && *q == ' ') ++q;

            uint32_t lat = 0;
            while (q < nl && *q >= '0' && *q <= '9') {
                lat = lat * 10u + static_cast<uint32_t>(*q++ - '0');
            }

            records.push_back({gid, lat, is_5xx});
            counts[gid]++;
            if (is_5xx) errors[gid]++;
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(parse_worker, i);
        for (auto& t : pool) t.join();
    }

    /* ── Compute global offsets & prefix sums ────────────────────────────── */
    std::vector<uint32_t> total_counts(num_groups, 0);
    std::vector<uint32_t> total_errors(num_groups, 0);
    for (uint32_t g = 0; g < num_groups; ++g) {
        for (unsigned w = 0; w < WORKERS; ++w) {
            total_counts[g] += thread_counts[w][g];
            total_errors[g] += thread_errors[w][g];
        }
    }

    std::vector<uint32_t> group_offsets(num_groups + 1, 0);
    for (uint32_t g = 0; g < num_groups; ++g) {
        group_offsets[g + 1] = group_offsets[g] + total_counts[g];
    }
    const uint32_t total_records = group_offsets[num_groups];

    std::vector<std::vector<uint32_t>> thread_write_ptrs(
        WORKERS, std::vector<uint32_t>(num_groups, 0));
    for (uint32_t g = 0; g < num_groups; ++g) {
        uint32_t cur = group_offsets[g];
        for (unsigned w = 0; w < WORKERS; ++w) {
            thread_write_ptrs[w][g] = cur;
            cur += thread_counts[w][g];
        }
    }

    /* ── Pass 3: Scatter latencies directly from memory buffers ─────────── */
    std::vector<uint32_t> all_latencies(total_records);

    auto scatter_worker = [&](unsigned w) {
        auto& write_ptrs = thread_write_ptrs[w];
        const auto& records = worker_records[w];
        for (const auto& rec : records) {
            all_latencies[write_ptrs[rec.gid]++] = rec.latency;
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(scatter_worker, i);
        for (auto& t : pool) t.join();
    }

    // Free worker memory buffers
    for (unsigned w = 0; w < WORKERS; ++w) {
        worker_records[w].clear();
        worker_records[w].shrink_to_fit();
    }

    /* ── Fast percentiles per group ─────────────────────────────────────── */
    auto sort_group = [&](uint32_t g) {
        const uint32_t count = total_counts[g];
        if (!count) return;

        const uint32_t offset = group_offsets[g];
        uint32_t* base = &all_latencies[offset];

        if (count == 1) return;
        if (count == 2) {
            if (base[1] < base[0]) std::swap(base[0], base[1]);
            return;
        }
        if (count <= 16) {
            std::sort(base, base + count);
            return;
        }

        const uint32_t r50 = (50u * count + 99u) / 100u - 1u;
        const uint32_t r95 = (95u * count + 99u) / 100u - 1u;
        const uint32_t r99 = (99u * count + 99u) / 100u - 1u;

        std::nth_element(base, base + r50, base + count);
        std::nth_element(base + r50 + 1, base + r95, base + count);
        std::nth_element(base + r95 + 1, base + r99, base + count);
    };

    {
        std::atomic<uint32_t> next_g{0};
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) {
            pool.emplace_back([&]() {
                for (;;) {
                    uint32_t g = next_g.fetch_add(1, std::memory_order_relaxed);
                    if (g >= num_groups) break;
                    sort_group(g);
                }
            });
        }
        for (auto& t : pool) t.join();
    }

    /* ── Format output ──────────────────────────────────────────────────── */
    std::string output;
    output.reserve(8 * 1024 * 1024);

    for (uint32_t hid = 0; hid < num_hours; ++hid) {
        const std::string& hstr = sorted_hours[hid];
        for (uint32_t eid = 0; eid < num_endpoints; ++eid) {
            const uint32_t gid = hid * num_endpoints + eid;
            const uint32_t count = total_counts[gid];
            if (!count) continue;

            const uint32_t offset = group_offsets[gid];
            const uint32_t* base = &all_latencies[offset];

            const uint32_t r50 = (50u * count + 99u) / 100u - 1u;
            const uint32_t r95 = (95u * count + 99u) / 100u - 1u;
            const uint32_t r99 = (99u * count + 99u) / 100u - 1u;

            output.append(hstr);
            output.append(":00:00Z ");
            output.append(sorted_eps[eid]);
            output.push_back(' ');
            append_u64(output, count);
            output.push_back(' ');
            append_u64(output, base[r50]);
            output.push_back(' ');
            append_u64(output, base[r95]);
            output.push_back(' ');
            append_u64(output, base[r99]);
            output.push_back(' ');
            const uint32_t errs = total_errors[gid];
            append_u64(output, errs);
            output.push_back(' ');
            const uint64_t ppm = static_cast<uint64_t>(errs) * 1000000ULL / count;
            append_u64(output, ppm);
            output.push_back('\n');
        }
    }

    /* ── Write output ───────────────────────────────────────────────────── */
    const int out_fd = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) _exit(2);

    const char* wp = output.data();
    size_t rem     = output.size();
    while (rem) {
        ssize_t written = write(out_fd, wp, rem);
        if (written <= 0) _exit(2);
        wp += written;
        rem -= static_cast<size_t>(written);
    }
    close(out_fd);
    return 0;
}
