#pragma GCC optimize("O3")
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

constexpr unsigned WORKERS = 12, HOURS = 72, SLOTS = 4096;
constexpr size_t SAFE_TAIL = 256;  // a line is at most 177 bytes and the vector scans read up to 31 past it

static const char* const HOUR_STRINGS[HOURS] = {
    "2026-10-30T18","2026-10-30T19","2026-10-30T20","2026-10-30T21","2026-10-30T22","2026-10-30T23",
    "2026-10-31T00","2026-10-31T01","2026-10-31T02","2026-10-31T03","2026-10-31T04","2026-10-31T05",
    "2026-10-31T06","2026-10-31T07","2026-10-31T08","2026-10-31T09","2026-10-31T10","2026-10-31T11",
    "2026-10-31T12","2026-10-31T13","2026-10-31T14","2026-10-31T15","2026-10-31T16","2026-10-31T17",
    "2026-10-31T18","2026-10-31T19","2026-10-31T20","2026-10-31T21","2026-10-31T22","2026-10-31T23",
    "2026-11-01T00","2026-11-01T01","2026-11-01T02","2026-11-01T03","2026-11-01T04","2026-11-01T05",
    "2026-11-01T06","2026-11-01T07","2026-11-01T08","2026-11-01T09","2026-11-01T10","2026-11-01T11",
    "2026-11-01T12","2026-11-01T13","2026-11-01T14","2026-11-01T15","2026-11-01T16","2026-11-01T17",
    "2026-11-01T18","2026-11-01T19","2026-11-01T20","2026-11-01T21","2026-11-01T22","2026-11-01T23",
    "2026-11-02T00","2026-11-02T01","2026-11-02T02","2026-11-02T03","2026-11-02T04","2026-11-02T05",
    "2026-11-02T06","2026-11-02T07","2026-11-02T08","2026-11-02T09","2026-11-02T10","2026-11-02T11",
    "2026-11-02T12","2026-11-02T13","2026-11-02T14","2026-11-02T15","2026-11-02T16","2026-11-02T17"};

// Hour of the 72-hour span (0 = 2026-10-30T18) from the fixed-width timestamp.
static inline uint32_t hour_index(const char* p) noexcept {
    const uint32_t month = static_cast<uint32_t>(p[5] - '0') * 10u + static_cast<uint32_t>(p[6] - '0');
    const uint32_t day = static_cast<uint32_t>(p[8] - '0') * 10u + static_cast<uint32_t>(p[9] - '0');
    const uint32_t hour = static_cast<uint32_t>(p[11] - '0') * 10u + static_cast<uint32_t>(p[12] - '0');
    return ((month == 10 ? day - 30u : day + 1u) * 24u) + hour - 18u;
}

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

static inline const char* find_space(const char* p) noexcept {
    const __m256i space = _mm256_set1_epi8(' ');
    for (;; p += 32) {
        const uint32_t m = static_cast<uint32_t>(_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)), space)));
        if (m) return p + __builtin_ctz(m);
    }
}

static inline void append_u64(std::string& out, uint64_t n) {
    char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n); out.append(buf, p);
}

struct Rec { uint32_t key, lat; };  // key: the shard's endpoint id, top bit set for a 5xx status

// An append-only list of records in fixed blocks: no reallocation, no copying, and at most one part-filled block.
struct Bucket {
    static constexpr size_t BLOCK = 1024;
    std::vector<Rec*> blocks;
    Rec* next = nullptr;
    Rec* limit = nullptr;
    void push(Rec r) {
        if (next == limit) {
            next = static_cast<Rec*>(std::malloc(BLOCK * sizeof(Rec)));
            blocks.push_back(next);
            limit = next + BLOCK;
        }
        *next++ = r;
    }
    size_t size() const { return blocks.size() * BLOCK - static_cast<size_t>(limit - next); }
    template <class F> void each(F&& f) const {
        for (size_t b = 0; b < blocks.size(); ++b) {
            const size_t n = b + 1 == blocks.size() ? BLOCK - static_cast<size_t>(limit - next) : BLOCK;
            for (size_t i = 0; i < n; ++i) f(blocks[b][i]);
        }
    }
};

struct Slot { uint64_t hash = 0; const char* name = nullptr; uint32_t len = 0, id = 0; };

// One worker's share of the file. Lines are bucketed by hour, whatever order they arrive in.
struct Shard {
    const char* begin; const char* end; const char* file_end;
    std::vector<Slot> table = std::vector<Slot>(SLOTS);
    std::vector<std::string_view> names;
    Bucket hours[HOURS];
    std::vector<char> tail;  // zero-padded copy of the last lines, so the vector reads stay inside memory
};

