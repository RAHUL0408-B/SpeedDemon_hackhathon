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
#include <vector>

struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY); struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
        size = static_cast<size_t>(st.st_size);
        if (size) {
            data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
            if (data == MAP_FAILED) _exit(2);
            madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        }
    }
    ~Mapping() { if (data && data != MAP_FAILED) munmap(const_cast<char*>(data), size); if (fd >= 0) close(fd); }
};

static inline void append_u64(std::string& out, uint64_t n) {
    char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n); out.append(buf, p);
}

static inline uint32_t hour_to_idx(const char* p) noexcept {
    const uint32_t month = (static_cast<uint32_t>(p[5] - '0') * 10u) + static_cast<uint32_t>(p[6] - '0');
    const uint32_t day = (static_cast<uint32_t>(p[8] - '0') * 10u) + static_cast<uint32_t>(p[9] - '0');
    const uint32_t hr = (static_cast<uint32_t>(p[11] - '0') * 10u) + static_cast<uint32_t>(p[12] - '0');
    const uint32_t day_off = (month == 10) ? (day - 30u) : (day + 1u);
    return day_off * 24u + hr - 18u;
}

static const char* const HOUR_STRINGS[72] = {
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

struct FastMap {
    static constexpr uint32_t CAP = 65536, MASK = CAP - 1;
    std::string_view keys[CAP];
    uint32_t vals[CAP];
    uint8_t occupied[CAP] = {};

    static inline uint64_t hash_sv(const char* p, size_t len) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i) { h ^= static_cast<unsigned char>(p[i]); h *= 1099511628211ULL; }
        return h;
    }

    inline int32_t get_or_insert(std::string_view sv, uint32_t new_id) noexcept {
        const uint64_t h = hash_sv(sv.data(), sv.size()); uint32_t idx = h & MASK;
        while (occupied[idx]) {
            if (keys[idx].size() == sv.size() && std::memcmp(keys[idx].data(), sv.data(), sv.size()) == 0) return static_cast<int32_t>(vals[idx]);
            idx = (idx + 1) & MASK;
        }
        keys[idx] = sv; vals[idx] = new_id; occupied[idx] = 1;
        return -1;
    }
};

struct EpInfo {
    std::string_view name;
    uint32_t count = 0;
    uint32_t errs = 0;
    uint32_t offset = 0;
};

