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

struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY); struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
        size = static_cast<size_t>(st.st_size);
        if (size) {
            data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
            if (data == MAP_FAILED) _exit(2);
            madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        }
    }
    ~Mapping() { if (data && data != MAP_FAILED) munmap(const_cast<char*>(data), size); if (fd >= 0) close(fd); }
};

struct Field { const char* begin; const char* end; bool quoted; };

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
    while (*p >= '0' && *p <= '9') n = n * 10u + static_cast<uint32_t>(*p++ - '0');
    return n;
}

static inline int64_t read_cents(const char*& p) noexcept {
    bool neg = (*p == '-'); if (neg) ++p;
    int64_t whole = 0;
    while (*p != '.') whole = whole * 10 + (*p++ - '0');
    ++p;
    int64_t cents = whole * 100 + int64_t(*p++ - '0') * 10 + int64_t(*p++ - '0');
    return neg ? -cents : cents;
}

static inline std::pair<uint64_t, size_t> name_hash_and_len(const Field& f) noexcept {
    uint64_t h = 14695981039346656037ULL; size_t n = 0;
    if (!f.quoted) {
        for (const char* p = f.begin; p < f.end; ++p) { h ^= static_cast<unsigned char>(*p); h *= 1099511628211ULL; ++n; }
    } else {
        for (const char* p = f.begin; p < f.end; ++p) {
            unsigned char c = static_cast<unsigned char>(*p); if (c == '"') ++p;
            h ^= static_cast<unsigned char>(*p); h *= 1099511628211ULL; ++n;
        }
    }
    return {h, n};
}

static inline void append_uint(std::string& out, uint64_t n) {
    char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n); out.append(buf, p);
}

static inline void append_cents(std::string& out, int64_t cents) {
    if (cents < 0) { out.push_back('-'); cents = -cents; }
    append_uint(out, static_cast<uint64_t>(cents / 100));
    out.push_back('.');
    out.push_back(static_cast<char>('0' + (cents / 10) % 10));
    out.push_back(static_cast<char>('0' + cents % 10));
}

struct WorkerCountryAgg {
    uint64_t count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
    int64_t amount_sum = 0, minimum = 0, maximum = 0;
};

static const char* find_boundary(const char* p, const char* data_end, const char* data_start) {
    while (p < data_end) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(data_end - p)));
        if (!nl) return data_end;
        const char* next_rec = nl + 1;
        if (next_rec < data_end && *next_rec >= '0' && *next_rec <= '9') {
            const char* q = next_rec;
            while (q < data_end && *q >= '0' && *q <= '9') ++q;
            if (q < data_end && *q == ',') {
                const char* b = (nl > data_start && *(nl - 1) == '\r') ? nl - 2 : nl - 1;
                if (b - 3 >= data_start && b[0] >= '0' && b[0] <= '9' && b[-1] >= '0' && b[-1] <= '9' && b[-2] == '.' && b[-3] >= '0' && b[-3] <= '9') {
                    return next_rec;
                }
            }
        }
        p = nl + 1;
    }
    return data_end;
}

