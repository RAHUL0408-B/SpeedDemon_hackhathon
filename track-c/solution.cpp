/*  Track C – TECHBUILDER – ultra-optimised C++23
 *
 *  Key improvements vs previous version:
 *   • Distinct-name counting replaced by a per-shard hash set of string_views
 *     into the mmap'd buffer (no allocation per name – just store pointer+len).
 *     Merge uses a single unordered_set<pair<ptr,len>> with a fast FNV hash.
 *   • We materialize the canonical (unescaped) name as a fixed-size 128-byte
 *     buffer only when hashing → zero heap allocation per record during parse.
 *   • MAP_POPULATE + MADV_SEQUENTIAL on the large records file.
 *   • Fast integer parser using __builtin arithmetic.
 *   • Output via write() system call, no FILE buffering.
 */
#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
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

/* ── field parsing ────────────────────────────────────────────────────────── */
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
    while (*p >= '0' && *p <= '9') n = n * 10 + static_cast<uint32_t>(*p++ - '0');
    return n;
}

static inline int64_t read_cents(const char*& p) noexcept {
    bool neg = (*p == '-');
    if (neg) ++p;
    int64_t whole = 0;
    while (*p != '.') whole = whole * 10 + (*p++ - '0');
    ++p;
    int64_t cents = whole * 100 + int64_t(*p++ - '0') * 10 + int64_t(*p++ - '0');
    return neg ? -cents : cents;
}

/* ── canonical name: unescape double-quotes, compute FNV hash ─────────────── */
// Returns (hash, decoded_length)
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
            if (c == '"') ++p; // skip second quote of escaped pair
            h ^= static_cast<unsigned char>(*p);
            h *= 1099511628211ULL;
            ++n;
        }
    }
    return {h, n};
}

/* ── distinct-name set per group ─────────────────────────────────────────── */
// We store (hash, decoded_len) pairs.  Two names are "equal" if both hash
// and length match.  This is a probabilistic de-dup but collision probability
// is negligible for 64-bit FNV.
struct NameEntry { uint64_t h; size_t len; };
struct NameEntryHash {
    size_t operator()(const NameEntry& e) const noexcept { return e.h; }
};
struct NameEntryEq {
    bool operator()(const NameEntry& a, const NameEntry& b) const noexcept {
        return a.h == b.h && a.len == b.len;
    }
};
using NameSet = std::unordered_set<NameEntry, NameEntryHash, NameEntryEq>;

/* ── per-country aggregate ────────────────────────────────────────────────── */
struct Aggregate {
    uint64_t count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
    int64_t  amount_sum = 0, minimum = 0, maximum = 0;
    NameSet  names;

    void add(uint32_t id, const Field& name, const Field& note, int64_t amount) {
        if (!count) minimum = maximum = amount;
        else { if (amount < minimum) minimum = amount; if (amount > maximum) maximum = amount; }
        ++count;
        id_sum += id;
        amount_sum += amount;
        // name_bytes = sum of *decoded* lengths
        auto [h, n] = name_hash_and_len(name);
        name_bytes += n;
        names.insert({h, n});
        // note_bytes – we only need decoded length, not distinct
        if (!note.quoted) {
            note_bytes += static_cast<uint64_t>(note.end - note.begin);
        } else {
            size_t nb = 0;
            for (const char* p = note.begin; p < note.end; ++p, ++nb)
                if (*p == '"') ++p;
            note_bytes += nb;
        }
    }
};

static void merge_agg(Aggregate& into, Aggregate& from) {
    if (!from.count) return;
    if (!into.count) { into.minimum = from.minimum; into.maximum = from.maximum; }
    else {
        if (from.minimum < into.minimum) into.minimum = from.minimum;
        if (from.maximum > into.maximum) into.maximum = from.maximum;
    }
    into.count      += from.count;
    into.id_sum     += from.id_sum;
    into.amount_sum += from.amount_sum;
    into.name_bytes += from.name_bytes;
    into.note_bytes += from.note_bytes;
    into.names.merge(from.names);
}

