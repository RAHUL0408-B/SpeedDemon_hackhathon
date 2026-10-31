#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY); struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
        size = static_cast<size_t>(st.st_size);
        if (size) {
            // Two spare zero pages follow the file, so the eight-byte and 32-byte reads at its end stay in bounds.
            void* base = mmap(nullptr, size + 8192, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base == MAP_FAILED || mmap(base, size, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) _exit(2);
            data = static_cast<const char*>(base);
            madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        }
    }
    ~Mapping() { if (data) munmap(const_cast<char*>(data), size + 8192); if (fd >= 0) close(fd); }
};

// begin..end is the raw text between the delimiting quotes (or the unquoted bytes); pairs counts the "" inside it.
struct Field { const char* begin; const char* end; size_t pairs; bool quoted; };

static inline const char* find_quote(const char* p, const char* lim) noexcept {
    const __m256i quote = _mm256_set1_epi8('"');
    for (; p + 32 <= lim; p += 32) {
        const uint32_t m = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)), quote)));
        if (m) return p + __builtin_ctz(m);
    }
    return static_cast<const char*>(std::memchr(p, '"', static_cast<size_t>(lim - p)));
}

static inline Field read_field(const char*& p, const char* lim) noexcept {
    if (*p != '"') {
        const char* b = p;
        while (*p != ',' && *p != '\r' && *p != '\n') ++p;
        return {b, p, 0, false};
    }
    const char* b = ++p;
    size_t pairs = 0;
    for (;;) {
        const char* q = find_quote(p, lim);
        if (q[1] == '"') { ++pairs; p = q + 2; continue; }
        p = q + 1;
        return {b, q, pairs, true};
    }
}

