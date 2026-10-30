#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <string_view>
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

#pragma pack(push, 2)
struct RecHash {
    uint16_t group;
    uint64_t hash;
};
#pragma pack(pop)

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
    std::vector<std::vector<RecHash>> worker_hashes(WORKERS);

    struct WorkerContext {
        unsigned w;
        const char* cur;
        const char* lim;
        const std::vector<uint16_t>* region_to_country;
        uint16_t unmatched_cid;
        std::vector<WorkerCountryAgg>* aggs;
        std::vector<RecHash>* hashes;
    };

    auto thread_worker = [](void* arg_ptr) -> void* {
        WorkerContext* ctx = static_cast<WorkerContext*>(arg_ptr);
        const char* cur = ctx->cur;
        const char* lim = ctx->lim;
        const auto& r2c = *(ctx->region_to_country);
        const uint16_t unmatched_cid = ctx->unmatched_cid;
        auto& aggs = *(ctx->aggs);
        auto& hashes = *(ctx->hashes);
        hashes.reserve(350000);

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

            const uint16_t mapped = (region < r2c.size()) ? r2c[region] : 0;
            const uint16_t group = mapped ? static_cast<uint16_t>(mapped - 1) : unmatched_cid;

            WorkerCountryAgg& agg = aggs[group];
            if (!agg.count) { agg.minimum = agg.maximum = amount; }
            else {
                if (amount < agg.minimum) agg.minimum = amount;
                if (amount > agg.maximum) agg.maximum = amount;
            }
            ++agg.count; agg.id_sum += rec_id; agg.amount_sum += amount;
            auto [h, n] = name_hash_and_len(name);
            agg.name_bytes += n;
            hashes.push_back({group, h});

            if (!note.quoted) agg.note_bytes += static_cast<uint64_t>(note.end - note.begin);
            else {
                size_t nb = 0;
                for (const char* np = note.begin; np < note.end; ++np, ++nb) { if (*np == '"') ++np; }
                agg.note_bytes += nb;
            }
        }
        // Local sort and dedup by (group, hash)
        std::sort(hashes.begin(), hashes.end(), [](const RecHash& a, const RecHash& b) {
            if (a.group != b.group) return a.group < b.group;
            return a.hash < b.hash;
        });
        auto it = std::unique(hashes.begin(), hashes.end(), [](const RecHash& a, const RecHash& b) {
            return a.group == b.group && a.hash == b.hash;
        });
        hashes.erase(it, hashes.end());
        return nullptr;
    };

    pthread_t threads[WORKERS];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 524288); // 512 KB stack size to minimize virtual memory

    WorkerContext contexts[WORKERS];
    for (unsigned w = 0; w < WORKERS; ++w) {
        contexts[w] = {w, split_points[w], split_points[w + 1], &region_to_country, unmatched_cid, &partial[w], &worker_hashes[w]};
        pthread_create(&threads[w], &attr, thread_worker, &contexts[w]);
    }
    for (unsigned w = 0; w < WORKERS; ++w) pthread_join(threads[w], nullptr);

    // Build per-worker group index boundaries in worker_hashes
    std::vector<std::vector<uint32_t>> worker_group_offsets(WORKERS, std::vector<uint32_t>(n_countries + 1, 0));
    for (unsigned w = 0; w < WORKERS; ++w) {
        const auto& wh = worker_hashes[w];
        uint32_t cur_g = 0;
        for (uint32_t i = 0; i < wh.size(); ++i) {
            while (cur_g < wh[i].group) {
                worker_group_offsets[w][++cur_g] = i;
            }
        }
        while (cur_g < n_countries) {
            worker_group_offsets[w][++cur_g] = static_cast<uint32_t>(wh.size());
        }
    }

    struct FinalResult {
        std::string country;
        uint64_t count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
        int64_t amount_sum = 0, amount_min = 0, amount_max = 0;
        size_t distinct_names = 0;
    };
    std::vector<FinalResult> results(n_countries);

    struct ReducerContext {
        unsigned thread_id;
        size_t n_countries;
        const std::vector<std::string>* countries;
        const std::vector<std::vector<WorkerCountryAgg>>* partial;
        const std::vector<std::vector<RecHash>>* worker_hashes;
        const std::vector<std::vector<uint32_t>>* worker_group_offsets;
        std::vector<FinalResult>* results;
    };

    auto reducer_worker = [](void* arg_ptr) -> void* {
        ReducerContext* ctx = static_cast<ReducerContext*>(arg_ptr);
        const unsigned tid = ctx->thread_id;
        const size_t n_countries = ctx->n_countries;
        const auto& countries = *(ctx->countries);
        const auto& partial = *(ctx->partial);
        const auto& worker_hashes = *(ctx->worker_hashes);
        const auto& worker_group_offsets = *(ctx->worker_group_offsets);
        auto& results = *(ctx->results);

        std::vector<uint64_t> merged; merged.reserve(16384);

        for (size_t c = tid; c < n_countries; c += WORKERS) {
            FinalResult& res = results[c]; res.country = countries[c];
            bool first_set = true;
            uint64_t total_c_cnt = 0;
            for (unsigned w = 0; w < WORKERS; ++w) {
                const auto& src = partial[w][c];
                if (!src.count) continue;
                total_c_cnt += src.count;
                if (first_set) { res.amount_min = src.minimum; res.amount_max = src.maximum; first_set = false; }
                else {
                    if (src.minimum < res.amount_min) res.amount_min = src.minimum;
                    if (src.maximum > res.amount_max) res.amount_max = src.maximum;
                }
                res.count += src.count; res.id_sum += src.id_sum; res.amount_sum += src.amount_sum;
                res.name_bytes += src.name_bytes; res.note_bytes += src.note_bytes;
            }
            if (!total_c_cnt) continue;

            merged.clear();
            for (unsigned w = 0; w < WORKERS; ++w) {
                const uint32_t b = worker_group_offsets[w][c];
                const uint32_t e = worker_group_offsets[w][c + 1];
                for (uint32_t i = b; i < e; ++i) merged.push_back(worker_hashes[w][i].hash);
            }
            std::sort(merged.begin(), merged.end());
            res.distinct_names = static_cast<size_t>(std::unique(merged.begin(), merged.end()) - merged.begin());
        }
        return nullptr;
    };

    ReducerContext red_contexts[WORKERS];
    for (unsigned w = 0; w < WORKERS; ++w) {
        red_contexts[w] = {w, n_countries, &countries, &partial, &worker_hashes, &worker_group_offsets, &results};
        pthread_create(&threads[w], &attr, reducer_worker, &red_contexts[w]);
    }
    for (unsigned w = 0; w < WORKERS; ++w) pthread_join(threads[w], nullptr);
    pthread_attr_destroy(&attr);

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