// Reads the decimal latency at p and returns the start of the next line. Up to eight digits are converted at
// once: XOR with '0' turns digits into 0..9 and the newline into a value above 9, which marks the end.
static inline const char* parse_latency(const char* p, uint32_t& value) noexcept {
    uint64_t w; std::memcpy(&w, p, 8);
    w ^= 0x3030303030303030ULL;
    const uint64_t stop = ((w + 0x7676767676767676ULL) | w) & 0x8080808080808080ULL;
    if (!stop) {  // nine or ten digits: rare
        uint64_t v = 0;
        for (; *p != '\n'; ++p) v = v * 10u + static_cast<uint64_t>(*p - '0');
        value = static_cast<uint32_t>(v);
        return p + 1;
    }
    const unsigned digits = static_cast<unsigned>(__builtin_ctzll(stop)) >> 3;
    w <<= (8 - digits) * 8;  // right-align the digits; the empty high-order places become zeros
    w = ((w & 0x0F0F0F0F0F0F0F0FULL) * 2561) >> 8;
    w = ((w & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;
    w = ((w & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32;
    value = static_cast<uint32_t>(w);
    return p + digits + 1;
}

static const char* scan_lines(Shard& s, const char* p, const char* end) {
    while (p < end) {
        const uint32_t hour = hour_index(p);
        const char* ep = find_space(p + 28) + 1;
        const char* sp = find_space(ep);
        const size_t len = static_cast<size_t>(sp - ep);
        const uint64_t h = hash_bytes(ep, len);
        uint32_t idx = static_cast<uint32_t>(h) & (SLOTS - 1), id;
        for (;;) {
            Slot& slot = s.table[idx];
            if (!slot.name) {
                id = static_cast<uint32_t>(s.names.size());
                slot = {h, ep, static_cast<uint32_t>(len), id};
                s.names.emplace_back(ep, len);
                break;
            }
            // 64 bits of hash plus the length identify an endpoint: at most a few thousand are in play.
            if (slot.hash == h && slot.len == len) { id = slot.id; break; }
            idx = (idx + 1) & (SLOTS - 1);
        }
        uint32_t latency;
        p = parse_latency(sp + 5, latency);
        s.hours[hour].push({id | (static_cast<uint32_t>(sp[1] == '5') << 31), latency});
    }
    return p;
}

static void* scan_worker(void* arg) {
    Shard& s = *static_cast<Shard*>(arg);
    const size_t room = static_cast<size_t>(s.file_end - s.begin);
    const char* stop = room > SAFE_TAIL ? std::min(s.end, s.file_end - SAFE_TAIL) : s.begin;
    const char* p = scan_lines(s, s.begin, stop);
    if (p < s.end) {
        s.tail.assign(static_cast<size_t>(s.end - p) + SAFE_TAIL, 0);
        std::memcpy(s.tail.data(), p, static_cast<size_t>(s.end - p));
        scan_lines(s, s.tail.data(), s.tail.data() + (s.end - p));
    }
    return nullptr;
}

struct Merge {
    std::vector<Shard>* shards;
    std::vector<std::vector<uint32_t>> remap;  // shard endpoint id -> global endpoint id
    std::vector<std::string_view> names;       // global endpoint id -> endpoint
    std::vector<uint32_t> order;               // global ids in bytewise endpoint order
    std::string out[HOURS];
    std::atomic<unsigned> next{0};
};

static void emit_hour(Merge& m, unsigned hour, std::vector<uint32_t>& counts, std::vector<uint32_t>& errors,
                      std::vector<uint32_t>& offsets, std::vector<uint32_t>& latencies) {
    const size_t endpoints = m.names.size();
    counts.assign(endpoints, 0); errors.assign(endpoints, 0);
    size_t total = 0;
    for (size_t w = 0; w < m.shards->size(); ++w) {
        const uint32_t* remap = m.remap[w].data();
        (*m.shards)[w].hours[hour].each([&](const Rec& r) {
            const uint32_t g = remap[r.key & 0x7fffffffu];
            ++counts[g]; errors[g] += r.key >> 31;
        });
        total += (*m.shards)[w].hours[hour].size();
    }
    if (!total) return;
    offsets.assign(endpoints + 1, 0);
    for (size_t g = 0; g < endpoints; ++g) offsets[g + 1] = offsets[g] + counts[g];
    latencies.resize(total);
    std::vector<uint32_t> fill(offsets.begin(), offsets.end() - 1);
    for (size_t w = 0; w < m.shards->size(); ++w) {
        const uint32_t* remap = m.remap[w].data();
        (*m.shards)[w].hours[hour].each([&](const Rec& r) { latencies[fill[remap[r.key & 0x7fffffffu]]++] = r.lat; });
    }

    std::string& out = m.out[hour];
    out.reserve(endpoints * 64);
    for (uint32_t g : m.order) {
        const uint32_t count = counts[g];
        if (!count) continue;
        uint32_t* base = &latencies[offsets[g]];
        const uint32_t r50 = (50u * count + 99u) / 100u - 1u;
        const uint32_t r95 = (95u * count + 99u) / 100u - 1u;
        const uint32_t r99 = (99u * count + 99u) / 100u - 1u;
        if (count == 2) { if (base[1] < base[0]) std::swap(base[0], base[1]); }
        else if (count <= 96) std::sort(base, base + count);
        else {
            std::nth_element(base, base + r50, base + count);
            std::nth_element(base + r50 + 1, base + r95, base + count);
            std::nth_element(base + r95 + 1, base + r99, base + count);
        }
        out.append(HOUR_STRINGS[hour]); out.append(":00:00Z "); out.append(m.names[g]); out.push_back(' ');
        append_u64(out, count); out.push_back(' ');
        append_u64(out, base[r50]); out.push_back(' ');
        append_u64(out, base[r95]); out.push_back(' ');
        append_u64(out, base[r99]); out.push_back(' ');
        append_u64(out, errors[g]); out.push_back(' ');
        append_u64(out, static_cast<uint64_t>(errors[g]) * 1000000ULL / count); out.push_back('\n');
    }
}

static void* merge_worker(void* arg) {
    Merge& m = *static_cast<Merge*>(arg);
    std::vector<uint32_t> counts, errors, offsets, latencies;
    for (unsigned hour; (hour = m.next.fetch_add(1)) < HOURS;) emit_hour(m, hour, counts, errors, offsets, latencies);
    return nullptr;
}

template <class Fn> static void run_workers(Fn fn, void* const* args) {
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 524288);  // the default 8 MB of address space per thread is not needed
    pthread_t threads[WORKERS];
    for (unsigned w = 0; w < WORKERS; ++w) pthread_create(&threads[w], &attr, fn, args[w]);
    pthread_attr_destroy(&attr);
    for (unsigned w = 0; w < WORKERS; ++w) pthread_join(threads[w], nullptr);
}

int main() {
    const int fd = open("/data/requests.log", O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
    const size_t size = static_cast<size_t>(st.st_size);
    const int out_fd = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) _exit(3);
    if (!size) return 0;
    const char* data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
    if (data == MAP_FAILED) _exit(2);

    // Equal byte ranges moved forward to the next line start. Nothing here assumes the file is in time order.
    std::vector<Shard> shards(WORKERS);
    const char* cut[WORKERS + 1];
    cut[0] = data; cut[WORKERS] = data + size;
    for (unsigned w = 1; w < WORKERS; ++w) {
        const char* from = data + size * w / WORKERS - 1;
        const char* nl = static_cast<const char*>(std::memchr(from, '\n', static_cast<size_t>(data + size - from)));
        cut[w] = nl ? nl + 1 : data + size;
    }
    void* args[WORKERS];
    for (unsigned w = 0; w < WORKERS; ++w) {
        shards[w].begin = cut[w]; shards[w].end = cut[w + 1]; shards[w].file_end = data + size;
        args[w] = &shards[w];
    }
    run_workers(scan_worker, args);

    Merge m; m.shards = &shards;
    std::vector<Slot> global(SLOTS * 2);
    m.remap.resize(WORKERS);
    for (unsigned w = 0; w < WORKERS; ++w) {
        for (std::string_view name : shards[w].names) {
            const uint64_t h = hash_bytes(name.data(), name.size());
            uint32_t idx = static_cast<uint32_t>(h) & (SLOTS * 2 - 1), id;
            for (;;) {
                Slot& slot = global[idx];
                if (!slot.name) {
                    id = static_cast<uint32_t>(m.names.size());
                    slot = {h, name.data(), static_cast<uint32_t>(name.size()), id};
                    m.names.push_back(name);
                    break;
                }
                if (slot.hash == h && slot.len == name.size() && std::memcmp(slot.name, name.data(), name.size()) == 0) { id = slot.id; break; }
                idx = (idx + 1) & (SLOTS * 2 - 1);
            }
            m.remap[w].push_back(id);
        }
    }
    m.order.resize(m.names.size());
    for (uint32_t g = 0; g < m.order.size(); ++g) m.order[g] = g;
    std::sort(m.order.begin(), m.order.end(), [&](uint32_t a, uint32_t b) { return m.names[a] < m.names[b]; });

    void* margs[WORKERS];
    for (unsigned w = 0; w < WORKERS; ++w) margs[w] = &m;
    run_workers(merge_worker, margs);

    for (const std::string& hour_out : m.out) {
        const char* wp = hour_out.data(); size_t rem = hour_out.size();
        while (rem) { ssize_t s = write(out_fd, wp, rem); if (s <= 0) _exit(3); wp += s; rem -= static_cast<size_t>(s); }
    }
    close(out_fd);
    return 0;
}