// Value of eight decimal digits held as bytes 0..9, first digit in the lowest byte.
static inline uint32_t digits8(uint64_t w) noexcept {
    w = ((w & 0x0F0F0F0F0F0F0F0FULL) * 2561) >> 8;
    w = ((w & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;
    return static_cast<uint32_t>(((w & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32);
}

// Reads a run of decimal digits and leaves p on the byte after it. XOR with '0' turns digits into 0..9 and
// every delimiter (comma, dot, CR, LF) into a value above 9, so one mask finds where the number ends.
static inline uint32_t read_uint32(const char*& p) noexcept {
    uint64_t w; std::memcpy(&w, p, 8);
    w ^= 0x3030303030303030ULL;
    const uint64_t stop = ((w + 0x7676767676767676ULL) | w) & 0x8080808080808080ULL;
    if (stop) {
        const unsigned digits = static_cast<unsigned>(__builtin_ctzll(stop)) >> 3;
        if (!digits) return 0;
        p += digits;
        return digits8(w << ((8 - digits) * 8));
    }
    uint32_t n = digits8(w);
    for (p += 8; *p >= '0' && *p <= '9'; ++p) n = n * 10u + static_cast<uint32_t>(*p - '0');
    return n;
}

static inline int64_t read_cents(const char*& p) noexcept {
    const bool neg = (*p == '-'); if (neg) ++p;
    const int64_t whole = read_uint32(p);
    ++p;
    const int64_t cents = whole * 100 + int64_t(p[0] - '0') * 10 + int64_t(p[1] - '0');
    p += 2;
    return neg ? -cents : cents;
}

// 64-bit hash, eight bytes per step; the length is mixed in so trailing-space variants differ.
static inline uint64_t hash_bytes(const char* p, size_t n) noexcept {
    uint64_t h = (n + 1) * 0x9E3779B97F4A7C15ULL;
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w; std::memcpy(&w, p, 8);
        h = (h ^ w) * 0xD6E8FEB86659FD93ULL; h ^= h >> 32;
    }
    if (n) {
        uint64_t w = 0; std::memcpy(&w, p, n);
        h = (h ^ w) * 0xD6E8FEB86659FD93ULL; h ^= h >> 32;
    }
    h *= 0x9E3779B97F4A7C15ULL;
    return h ^ (h >> 29);
}

// Hash and byte length of a name's value, with each "" counted and hashed as one quote.
static inline std::pair<uint64_t, size_t> name_hash_and_len(const Field& f, std::string& scratch) {
    if (!f.pairs) return {hash_bytes(f.begin, static_cast<size_t>(f.end - f.begin)), static_cast<size_t>(f.end - f.begin)};
    scratch.clear();
    for (const char* p = f.begin; p < f.end; ++p) {
        scratch.push_back(*p);
        if (*p == '"') ++p;
    }
    return {hash_bytes(scratch.data(), scratch.size()), scratch.size()};
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
    // Country codes are three capital letters, so index them as base-26 numbers.
    std::vector<uint16_t> code_to_cid(26 * 26 * 26, 0xFFFF);
    std::vector<std::string> countries;
    std::vector<uint16_t> region_to_country(1'000'001, 0);

    const char* rp = reg_file.data, *rend = rp + reg_file.size;
    while (rp < rend && *rp++ != '\n') {}
    while (rp < rend) {
        const uint32_t region = read_uint32(rp);
        if (rp < rend && *rp == ',') ++rp;
        if (rend - rp < 3) break;
        uint16_t& cid = code_to_cid[static_cast<size_t>(rp[0] - 'A') * 676 + static_cast<size_t>(rp[1] - 'A') * 26 + static_cast<size_t>(rp[2] - 'A')];
        if (cid == 0xFFFF) { cid = static_cast<uint16_t>(countries.size()); countries.emplace_back(rp, 3); }
        rp += 3;
        while (rp < rend && (*rp == '\r' || *rp == '\n')) ++rp;
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

    // Distinct names per country: a lock-free table maps each name hash to a slot, and every country
    // owns one bit per slot. Counting a country's distinct names is then a popcount of its row.
    constexpr uint32_t NAME_SLOTS = 1u << 18, ROW_WORDS = NAME_SLOTS / 64;
    // calloc'd pages are zero lazily, and the worker threads take the faults instead of the main thread.
    auto* const name_table = static_cast<std::atomic<uint64_t>*>(std::calloc(NAME_SLOTS, sizeof(uint64_t)));
    auto* const seen = static_cast<std::atomic<uint64_t>*>(std::calloc(n_countries * ROW_WORDS, sizeof(uint64_t)));

    auto parse_range = [&](unsigned w) {
        const char* cur = split_points[w], *lim = split_points[w + 1];
        auto& aggs = partial[w];
        std::string scratch;

        // Records are parsed into a small batch first. The random accesses (region lookup, name table, country
        // bitmap) are prefetched while the batch fills and again while it drains, so their latency overlaps.
        struct Rec { uint64_t hash; int64_t amount; uint32_t id, region, name_len, note_len; uint32_t group, slot; };
        constexpr unsigned BATCH = 32;
        Rec batch[BATCH];
        unsigned nb = 0;

        auto drain = [&]() {
            for (unsigned i = 0; i < nb; ++i) {
                Rec& r = batch[i];
                const uint16_t mapped = (r.region < region_to_country.size()) ? region_to_country[r.region] : 0;
                r.group = mapped ? static_cast<uint32_t>(mapped - 1) : unmatched_cid;
                uint32_t slot = static_cast<uint32_t>(r.hash) & (NAME_SLOTS - 1);
                for (;;) {
                    uint64_t key = name_table[slot].load(std::memory_order_relaxed);
                    if (key == 0 && name_table[slot].compare_exchange_strong(key, r.hash, std::memory_order_relaxed)) break;
                    if (key == r.hash) break;
                    slot = (slot + 1) & (NAME_SLOTS - 1);
                }
                r.slot = slot;
                __builtin_prefetch(&seen[static_cast<size_t>(r.group) * ROW_WORDS + (slot >> 6)], 1);
            }
            for (unsigned i = 0; i < nb; ++i) {
                const Rec& r = batch[i];
                WorkerCountryAgg& agg = aggs[r.group];
                if (!agg.count) { agg.minimum = agg.maximum = r.amount; }
                else {
                    if (r.amount < agg.minimum) agg.minimum = r.amount;
                    if (r.amount > agg.maximum) agg.maximum = r.amount;
                }
                ++agg.count; agg.id_sum += r.id; agg.amount_sum += r.amount;
                agg.name_bytes += r.name_len; agg.note_bytes += r.note_len;
                const uint64_t bit = 1ULL << (r.slot & 63);
                std::atomic<uint64_t>& word = seen[static_cast<size_t>(r.group) * ROW_WORDS + (r.slot >> 6)];
                if (!(word.load(std::memory_order_relaxed) & bit)) word.fetch_or(bit, std::memory_order_relaxed);
            }
            nb = 0;
        };

        while (cur < lim) {
            if (lim - cur >= 3 && static_cast<unsigned char>(cur[0]) == 0xef && static_cast<unsigned char>(cur[1]) == 0xbb && static_cast<unsigned char>(cur[2]) == 0xbf) cur += 3;
            const uint32_t rec_id = read_uint32(cur);
            if (cur < lim && *cur == ',') ++cur;
            const Field name = read_field(cur, lim);
            if (cur < lim && *cur == ',') ++cur;
            const Field note = read_field(cur, lim);
            if (cur < lim && *cur == ',') ++cur;
            const uint32_t region = read_uint32(cur);
            if (cur < lim && *cur == ',') ++cur;
            const int64_t amount = read_cents(cur);
            if (cur < lim && *cur == '\r') ++cur;
            if (cur < lim && *cur == '\n') ++cur;

            auto [h, n] = name_hash_and_len(name, scratch);
            if (!h) h = 1;
            __builtin_prefetch(&name_table[static_cast<uint32_t>(h) & (NAME_SLOTS - 1)], 1);
            if (region < region_to_country.size()) __builtin_prefetch(&region_to_country[region]);
            batch[nb++] = {h, amount, rec_id, region, static_cast<uint32_t>(n),
                           static_cast<uint32_t>(static_cast<uint64_t>(note.end - note.begin) - note.pairs), 0, 0};
            if (nb == BATCH) drain();
        }
        drain();
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

    for (size_t c = 0; c < n_countries; ++c) {
        FinalResult& res = results[c]; res.country = countries[c];
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
        for (uint32_t i = 0; i < ROW_WORDS; ++i) res.distinct_names += static_cast<size_t>(__builtin_popcountll(seen[c * ROW_WORDS + i].load(std::memory_order_relaxed)));
    }

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
    _exit(0);  // the answer is on disk; skip the destructors and their frees
}
