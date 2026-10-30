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

/* ── Field parsing ────────────────────────────────────────────────────────── */
struct Field {
    const char* begin;
    const char* end;
    bool quoted;
};

static inline Field read_field(const char*& p) noexcept {
    if (*p != '"') {
        const char* b = p;
        while (*p != ',' && *p != '\r' && *p != '\n') ++p;
        return {b, p, false};
    }
    const char* b = ++p;
    for (;;) {
        if (*p != '"') { ++p; continue; }
        if (p[1] == '"') { p += 2; continue; }
        const char* e = p++;
        return {b, e, true};
    }
}

static inline uint32_t read_uint32(const char*& p) noexcept {
    uint32_t n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10u + static_cast<uint32_t>(*p++ - '0');
    }
    return n;
}

static inline int64_t read_cents(const char*& p) noexcept {
    bool neg = (*p == '-');
    if (neg) ++p;
    int64_t whole = 0;
    while (*p != '.') {
        whole = whole * 10 + (*p++ - '0');
    }
    ++p; // skip '.'
    int64_t cents = whole * 100 + int64_t(*p++ - '0') * 10 + int64_t(*p++ - '0');
    return neg ? -cents : cents;
}

/* ── Hash & length for name ───────────────────────────────────────────────── */
static inline std::pair<uint64_t, size_t>
name_hash_and_len(const Field& f) noexcept {
    uint64_t h = 14695981039346656037ULL;
    size_t   n = 0;
    if (!f.quoted) {
        for (const char* p = f.begin; p < f.end; ++p) {
            h ^= static_cast<unsigned char>(*p);
            h *= 1099511628211ULL;
            ++n;
        }
    } else {
        for (const char* p = f.begin; p < f.end; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') ++p; // skip first of ""
            h ^= static_cast<unsigned char>(*p);
            h *= 1099511628211ULL;
            ++n;
        }
    }
    return {h, n};
}

/* ── Output formatting helpers ────────────────────────────────────────────── */
static inline void append_uint(std::string& out, uint64_t n) {
    char buf[24];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, p);
}

static inline void append_cents(std::string& out, int64_t cents) {
    if (cents < 0) { out.push_back('-'); cents = -cents; }
    append_uint(out, static_cast<uint64_t>(cents / 100));
    out.push_back('.');
    out.push_back(static_cast<char>('0' + (cents / 10) % 10));
    out.push_back(static_cast<char>('0' + cents % 10));
}

/* ── Per-worker aggregation struct ────────────────────────────────────────── */
struct WorkerCountryAgg {
    uint64_t count = 0;
    uint64_t id_sum = 0;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    int64_t  amount_sum = 0;
    int64_t  minimum = 0;
    int64_t  maximum = 0;
    std::vector<uint64_t> name_hashes;
};

