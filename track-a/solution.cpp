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
    static constexpr uint32_t CAP = 4096, MASK = CAP - 1;
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

struct Rec {
    uint32_t gid;
    uint32_t lat;
};

// Binary search for first line whose hour index >= target_hid
static const char* find_hour_binary(const char* begin, const char* finish, uint32_t target_hid) noexcept {
    const char* low = begin;
    const char* high = finish - 1;
    const char* best = finish;
    while (low <= high) {
        const char* mid = low + (high - low) / 2;
        const char* nl = static_cast<const char*>(std::memchr(mid, '\n', static_cast<size_t>(finish - mid)));
        if (!nl || nl + 14 >= finish) {
            high = mid - 1;
            continue;
        }
        const char* line = nl + 1;
        uint32_t hid = hour_to_idx(line);
        if (hid >= target_hid) {
            best = line;
            high = mid - 1;
        } else {
            low = nl + 1;
        }
    }
    return best;
}

int main() {
    Mapping input("/data/requests.log");
    const char *begin = input.data, *finish = input.data + input.size;
    constexpr unsigned W = 12;

    const char* worker_start[W];
    const char* worker_end[W];

    worker_start[0] = begin;
    worker_end[W - 1] = finish;

    // For each internal boundary w in 1..W-1, target hour is w * 6
    for (unsigned w = 1; w < W; ++w) {
        const uint32_t target_h = w * 6;
        const char* probe = find_hour_binary(begin, finish, target_h);
        // Margin: 3.5 MB before and after probe to guarantee capturing 100% of jittered lines
        constexpr size_t MARGIN = 3500000;
        const char* raw_start = (probe - begin > static_cast<ptrdiff_t>(MARGIN)) ? probe - MARGIN : begin;
        const char* raw_end = (finish - probe > static_cast<ptrdiff_t>(MARGIN)) ? probe + MARGIN : finish;

        // Align raw_start to next line
        if (raw_start > begin) {
            const char* nl = static_cast<const char*>(std::memchr(raw_start, '\n', static_cast<size_t>(finish - raw_start)));
            worker_start[w] = nl ? nl + 1 : finish;
        } else {
            worker_start[w] = begin;
        }

        // Align raw_end to next line
        if (raw_end < finish) {
            const char* nl = static_cast<const char*>(std::memchr(raw_end, '\n', static_cast<size_t>(finish - raw_end)));
            worker_end[w - 1] = nl ? nl + 1 : finish;
        } else {
            worker_end[w - 1] = finish;
        }
    }

    std::vector<std::string> worker_outputs(W);

    struct ThreadArg {
        unsigned w;
        uint32_t h_start;
        uint32_t h_end;
        const char* my_start;
        const char* my_end;
        std::string* out;
    };

    auto thread_worker = [](void* arg_ptr) -> void* {
        ThreadArg* arg = static_cast<ThreadArg*>(arg_ptr);
        const uint32_t h_start = arg->h_start;
        const uint32_t h_end = arg->h_end;
        const char* my_start = arg->my_start;
        const char* my_end = arg->my_end;
        std::string& out = *(arg->out);

        FastMap map;
        std::vector<std::string_view> ep_names; ep_names.reserve(2048);
        constexpr size_t TOTAL_GROUPS = 6 * 2048;
        std::vector<uint32_t> group_counts(TOTAL_GROUPS, 0);
        std::vector<uint32_t> group_errs(TOTAL_GROUPS, 0);
        std::vector<Rec> recs; recs.reserve(250000);

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
            while (q < nl && *q != ' ') ++q;
            while (q < nl && *q == ' ') ++q;
            uint32_t lat = 0;
            while (q < nl && *q >= '0' && *q <= '9') lat = lat * 10u + static_cast<uint32_t>(*q++ - '0');

            int32_t eid = map.get_or_insert(ep_sv, static_cast<uint32_t>(ep_names.size()));
            if (eid < 0) {
                eid = static_cast<int32_t>(ep_names.size());
                ep_names.push_back(ep_sv);
            }
            const uint32_t gid = local_hid * 2048u + static_cast<uint32_t>(eid);
            group_counts[gid]++;
            if (is_5xx) group_errs[gid]++;
            recs.push_back({gid, lat});
        }

        std::vector<uint32_t> group_offsets(TOTAL_GROUPS + 1, 0);
        for (size_t g = 0; g < TOTAL_GROUPS; ++g) group_offsets[g + 1] = group_offsets[g] + group_counts[g];
        std::vector<uint32_t> write_pos = group_offsets;
        std::vector<uint32_t> latencies(group_offsets.back());

        for (const auto& r : recs) latencies[write_pos[r.gid]++] = r.lat;
        recs.clear(); recs.shrink_to_fit();

        out.reserve(6 * 1850 * 55);

        for (uint32_t local_hid = 0; local_hid < 6; ++local_hid) {
            const char* hstr = HOUR_STRINGS[h_start + local_hid];
            const uint32_t g_base = local_hid * 2048u;
            std::vector<uint32_t> active_eids; active_eids.reserve(ep_names.size());
            for (uint32_t eid = 0; eid < ep_names.size(); ++eid) {
                const uint32_t gid = g_base + eid;
                if (group_counts[gid] > 0) active_eids.push_back(eid);
            }
            std::sort(active_eids.begin(), active_eids.end(), [&](uint32_t a, uint32_t b) {
                return ep_names[a] < ep_names[b];
            });

            for (uint32_t eid : active_eids) {
                const uint32_t gid = g_base + eid;
                const uint32_t count = group_counts[gid];
                uint32_t* base = &latencies[group_offsets[gid]];

                if (count > 1) {
                    if (count == 2) { if (base[1] < base[0]) std::swap(base[0], base[1]); }
                    else if (count <= 128) { std::sort(base, base + count); }
                    else {
                        const uint32_t r50 = (50u * count + 99u) / 100u - 1u;
                        const uint32_t r95 = (95u * count + 99u) / 100u - 1u;
                        const uint32_t r99 = (99u * count + 99u) / 100u - 1u;
                        std::nth_element(base, base + r50, base + count);
                        std::nth_element(base + r50 + 1, base + r95, base + count);
                        std::nth_element(base + r95 + 1, base + r99, base + count);
                    }
                }

                out.append(hstr); out.append(":00:00Z "); out.append(ep_names[eid]); out.push_back(' ');
                append_u64(out, count); out.push_back(' ');
                append_u64(out, base[(50u * count + 99u) / 100u - 1u]); out.push_back(' ');
                append_u64(out, base[(95u * count + 99u) / 100u - 1u]); out.push_back(' ');
                append_u64(out, base[(99u * count + 99u) / 100u - 1u]); out.push_back(' ');
                const uint32_t errs = group_errs[gid];
                append_u64(out, errs); out.push_back(' ');
                append_u64(out, static_cast<uint64_t>(errs) * 1000000ULL / count); out.push_back('\n');
            }
        }
        return nullptr;
    };

    pthread_t threads[W];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Explicit 512 KB stack size to avoid default 8 MB virtual memory per thread
    pthread_attr_setstacksize(&attr, 524288);

    ThreadArg args[W];
    for (unsigned w = 0; w < W; ++w) {
        args[w] = {w, w * 6, (w + 1) * 6, worker_start[w], worker_end[w], &worker_outputs[w]};
        pthread_create(&threads[w], &attr, thread_worker, &args[w]);
    }
    pthread_attr_destroy(&attr);

    for (unsigned w = 0; w < W; ++w) {
        pthread_join(threads[w], nullptr);
    }

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
