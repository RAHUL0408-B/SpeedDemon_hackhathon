#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY); struct stat st {};
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

static inline uint32_t fast_u32(const char*& p, const char* end) noexcept {
    while (p < end && static_cast<unsigned char>(*p) <= ' ') ++p;
    uint32_t n = 0;
    while (p < end && *p >= '0' && *p <= '9') n = n * 10u + static_cast<uint32_t>(*p++ - '0');
    return n;
}

struct RadixHeap {
    struct Item { uint64_t key; uint32_t val; };
    std::array<std::vector<Item>, 65> b;
    uint64_t last = 0; size_t count = 0;
    static inline unsigned buck(uint64_t k, uint64_t lk) noexcept {
        return k == lk ? 0 : 64u - static_cast<unsigned>(__builtin_clzll(k ^ lk));
    }
    void clear() noexcept { for (auto& v : b) v.clear(); last = 0; count = 0; }
    void push(uint64_t k, uint32_t v) { b[buck(k, last)].push_back({k, v}); ++count; }
    Item pop() {
        if (b[0].empty()) {
            unsigned i = 1; while (b[i].empty()) ++i;
            uint64_t next = std::numeric_limits<uint64_t>::max();
            for (const Item& x : b[i]) if (x.key < next) next = x.key;
            last = next;
            std::vector<Item> move; move.swap(b[i]);
            for (const Item& x : move) b[buck(x.key, last)].push_back(x);
        }
        Item res = b[0].back(); b[0].pop_back(); --count; return res;
    }
};

struct Edge { uint32_t v; uint32_t w; };
struct Worker {
    const std::vector<uint32_t>& offsets;
    const std::vector<Edge>& edges;
    std::vector<uint64_t> dist;
    std::vector<uint32_t> seen;
    uint32_t stamp = 1;
    RadixHeap heap;
    Worker(const std::vector<uint32_t>& off, const std::vector<Edge>& e)
        : offsets(off), edges(e), dist(off.size() - 1), seen(off.size() - 1, 0) {}
    int64_t shortest(uint32_t s, uint32_t t) {
        if (s == t) return 0;
        if (++stamp == 0) { std::fill(seen.begin(), seen.end(), 0); stamp = 1; }
        heap.clear();
        seen[s] = stamp; dist[s] = 0; heap.push(0, s);
        while (heap.count) {
            const auto cur = heap.pop();
            if (seen[cur.val] != stamp || dist[cur.val] != cur.key) continue;
            if (cur.val == t) return static_cast<int64_t>(cur.key);
            for (uint32_t e = offsets[cur.val]; e < offsets[cur.val + 1]; ++e) {
                const Edge edge = edges[e];
                const uint64_t nd = cur.key + edge.w;
                if (seen[edge.v] != stamp || nd < dist[edge.v]) {
                    seen[edge.v] = stamp; dist[edge.v] = nd; heap.push(nd, edge.v);
                }
            }
        }
        return -1;
    }
};

int main() {
    Mapping gf("/data/graph.txt");
    const char* gp = gf.data; const char* gend = gp + gf.size;
    const uint32_t n = fast_u32(gp, gend), m = fast_u32(gp, gend);
    while (gp < gend && *gp++ != '\n');
    const char* es = gp; const size_t eb = static_cast<size_t>(gend - es);

    constexpr unsigned W = 6;
    const char* st[W + 1]; st[0] = es; st[W] = gend;
    for (unsigned w = 1; w < W; ++w) {
        const char* p = es + (eb * w / W);
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(gend - p)));
        st[w] = nl ? nl + 1 : gend;
    }

    struct Raw { uint32_t u, v, w; };
    std::vector<std::vector<Raw>> le(W);
    std::vector<std::vector<uint32_t>> ld(W, std::vector<uint32_t>(n, 0));
    std::vector<std::thread> pool; pool.reserve(W);
    for (unsigned w = 0; w < W; ++w) {
        pool.emplace_back([&, w] {
            const char* p = st[w]; const char* lim = st[w + 1];
            auto& e = le[w]; e.reserve(m / W + 5000); auto& d = ld[w];
            while (p < lim) {
                uint32_t u = fast_u32(p, lim); if (p >= lim) break;
                uint32_t v = fast_u32(p, lim), wt = fast_u32(p, lim);
                e.push_back({u, v, wt}); ++d[u];
            }
        });
    }
    for (auto& t : pool) t.join();
    pool.clear();

    std::vector<uint32_t> off(static_cast<size_t>(n) + 1, 0);
    for (uint32_t u = 0; u < n; ++u) {
        uint32_t d = 0; for (unsigned w = 0; w < W; ++w) d += ld[w][u];
        off[u + 1] = off[u] + d;
    }
    std::vector<std::vector<uint32_t>> ws(W, std::vector<uint32_t>(n));
    for (uint32_t u = 0; u < n; ++u) {
        uint32_t cur = off[u];
        for (unsigned w = 0; w < W; ++w) { ws[w][u] = cur; cur += ld[w][u]; }
    }
    std::vector<Edge> edges(m);
    for (unsigned w = 0; w < W; ++w) {
        pool.emplace_back([&, w] {
            auto& s = ws[w];
            for (const auto& e : le[w]) edges[s[e.u]++] = {e.v, e.w};
            le[w].clear(); le[w].shrink_to_fit(); ld[w].clear(); ld[w].shrink_to_fit();
        });
    }
    for (auto& t : pool) t.join();
    pool.clear();
    ws.clear(); ws.shrink_to_fit();

    Mapping qf("/data/queries.txt");
    const char* qp = qf.data; const char* qend = qp + qf.size;
    std::vector<std::pair<uint32_t, uint32_t>> q; q.reserve(2500);
    while (qp < qend) {
        uint32_t s = fast_u32(qp, qend); if (qp >= qend) break;
        q.push_back({s, fast_u32(qp, qend)});
    }
    std::unordered_map<uint64_t, uint32_t> uidx; uidx.reserve(q.size() * 2);
    std::vector<std::pair<uint32_t, uint32_t>> uq;
    std::vector<uint32_t> rem(q.size());
    for (size_t i = 0; i < q.size(); ++i) {
        const uint64_t k = (static_cast<uint64_t>(q[i].first) << 32) | q[i].second;
        auto [it, ins] = uidx.emplace(k, static_cast<uint32_t>(uq.size()));
        if (ins) uq.push_back(q[i]);
        rem[i] = it->second;
    }

    std::vector<int64_t> uans(uq.size()), ans(q.size());
    std::atomic<size_t> next{0};
    for (unsigned t = 0; t < W; ++t) {
        pool.emplace_back([&] {
            Worker worker(off, edges);
            for (;;) {
                const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= uq.size()) break;
                uans[i] = worker.shortest(uq[i].first, uq[i].second);
            }
        });
    }
    for (auto& t : pool) t.join();
    for (size_t i = 0; i < ans.size(); ++i) ans[i] = uans[rem[i]];

    std::string out; out.reserve(ans.size() * 12);
    for (int64_t a : ans) {
        char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), a);
        out.append(buf, p); out.push_back('\n');
    }
    const int fd = open("/out/distances.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, out.data(), out.size()); close(fd); }
    return 0;
}