/* ═══════════════════════════════════════════════════════════════════════════ */
int main() {
    /* ── 1. Load regions.csv ─────────────────────────────────────────────── */
    Mapping reg_file("/data/regions.csv");
    std::vector<std::string> countries;
    std::unordered_map<std::string, uint16_t> country_map;
    std::vector<uint16_t> region_to_country(10'000'000, 0);

    const char* rp   = reg_file.data;
    const char* rend = rp + reg_file.size;
    while (rp < rend && *rp++ != '\n') {} // skip header line

    while (rp < rend) {
        const uint32_t region = read_uint32(rp);
        if (rp < rend && *rp == ',') ++rp;
        const char* cs = rp;
        while (rp < rend && *rp != '\r' && *rp != '\n') ++rp;
        std::string country(cs, rp);
        while (rp < rend && (*rp == '\r' || *rp == '\n')) ++rp;

        auto it = country_map.find(country);
        uint16_t cid;
        if (it == country_map.end()) {
            cid = static_cast<uint16_t>(countries.size());
            country_map.emplace(country, cid);
            countries.push_back(country);
        } else {
            cid = it->second;
        }
        if (region < region_to_country.size()) {
            region_to_country[region] = static_cast<uint16_t>(cid + 1); // 0 = unmatched
        }
    }

    const uint16_t unmatched_cid = static_cast<uint16_t>(countries.size());
    countries.push_back("_UNMATCHED");
    const size_t n_countries = countries.size();

    /* ── 2. Scan records.csv to find 12 chunk split points ────────────────── */
    Mapping rec_file("/data/records.csv");
    const char* recs     = rec_file.data;
    const char* recs_end = recs + rec_file.size;

    // Skip BOM if present at start of file
    if (recs_end - recs >= 3 &&
        static_cast<unsigned char>(recs[0]) == 0xef &&
        static_cast<unsigned char>(recs[1]) == 0xbb &&
        static_cast<unsigned char>(recs[2]) == 0xbf) {
        recs += 3;
    }
    // Skip header line
    while (recs < recs_end && *recs++ != '\n') {}
    const char* data_start = recs;
    const size_t total_data_size = static_cast<size_t>(recs_end - data_start);

    constexpr unsigned WORKERS = 12;
    const char* split_points[WORKERS + 1];
    split_points[0]       = data_start;
    split_points[WORKERS] = recs_end;

    {
        bool in_quote = false;
        size_t next_idx = 1;
        const char* next_target = data_start + total_data_size * next_idx / WORKERS;
        const char* p = data_start;

        while (p < recs_end) {
            if (*p == '"') {
                if (in_quote && p + 1 < recs_end && p[1] == '"') {
                    ++p; // skip escaped quote
                } else {
                    in_quote = !in_quote;
                }
            } else if (*p == '\n' && !in_quote) {
                if (p >= next_target && next_idx < WORKERS) {
                    split_points[next_idx++] = p + 1;
                    next_target = data_start + total_data_size * next_idx / WORKERS;
                }
            }
            ++p;
        }
        while (next_idx < WORKERS) {
            split_points[next_idx++] = recs_end;
        }
    }

    /* ── 3. Parallel parse records ───────────────────────────────────────── */
    std::vector<std::vector<WorkerCountryAgg>> partial(
        WORKERS, std::vector<WorkerCountryAgg>(n_countries));
    for (unsigned w = 0; w < WORKERS; ++w) {
        for (size_t c = 0; c < n_countries; ++c) {
            partial[w][c].name_hashes.reserve(1024);
        }
    }

    auto parse_range = [&](unsigned w) {
        const char* cur = split_points[w];
        const char* lim = split_points[w + 1];
        auto& aggs = partial[w];

        while (cur < lim) {
            // Skip BOM if at chunk boundary
            if (lim - cur >= 3 &&
                static_cast<unsigned char>(cur[0]) == 0xef &&
                static_cast<unsigned char>(cur[1]) == 0xbb &&
                static_cast<unsigned char>(cur[2]) == 0xbf) {
                cur += 3;
            }

            const uint32_t rec_id = read_uint32(cur);
            if (cur < lim && *cur == ',') ++cur;

            const Field name = read_field(cur);
            if (cur < lim && *cur == ',') ++cur;

            const Field note = read_field(cur);
            if (cur < lim && *cur == ',') ++cur;

            const uint32_t region = read_uint32(cur);
            if (cur < lim && *cur == ',') ++cur;

            const int64_t amount  = read_cents(cur);
            if (cur < lim && *cur == '\r') ++cur;
            if (cur < lim && *cur == '\n') ++cur;

            const uint16_t mapped = (region < region_to_country.size())
                                    ? region_to_country[region] : 0;
            const size_t group = mapped ? static_cast<size_t>(mapped - 1) : unmatched_cid;

            WorkerCountryAgg& agg = aggs[group];
            if (!agg.count) {
                agg.minimum = agg.maximum = amount;
            } else {
                if (amount < agg.minimum) agg.minimum = amount;
                if (amount > agg.maximum) agg.maximum = amount;
            }
            ++agg.count;
            agg.id_sum += rec_id;
            agg.amount_sum += amount;

            auto [h, n] = name_hash_and_len(name);
            agg.name_bytes += n;
            agg.name_hashes.push_back(h);

            if (!note.quoted) {
                agg.note_bytes += static_cast<uint64_t>(note.end - note.begin);
            } else {
                size_t nb = 0;
                for (const char* np = note.begin; np < note.end; ++np, ++nb) {
                    if (*np == '"') ++np;
                }
                agg.note_bytes += nb;
            }
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(parse_range, i);
        for (auto& t : pool) t.join();
    }

    /* ── 4. Parallel distinct-name counting and reduction per country ─────── */
    struct FinalResult {
        std::string country;
        uint64_t count = 0;
        uint64_t id_sum = 0;
        int64_t  amount_sum = 0;
        int64_t  amount_min = 0;
        int64_t  amount_max = 0;
        size_t   distinct_names = 0;
        uint64_t name_bytes = 0;
        uint64_t note_bytes = 0;
    };

    std::vector<FinalResult> results(n_countries);

    auto reduce_country = [&](size_t c) {
        FinalResult& res = results[c];
        res.country = countries[c];

        size_t total_hashes_len = 0;
        for (unsigned w = 0; w < WORKERS; ++w) {
            total_hashes_len += partial[w][c].name_hashes.size();
        }
        if (!total_hashes_len) return;

        bool first_set = true;
        for (unsigned w = 0; w < WORKERS; ++w) {
            const auto& src = partial[w][c];
            if (!src.count) continue;
            if (first_set) {
                res.amount_min = src.minimum;
                res.amount_max = src.maximum;
                first_set = false;
            } else {
                if (src.minimum < res.amount_min) res.amount_min = src.minimum;
                if (src.maximum > res.amount_max) res.amount_max = src.maximum;
            }
            res.count      += src.count;
            res.id_sum     += src.id_sum;
            res.amount_sum += src.amount_sum;
            res.name_bytes += src.name_bytes;
            res.note_bytes += src.note_bytes;
        }

        std::vector<uint64_t> all_h;
        all_h.reserve(total_hashes_len);
        for (unsigned w = 0; w < WORKERS; ++w) {
            const auto& src = partial[w][c];
            all_h.insert(all_h.end(), src.name_hashes.begin(), src.name_hashes.end());
        }
        std::sort(all_h.begin(), all_h.end());
        res.distinct_names = static_cast<size_t>(
            std::unique(all_h.begin(), all_h.end()) - all_h.begin());
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(WORKERS);
        std::atomic<size_t> next_c{0};
        for (unsigned i = 0; i < WORKERS; ++i) {
            pool.emplace_back([&]() {
                for (;;) {
                    size_t c = next_c.fetch_add(1, std::memory_order_relaxed);
                    if (c >= n_countries) break;
                    reduce_country(c);
                }
            });
        }
        for (auto& t : pool) t.join();
    }

    /* ── 5. Sort countries in UTF-8 byte order and format output ──────────── */
    std::vector<size_t> order;
    order.reserve(n_countries);
    for (size_t c = 0; c < n_countries; ++c) {
        if (results[c].count > 0) order.push_back(c);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return results[a].country < results[b].country;
    });

    std::string output;
    output.reserve(64 * 1024);

    for (size_t idx : order) {
        const auto& r = results[idx];
        output.append(r.country);
        output.push_back(' ');
        append_uint(output, r.count);
        output.push_back(' ');
        append_uint(output, r.id_sum);
        output.push_back(' ');
        append_cents(output, r.amount_sum);
        output.push_back(' ');
        append_cents(output, r.amount_min);
        output.push_back(' ');
        append_cents(output, r.amount_max);
        output.push_back(' ');
        append_uint(output, r.distinct_names);
        output.push_back(' ');
        append_uint(output, r.name_bytes);
        output.push_back(' ');
        append_uint(output, r.note_bytes);
        output.push_back('\n');
    }

    const int out_fd = open("/out/aggregates.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