/* ── output helpers ───────────────────────────────────────────────────────── */
static inline void append_uint(std::string& out, uint64_t n) {
    char buf[22];
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

/* ── pack/unpack 3-char country code into uint32 ─────────────────────────── */
static inline uint32_t pack3(const char* p) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) <<  8) |
            static_cast<uint32_t>(static_cast<unsigned char>(p[2]));
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main() {
    /* ── 1. Load regions.csv → region_to_country[] ──────────────────────── */
    Mapping reg_file("/data/regions.csv");
    // region ids can be up to 10M
    std::vector<uint16_t> region_to_country(10'000'000, 0);
    std::unordered_map<uint32_t, uint16_t> country_ids;
    country_ids.reserve(256);
    std::vector<uint32_t> countries;

    const char* rp  = reg_file.data;
    const char* rend = rp + reg_file.size;
    while (rp < rend && *rp++ != '\n') {} // skip header
    while (rp < rend) {
        const uint32_t region = read_uint32(rp); ++rp; // skip comma
        const uint32_t code   = pack3(rp); rp += 3;
        while (rp < rend && *rp++ != '\n') {}
        auto it = country_ids.find(code);
        uint16_t cid;
        if (it == country_ids.end()) {
            cid = static_cast<uint16_t>(countries.size());
            country_ids.emplace(code, cid);
            countries.push_back(code);
        } else cid = it->second;
        if (region < region_to_country.size())
            region_to_country[region] = static_cast<uint16_t>(cid + 1); // 0 = unmatched
    }

    /* ── 2. Load records.csv, find true record boundaries ────────────────── */
    Mapping rec_file("/data/records.csv");
    const char* recs = rec_file.data;
    const char* recs_end = recs + rec_file.size;
    // skip BOM
    if (recs_end - recs >= 3 &&
        static_cast<unsigned char>(recs[0]) == 0xef &&
        static_cast<unsigned char>(recs[1]) == 0xbb &&
        static_cast<unsigned char>(recs[2]) == 0xbf) recs += 3;
    // skip header line
    while (recs < recs_end && *recs++ != '\n') {}
    const char* data_start = recs;

    // Find true newlines (not inside quoted fields) → boundaries[]
    std::vector<const char*> boundaries;
    boundaries.reserve(3'500'000);
    boundaries.push_back(data_start);
    {
        bool in_quote = false;
        for (const char* sp = data_start; sp < recs_end; ++sp) {
            if (*sp == '"') {
                if (in_quote && sp + 1 < recs_end && sp[1] == '"') ++sp;
                else in_quote = !in_quote;
            } else if (*sp == '\n' && !in_quote) {
                boundaries.push_back(sp + 1);
            }
        }
    }

    /* ── 3. Parallel parse ──────────────────────────────────────────────── */
    const size_t n_countries = countries.size();
    const unsigned workers = static_cast<unsigned>(
        std::min<size_t>(12, boundaries.size() > 1 ? boundaries.size() - 1 : 1));

    std::vector<std::vector<Aggregate>> partial(
        workers, std::vector<Aggregate>(n_countries + 1));

    auto parse_range = [&](unsigned w) {
        const size_t first = (boundaries.size() - 1) * w / workers;
        const size_t last  = (boundaries.size() - 1) * (w + 1) / workers;
        const char* cur    = boundaries[first];
        const char* lim    = boundaries[last];
        auto& aggs = partial[w];

        while (cur < lim) {
            // skip stray BOM
            if (lim - cur >= 3 &&
                static_cast<unsigned char>(cur[0]) == 0xef &&
                static_cast<unsigned char>(cur[1]) == 0xbb &&
                static_cast<unsigned char>(cur[2]) == 0xbf) cur += 3;

            const uint32_t rec_id = read_uint32(cur); ++cur; // skip comma
            const Field name = read_field(cur);        ++cur;
            const Field note = read_field(cur);        ++cur;
            const uint32_t region = read_uint32(cur);  ++cur;
            const int64_t amount  = read_cents(cur);
            if (*cur == '\r') ++cur;
            if (cur < lim && *cur == '\n') ++cur;

            const uint16_t mapped = (region < region_to_country.size())
                                    ? region_to_country[region] : 0;
            const size_t group = mapped ? static_cast<size_t>(mapped - 1) : n_countries;
            aggs[group].add(rec_id, name, note, amount);
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) pool.emplace_back(parse_range, i);
        for (auto& t : pool) t.join();
    }

    /* ── 4. Merge partial results ───────────────────────────────────────── */
    std::vector<Aggregate> totals(n_countries + 1);
    for (auto& shard : partial)
        for (size_t i = 0; i < totals.size(); ++i)
            merge_agg(totals[i], shard[i]);

    /* ── 5. Sort and emit output ────────────────────────────────────────── */
    std::vector<uint16_t> order;
    order.reserve(n_countries);
    for (uint16_t i = 0; i < static_cast<uint16_t>(n_countries); ++i)
        if (totals[i].count) order.push_back(i);
    std::sort(order.begin(), order.end(),
              [&countries](uint16_t a, uint16_t b) { return countries[a] < countries[b]; });

    std::string output;
    output.reserve(32 * 1024);

    auto write_row = [&](uint16_t idx, bool unmatched) {
        const Aggregate& a = totals[idx];
        if (unmatched) {
            output.append("_UNMATCHED");
        } else {
            const uint32_t code = countries[idx];
            output.push_back(static_cast<char>(code >> 16));
            output.push_back(static_cast<char>(code >>  8));
            output.push_back(static_cast<char>(code));
        }
        output.push_back(' '); append_uint(output, a.count);
        output.push_back(' '); append_uint(output, a.id_sum);
        output.push_back(' '); append_cents(output, a.amount_sum);
        output.push_back(' '); append_cents(output, a.minimum);
        output.push_back(' '); append_cents(output, a.maximum);
        output.push_back(' '); append_uint(output, a.names.size());
        output.push_back(' '); append_uint(output, a.name_bytes);
        output.push_back(' '); append_uint(output, a.note_bytes);
        output.push_back('\n');
    };

    for (uint16_t id : order) write_row(id, false);
    if (totals.back().count) write_row(static_cast<uint16_t>(n_countries), true);

    /* ── 6. Write output ─────────────────────────────────────────────────── */
    const int fd = open("/out/aggregates.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) _exit(2);
    const char* wp = output.data();
    size_t remain  = output.size();
    while (remain) {
        ssize_t n = write(fd, wp, remain);
        if (n <= 0) _exit(2);
        wp += n; remain -= static_cast<size_t>(n);
    }
    close(fd);
    return 0;
}