int main() {
    Mapping reg_file("/data/regions.csv");
    std::vector<std::string> countries;
    std::unordered_map<std::string, uint16_t> country_map;
    std::vector<uint16_t> region_to_country(1'000'001, 0);

    const char* rp = reg_file.data, *rend = rp + reg_file.size;
    while (rp < rend && *rp++ != '\n') {}
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
        } else cid = it->second;
        if (region < region_to_country.size()) region_to_country[region] = static_cast<uint16_t>(cid + 1);
    }

    const uint16_t unmatched_cid = static_cast<uint16_t>(countries.size());
    countries.push_back("_UNMATCHED");
    const size_t n_countries = countries.size();

    Mapping rec_file("/data/records.csv");
    const char* recs = rec_file.data, *recs_end = recs + rec_file.size;
    if (recs_end - recs >= 3 && static_cast<unsigned char>(recs[0]) == 0xef && static_cast<unsigned char>(recs[1]) == 0xbb && static_cast<unsigned char>(recs[2]) == 0xbf) recs += 3;
    while (recs < recs_end && *recs++ != '\n') {}
    const char* data_start = recs;
    const size_t total_data_size = static_cast<size_t>(recs_end - data_start);

    constexpr unsigned WORKERS = 12;
    const char* split_points[WORKERS + 1];
    split_points[0] = data_start; split_points[WORKERS] = recs_end;
    for (unsigned w = 1; w < WORKERS; ++w) {
        split_points[w] = find_boundary(data_start + total_data_size * w / WORKERS, recs_end, data_start);
    }

    std::vector<std::vector<WorkerCountryAgg>> partial(WORKERS, std::vector<WorkerCountryAgg>(n_countries));
    std::vector<std::vector<std::vector<uint64_t>>> worker_hashes(WORKERS, std::vector<std::vector<uint64_t>>(n_countries));

    auto parse_range = [&](unsigned w) {
        const char* cur = split_points[w], *lim = split_points[w + 1];
        auto& aggs = partial[w]; auto& hashes = worker_hashes[w];
        while (cur < lim) {
            if (lim - cur >= 3 && static_cast<unsigned char>(cur[0]) == 0xef && static_cast<unsigned char>(cur[1]) == 0xbb && static_cast<unsigned char>(cur[2]) == 0xbf) cur += 3;
            const uint32_t rec_id = read_uint32(cur);
            if (cur < lim && *cur == ',') ++cur;
            const Field name = read_field(cur);
            if (cur < lim && *cur == ',') ++cur;
            const Field note = read_field(cur);
            if (cur < lim && *cur == ',') ++cur;
            const uint32_t region = read_uint32(cur);
            if (cur < lim && *cur == ',') ++cur;
            const int64_t amount = read_cents(cur);
            if (cur < lim && *cur == '\r') ++cur;
            if (cur < lim && *cur == '\n') ++cur;

            const uint16_t mapped = (region < region_to_country.size()) ? region_to_country[region] : 0;
            const size_t group = mapped ? static_cast<size_t>(mapped - 1) : unmatched_cid;

            WorkerCountryAgg& agg = aggs[group];
            if (!agg.count) { agg.minimum = agg.maximum = amount; }
            else {
                if (amount < agg.minimum) agg.minimum = amount;
                if (amount > agg.maximum) agg.maximum = amount;
            }
            ++agg.count; agg.id_sum += rec_id; agg.amount_sum += amount;
            auto [h, n] = name_hash_and_len(name);
            agg.name_bytes += n;
            hashes[group].push_back(h);

            if (!note.quoted) agg.note_bytes += static_cast<uint64_t>(note.end - note.begin);
            else {
                size_t nb = 0;
                for (const char* np = note.begin; np < note.end; ++np, ++nb) { if (*np == '"') ++np; }
                agg.note_bytes += nb;
            }
        }
    };

    std::vector<std::thread> pool; pool.reserve(WORKERS);
    for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(parse_range, i);
    for (auto& t : pool) t.join();
    pool.clear();

    struct FinalResult {
        std::string country;
        uint64_t count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
        int64_t amount_sum = 0, amount_min = 0, amount_max = 0;
        size_t distinct_names = 0;
    };
    std::vector<FinalResult> results(n_countries);

    auto reduce_country = [&](size_t c) {
        FinalResult& res = results[c]; res.country = countries[c];
        size_t total_hashes_len = 0;
        for (unsigned w = 0; w < WORKERS; ++w) total_hashes_len += worker_hashes[w][c].size();
        if (!total_hashes_len) return;

        bool first_set = true;
        for (unsigned w = 0; w < WORKERS; ++w) {
            const auto& src = partial[w][c];
            if (!src.count) continue;
            if (first_set) { res.amount_min = src.minimum; res.amount_max = src.maximum; first_set = false; }
            else {
                if (src.minimum < res.amount_min) res.amount_min = src.minimum;
                if (src.maximum > res.amount_max) res.amount_max = src.maximum;
            }
            res.count += src.count; res.id_sum += src.id_sum; res.amount_sum += src.amount_sum;
            res.name_bytes += src.name_bytes; res.note_bytes += src.note_bytes;
        }

        std::vector<uint64_t> all_h; all_h.reserve(total_hashes_len);
        for (unsigned w = 0; w < WORKERS; ++w) all_h.insert(all_h.end(), worker_hashes[w][c].begin(), worker_hashes[w][c].end());
        std::sort(all_h.begin(), all_h.end());
        res.distinct_names = static_cast<size_t>(std::unique(all_h.begin(), all_h.end()) - all_h.begin());
    };

    for (unsigned i = 0; i < WORKERS; ++i) {
        pool.emplace_back([&, i]() {
            for (size_t c = i; c < n_countries; c += WORKERS) reduce_country(c);
        });
    }
    for (auto& t : pool) t.join();
    pool.clear();

    std::vector<size_t> order; order.reserve(n_countries);
    for (size_t c = 0; c < n_countries; ++c) if (results[c].count > 0) order.push_back(c);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return results[a].country < results[b].country; });

    std::string out; out.reserve(65536);
    for (size_t c : order) {
        const auto& r = results[c];
        out.append(r.country); out.push_back(' ');
        append_uint(out, r.count); out.push_back(' ');
        append_uint(out, r.id_sum); out.push_back(' ');
        append_cents(out, r.amount_sum); out.push_back(' ');
        append_cents(out, r.amount_min); out.push_back(' ');
        append_cents(out, r.amount_max); out.push_back(' ');
        append_uint(out, r.distinct_names); out.push_back(' ');
        append_uint(out, r.name_bytes); out.push_back(' ');
        append_uint(out, r.note_bytes); out.push_back('\n');
    }

    int out_fd = open("/out/aggregates.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) _exit(3);
    const char* obuf = out.data(); size_t rem = out.size();
    while (rem > 0) {
        ssize_t written = write(out_fd, obuf, rem);
        if (written <= 0) _exit(3);
        obuf += written; rem -= static_cast<size_t>(written);
    }
    close(out_fd);
    return 0;
}
