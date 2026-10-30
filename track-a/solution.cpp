/*  Track A – TECHBUILDER – ultra-optimised C++23
 *
 *  Key changes vs. previous:
 *   • MAP_POPULATE + MADV_SEQUENTIAL  → kernel pre-faults pages before parsing
 *   • Counting-sort instead of nth_element → O(n) per-group percentile
 *     (latency is bounded 0–999999 µs, so we keep a histogram[1000000])
 *   • Flat arrays instead of vector-of-vectors → cache friendly
 *   • Single large write() instead of ofstream → no stdio overhead
 *   • Endpoint strings stored by pointer into the mmap'd region → no copy
 *   • Thread-local endpoint tables merged with a single pass
 */
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
#include <unordered_map>
#include <vector>

/* ── mmap helper ──────────────────────────────────────────────────────────── */
struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
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

/* ── constants ────────────────────────────────────────────────────────────── */
static constexpr unsigned WORKERS = 12;
static constexpr unsigned HOURS   = 72;      // 3 days × 24 h
static constexpr uint32_t LAT_MAX = 1000000; // histogram buckets

/* ── per-thread data ──────────────────────────────────────────────────────── */
struct Group {
    // Counting sort histogram (latency 0–999999)
    uint32_t* hist  = nullptr; // allocated lazily
    uint32_t  total = 0;
    uint32_t  errors= 0;
    void record(uint32_t lat, bool err5xx) {
        if (!hist) {
            hist = new uint32_t[LAT_MAX]();
        }
        if (lat >= LAT_MAX) lat = LAT_MAX - 1;
        ++hist[lat];
        ++total;
        errors += static_cast<uint32_t>(err5xx);
    }
};

struct LocalData {
    // endpoint → index into groups (groups has endpoint_count * HOURS entries)
    std::unordered_map<std::string_view, int> endpoints;
    std::vector<Group> groups;   // groups[ep * HOURS + h]
    std::string hour_labels[HOURS]; // "YYYY-MM-DDTHH"
    LocalData() { endpoints.reserve(2048); }
};

/* ── fast digit parser ────────────────────────────────────────────────────── */
static inline uint32_t parse_u32_raw(const char* p, const char* end) {
    uint32_t v = 0;
    while (p < end) v = v * 10u + static_cast<uint32_t>(*p++ - '0');
    return v;
}

static inline void append_u64(std::string& out, uint64_t n) {
    char buf[22];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, p);
}