int main() {
    Mapping input("/data/requests.log");
    const char *begin = input.data, *finish = input.data + input.size;
    constexpr unsigned W = 12;

    const char* chunk_starts[W + 1]; chunk_starts[0] = begin; chunk_starts[W] = finish;
    for (unsigned i = 1; i < W; ++i) {
        const char* p = begin + input.size * i / W;
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(finish - p)));
        chunk_starts[i] = nl ? nl + 1 : finish;
    }

    const char* thread_min[W][72] = {};
    const char* thread_max[W][72] = {};
    std::vector<std::thread> pool; pool.reserve(W);

    for (unsigned w = 0; w < W; ++w) {
        pool.emplace_back([&, w] {
            const char* p = chunk_starts[w], *lim = chunk_starts[w + 1];
            while (p < lim) {
                const char* line = p;
                const char* nl = static_cast<const char*>(std::memchr(line, '\n', static_cast<size_t>(lim - line)));
                if (!nl) nl = lim; p = nl < lim ? nl + 1 : lim;
                if (nl - line < 35) continue;
                uint32_t hid = hour_to_idx(line);
                if (hid < 72) {
                    if (!thread_min[w][hid]) thread_min[w][hid] = line;
                    thread_max[w][hid] = nl;
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    pool.clear();

    const char* global_first[72];
    const char* global_last[72];
    for (size_t h = 0; h < 72; ++h) {
        const char* f = finish; const char* l = begin;
        for (unsigned w = 0; w < W; ++w) {
            if (thread_min[w][h] && thread_min[w][h] < f) f = thread_min[w][h];
            if (thread_max[w][h] && thread_max[w][h] > l) l = thread_max[w][h];
        }
        global_first[h] = f; global_last[h] = l;
    }

    std::vector<std::string> worker_outputs(W);
    for (unsigned w = 0; w < W; ++w) {
        pool.emplace_back([&, w] {
            const uint32_t h_start = w * 6;
            const uint32_t h_end = h_start + 6;
            const char* my_start = (w == 0) ? begin : global_first[h_start];
            const char* my_end = (w == W - 1) ? finish : global_last[h_end - 1];

            std::vector<FastMap> maps(6);
            std::vector<std::vector<EpInfo>> hour_eps(6);
            uint32_t total_recs = 0;

            // Pass 1: count records per endpoint
            const char* p = my_start;
            while (p < my_end) {
                const char* line = p;
                const char* nl = static_cast<const char*>(std::memchr(line, '\n', static_cast<size_t>(my_end - line)));
                if (!nl) nl = my_end; p = nl < my_end ? nl + 1 : my_end;
                if (nl - line < 35) continue;

                uint32_t hid = hour_to_idx(line);
                if (hid < h_start || hid >= h_end) continue;
                uint32_t local_hid = hid - h_start;

                const char* q = line + 27;
                if (*q != ' ') { while (q < nl && *q != ' ') ++q; }
                while (q < nl && *q == ' ') ++q;
                while (q < nl && *q != ' ') ++q;
                while (q < nl && *q == ' ') ++q;
                const char* es = q;
                while (q < nl && *q != ' ') ++q;
                std::string_view ep_sv(es, static_cast<size_t>(q - es));

                while (q < nl && *q == ' ') ++q;
                const bool is_5xx = (*q == '5');

                auto& h_info = hour_eps[local_hid];
                auto& map = maps[local_hid];
                int32_t eid = map.get_or_insert(ep_sv, static_cast<uint32_t>(h_info.size()));
                if (eid < 0) {
                    eid = static_cast<int32_t>(h_info.size());
                    h_info.push_back({ep_sv, 1, is_5xx ? 1u : 0u, 0});
                } else {
                    h_info[eid].count++;
                    if (is_5xx) h_info[eid].errs++;
                }
                ++total_recs;
            }

            uint32_t cur_offset = 0;
            for (size_t hi = 0; hi < 6; ++hi) {
                for (auto& ep : hour_eps[hi]) {
                    ep.offset = cur_offset;
                    cur_offset += ep.count;
                }
            }

            std::vector<uint32_t> latencies(total_recs);
            std::vector<std::vector<uint32_t>> write_pos(6);
            for (size_t hi = 0; hi < 6; ++hi) {
                write_pos[hi].resize(hour_eps[hi].size());
                for (size_t ei = 0; ei < hour_eps[hi].size(); ++ei) write_pos[hi][ei] = hour_eps[hi][ei].offset;
            }

            // Pass 2: store latencies
            p = my_start;
            while (p < my_end) {
                const char* line = p;
                const char* nl = static_cast<const char*>(std::memchr(line, '\n', static_cast<size_t>(my_end - line)));
                if (!nl) nl = my_end; p = nl < my_end ? nl + 1 : my_end;
                if (nl - line < 35) continue;

                uint32_t hid = hour_to_idx(line);
                if (hid < h_start || hid >= h_end) continue;
                uint32_t local_hid = hid - h_start;

                const char* q = line + 27;
                if (*q != ' ') { while (q < nl && *q != ' ') ++q; }
                while (q < nl && *q == ' ') ++q;
                while (q < nl && *q != ' ') ++q;
                while (q < nl && *q == ' ') ++q;
                const char* es = q;
                while (q < nl && *q != ' ') ++q;
                std::string_view ep_sv(es, static_cast<size_t>(q - es));

                while (q < nl && *q == ' ') ++q;
                while (q < nl && *q != ' ') ++q;
                while (q < nl && *q == ' ') ++q;
                uint32_t lat = 0;
                while (q < nl && *q >= '0' && *q <= '9') lat = lat * 10u + static_cast<uint32_t>(*q++ - '0');

                int32_t eid = maps[local_hid].get_or_insert(ep_sv, 0);
                latencies[write_pos[local_hid][eid]++] = lat;
            }

            // Output formatting
            std::string& out = worker_outputs[w];
            out.reserve(6 * 2048 * 64);

            for (size_t hi = 0; hi < 6; ++hi) {
                const char* hstr = HOUR_STRINGS[h_start + hi];
                auto& eps = hour_eps[hi];
                std::vector<uint32_t> order(eps.size());
                for (size_t i = 0; i < eps.size(); ++i) order[i] = static_cast<uint32_t>(i);
                std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                    return eps[a].name < eps[b].name;
                });

                for (uint32_t ei : order) {
                    const auto& ep = eps[ei];
                    const uint32_t count = ep.count;
                    uint32_t* base = &latencies[ep.offset];

                    if (count > 1) {
                        if (count == 2) { if (base[1] < base[0]) std::swap(base[0], base[1]); }
                        else if (count <= 16) { std::sort(base, base + count); }
                        else {
                            const uint32_t r50 = (50u * count + 99u) / 100u - 1u;
                            const uint32_t r95 = (95u * count + 99u) / 100u - 1u;
                            const uint32_t r99 = (99u * count + 99u) / 100u - 1u;
                            std::nth_element(base, base + r50, base + count);
                            std::nth_element(base + r50 + 1, base + r95, base + count);
                            std::nth_element(base + r95 + 1, base + r99, base + count);
                        }
                    }

                    out.append(hstr); out.append(":00:00Z "); out.append(ep.name); out.push_back(' ');
                    append_u64(out, count); out.push_back(' ');
                    append_u64(out, base[(50u * count + 99u) / 100u - 1u]); out.push_back(' ');
                    append_u64(out, base[(95u * count + 99u) / 100u - 1u]); out.push_back(' ');
                    append_u64(out, base[(99u * count + 99u) / 100u - 1u]); out.push_back(' ');
                    append_u64(out, ep.errs); out.push_back(' ');
                    append_u64(out, static_cast<uint64_t>(ep.errs) * 1000000ULL / count); out.push_back('\n');
                }
            }
        });
    }
    for (auto& t : pool) t.join();
    pool.clear();

    const int out_fd = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) {
        for (unsigned w = 0; w < W; ++w) {
            const char* wp = worker_outputs[w].data(); size_t rem = worker_outputs[w].size();
            while (rem) { ssize_t s = write(out_fd, wp, rem); if (s <= 0) break; wp += s; rem -= static_cast<size_t>(s); }
        }
        close(out_fd);
    }
    return 0;
}
