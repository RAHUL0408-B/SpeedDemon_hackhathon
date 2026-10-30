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

/* ── Hash tables for interning ────────────────────────────────────────────── */
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

    uint16_t lookup(const char* p) const noexcept {
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

    static inline uint64_t hash_sv(std::string_view sv) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (char c : sv) {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }

    void insert(std::string_view sv, uint16_t id) noexcept {
        const uint64_t h = hash_sv(sv);
        uint32_t idx = h & MASK;
        while (occupied[idx]) idx = (idx + 1) & MASK;
        keys[idx] = sv;
        ids[idx] = id;
        occupied[idx] = 1;
    }

    uint16_t lookup(const char* ptr, size_t len) const noexcept {
        std::string_view sv(ptr, len);
        const uint64_t h = hash_sv(sv);
        uint32_t idx = h & MASK;
        while (occupied[idx]) {
            if (keys[idx] == sv) return ids[idx];
            idx = (idx + 1) & MASK;
        }
        return 0;
    }
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

            if (nl - line < 35) continue; // safety check
            // Hour is line[0..12]
            hours.emplace(line, 13);

            // Skip to method space
            const char* q = line + 27;
            if (*q != ' ') {
                while (q < nl && *q != ' ') ++q;
            }
            // Skip method
            while (q < nl && *q == ' ') ++q;
            while (q < nl && *q != ' ') ++q;
            // Now at endpoint
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

    /* ── Pass 2: Count records per group per worker ─────────────────────── */
    std::vector<std::vector<uint32_t>> thread_counts(
        WORKERS, std::vector<uint32_t>(num_groups, 0));
    std::vector<std::vector<uint32_t>> thread_errors(
        WORKERS, std::vector<uint32_t>(num_groups, 0));

    auto count_records = [&](unsigned w) {
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

            ++counts[gid];
            errors[gid] += static_cast<uint32_t>(is_5xx);
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(count_records, i);
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

    /* ── Pass 3: Fill latencies into single flat buffer ─────────────────── */
    std::vector<uint32_t> all_latencies(total_records);

    auto fill_latencies = [&](unsigned w) {
        auto& write_ptrs = thread_write_ptrs[w];
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

            // skip status
            while (q < nl && *q == ' ') ++q;
            while (q < nl && *q != ' ') ++q;
            // now at latency
            while (q < nl && *q == ' ') ++q;
            uint32_t lat = 0;
            while (q < nl && *q >= '0' && *q <= '9') {
                lat = lat * 10u + static_cast<uint32_t>(*q++ - '0');
            }

            all_latencies[write_ptrs[gid]++] = lat;
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(fill_latencies, i);
        for (auto& t : pool) t.join();
    }

    /* ── Pass 4: Compute percentiles & format output in parallel ─────────── */
    std::vector<uint32_t> active_groups;
    active_groups.reserve(num_groups / 2);
    for (uint32_t g = 0; g < num_groups; ++g) {
        if (total_counts[g] > 0) active_groups.push_back(g);
    }

    const size_t n_active = active_groups.size();
    std::vector<std::string> thread_outputs(WORKERS);

    auto format_worker = [&](unsigned w) {
        std::string& out = thread_outputs[w];
        out.reserve(2 * 1024 * 1024);

        const size_t first = n_active * w / WORKERS;
        const size_t last  = n_active * (w + 1) / WORKERS;

        for (size_t i = first; i < last; ++i) {
            const uint32_t gid = active_groups[i];
            const uint32_t hid = gid / num_endpoints;
            const uint32_t eid = gid % num_endpoints;

            const uint32_t start = group_offsets[gid];
            const uint32_t count = total_counts[gid];
            uint32_t* base = &all_latencies[start];

            const size_t r50 = (50ULL * count + 99) / 100 - 1;
            const size_t r95 = (95ULL * count + 99) / 100 - 1;
            const size_t r99 = (99ULL * count + 99) / 100 - 1;

            if (count <= 64) {
                std::sort(base, base + count);
            } else {
                std::nth_element(base, base + r50, base + count);
                std::nth_element(base + r50 + 1, base + r95, base + count);
                std::nth_element(base + r95 + 1, base + r99, base + count);
            }

            const uint32_t p50 = base[r50];
            const uint32_t p95 = base[r95];
            const uint32_t p99 = base[r99];
            const uint32_t errs = total_errors[gid];
            const uint64_t ppm  = static_cast<uint64_t>(errs) * 1000000ULL / count;

            out.append(sorted_hours[hid]);
            out.append(":00:00Z ");
            out.append(sorted_eps[eid]);
            out.push_back(' ');
            append_u64(out, count);
            out.push_back(' ');
            append_u64(out, p50);
            out.push_back(' ');
            append_u64(out, p95);
            out.push_back(' ');
            append_u64(out, p99);
            out.push_back(' ');
            append_u64(out, errs);
            out.push_back(' ');
            append_u64(out, ppm);
            out.push_back('\n');
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(format_worker, i);
        for (auto& t : pool) t.join();
    }

    /* ── Pass 5: Write output directly via write() ──────────────────────── */
    const int out_fd = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) _exit(2);

    for (unsigned w = 0; w < WORKERS; ++w) {
        const char* wp = thread_outputs[w].data();
        size_t rem     = thread_outputs[w].size();
        while (rem) {
            ssize_t written = write(out_fd, wp, rem);
            if (written <= 0) _exit(2);
            wp += written;
            rem -= static_cast<size_t>(written);
        }
    }
    close(out_fd);
    return 0;
}