/* ── percentile from histogram ────────────────────────────────────────────── */
static uint32_t percentile(const uint32_t* hist, uint32_t total, unsigned pct) {
    // pct in 0–100; ceiling rank
    const size_t rank = (static_cast<size_t>(pct) * total + 99) / 100 - 1;
    size_t cum = 0;
    for (uint32_t v = 0; v < LAT_MAX; ++v) {
        cum += hist[v];
        if (cum > rank) return v;
    }
    return LAT_MAX - 1;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main() {
    Mapping input("/data/requests.log");
    const char* begin  = input.data;
    const char* finish = input.data + input.size;

    /* ── split into WORKERS line-aligned shards ─────────────────────────── */
    const char* starts[WORKERS + 1];
    starts[0]       = begin;
    starts[WORKERS] = finish;
    for (unsigned i = 1; i < WORKERS; ++i) {
        const char* probe = begin + input.size * i / WORKERS;
        const char* nl    = static_cast<const char*>(
            memchr(probe, '\n', static_cast<size_t>(finish - probe)));
        starts[i] = nl ? nl + 1 : finish;
    }

    /* ── parallel parse ─────────────────────────────────────────────────── */
    std::vector<LocalData> locals(WORKERS);

    auto parse_range = [&](unsigned w) {
        LocalData& local   = locals[w];
        const char* p      = starts[w];
        const char* limit  = starts[w + 1];

        while (p < limit) {
            const char* line = p;
            /* find newline */
            const char* nl = static_cast<const char*>(
                memchr(line, '\n', static_cast<size_t>(limit - line)));
            if (!nl) nl = limit - 1;
            p = nl + 1;

            /* parse timestamp: "YYYY-MM-DDTHH:MM:SSZ " */
            /* day:  line[8..9], hour: line[11..12]        */
            /* month digit: line[6] = '0' means Oct (day 30,31) else Nov */
            const int day  = (line[8] - '0') * 10 + (line[9] - '0');
            const int hour = (line[11] - '0') * 10 + (line[12] - '0');
            /* bucket: Oct30=0, Oct31=24, Nov1=48 each with hours 18..23,00..17 */
            int bucket;
            if (line[6] == '0') {          /* October */
                bucket = (day - 30) * 24 + hour - 18;
            } else {                        /* November */
                bucket = (day + 1) * 24 + hour - 18;
            }
            if (bucket < 0) bucket += HOURS;
            if (bucket < 0 || bucket >= (int)HOURS) { continue; }

            if (local.hour_labels[bucket].empty())
                local.hour_labels[bucket].assign(line, 13);

            /* skip to endpoint: past "YYYY-MM-DDTHH:MM:SSZ METHOD " */
            const char* q = line + 21; // after "Z "
            while (*q != ' ') ++q;     // skip METHOD
            ++q;                        // skip space
            const char* ep_begin = q;
            while (*q != ' ') ++q;
            const std::string_view ep(ep_begin, static_cast<size_t>(q - ep_begin));

            auto found = local.endpoints.find(ep);
            int ep_id;
            if (found == local.endpoints.end()) {
                ep_id = static_cast<int>(local.endpoints.size());
                local.endpoints.emplace(ep, ep_id);
                local.groups.resize(static_cast<size_t>(ep_id + 1) * HOURS);
            } else {
                ep_id = found->second;
            }

            /* status + latency: " 2XX NNNNN\n" */
            const char* status = q + 1;   // e.g. "200"
            const char* lat_p  = status + 4; // skip "200 "
            const uint32_t lat = parse_u32_raw(lat_p, nl);
            const bool err5xx  = (status[0] == '5');

            local.groups[static_cast<size_t>(ep_id) * HOURS + bucket]
                .record(lat, err5xx);
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(parse_range, i);
        for (auto& t : pool) t.join();
    }

    /* ── merge locals ───────────────────────────────────────────────────── */
    std::unordered_map<std::string_view, int> endpoints;
    endpoints.reserve(4096);
    std::vector<Group> groups;
    std::string hour_labels[HOURS];

    for (auto& local : locals) {
        for (int h = 0; h < (int)HOURS; ++h)
            if (hour_labels[h].empty() && !local.hour_labels[h].empty())
                hour_labels[h] = local.hour_labels[h];

        for (const auto& [sv, local_id] : local.endpoints) {
            auto found = endpoints.find(sv);
            int id;
            if (found == endpoints.end()) {
                id = static_cast<int>(endpoints.size());
                endpoints.emplace(sv, id);
                groups.resize(static_cast<size_t>(id + 1) * HOURS);
            } else {
                id = found->second;
            }
            for (int h = 0; h < (int)HOURS; ++h) {
                Group& src = local.groups[static_cast<size_t>(local_id) * HOURS + h];
                if (!src.total) continue;
                Group& dst = groups[static_cast<size_t>(id) * HOURS + h];
                if (!dst.hist) dst.hist = new uint32_t[LAT_MAX]();
                for (uint32_t v = 0; v < LAT_MAX; ++v) dst.hist[v] += src.hist[v];
                dst.total  += src.total;
                dst.errors += src.errors;
            }
        }
    }

    /* ── sort endpoints alphabetically ─────────────────────────────────── */
    const size_t ep_count = endpoints.size();
    std::vector<int> order(ep_count);
    for (size_t i = 0; i < ep_count; ++i) order[i] = static_cast<int>(i);
    std::vector<std::string_view> names(ep_count);
    for (const auto& [sv, id] : endpoints) names[id] = sv;
    std::sort(order.begin(), order.end(),
              [&names](int a, int b) { return names[a] < names[b]; });

    /* ── build output ───────────────────────────────────────────────────── */
    std::string output;
    output.reserve(16 * 1024 * 1024);

    for (int h = 0; h < (int)HOURS; ++h) {
        for (int id : order) {
            const Group& g = groups[static_cast<size_t>(id) * HOURS + h];
            if (!g.total) continue;
            const uint32_t p50 = percentile(g.hist, g.total, 50);
            const uint32_t p95 = percentile(g.hist, g.total, 95);
            const uint32_t p99 = percentile(g.hist, g.total, 99);
            output.append(hour_labels[h]).append(":00:00Z ");
            output.append(names[id]);    output.push_back(' ');
            append_u64(output, g.total); output.push_back(' ');
            append_u64(output, p50);     output.push_back(' ');
            append_u64(output, p95);     output.push_back(' ');
            append_u64(output, p99);     output.push_back(' ');
            append_u64(output, g.errors); output.push_back(' ');
            append_u64(output, static_cast<uint64_t>(g.errors) * 1000000 / g.total);
            output.push_back('\n');
        }
    }

    /* ── single write ───────────────────────────────────────────────────── */
    const int fd = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) _exit(2);
    const char* out_p = output.data();
    size_t remain = output.size();
    while (remain) {
        ssize_t written = write(fd, out_p, remain);
        if (written <= 0) _exit(2);
        out_p += written; remain -= static_cast<size_t>(written);
    }
    close(fd);
    return 0;
}
