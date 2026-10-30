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
            data = static_cast<const char*>(
                mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
            if (data == MAP_FAILED) _exit(2);
            madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        }
    }
    ~Mapping() { if (data && data != MAP_FAILED) munmap(const_cast<char*>(data), size); if (fd >= 0) close(fd); }
};

static inline uint32_t fast_parse_uint(const char*& p, const char* end) noexcept {
    while (p < end && static_cast<unsigned char>(*p) <= ' ') ++p;
    uint32_t n = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        n = n * 10u + static_cast<uint32_t>(*p++ - '0');
    }
    return n;
}

struct RadixHeap {
    struct Item { uint64_t key; uint32_t value; };
    std::array<std::vector<Item>, 65> buckets;
    uint64_t last = 0; size_t count = 0;
    static inline unsigned bucket(uint64_t key, uint64_t last_key) noexcept {
        if (key == last_key) return 0;
        return 64u - static_cast<unsigned>(__builtin_clzll(key ^ last_key));
    }
    void clear() noexcept { for (auto& b : buckets) b.clear(); last = 0; count = 0; }
    void push(uint64_t key, uint32_t value) { buckets[bucket(key, last)].push_back({key, value}); ++count; }
    bool empty() const noexcept { return count == 0; }
    Item pop() {
        if (buckets[0].empty()) {
            unsigned i = 1; while (buckets[i].empty()) ++i;
            uint64_t next = std::numeric_limits<uint64_t>::max();
            for (const Item& x : buckets[i]) if (x.key < next) next = x.key;
            last = next;
            std::vector<Item> move; move.swap(buckets[i]);
            for (const Item& x : move) buckets[bucket(x.key, last)].push_back(x);
        }
        Item answer = buckets[0].back(); buckets[0].pop_back(); --count; return answer;
    }
};

struct Edge {
    uint32_t v;
    uint32_t w;
};

struct Worker {
    const std::vector<uint32_t>& offsets;
    const std::vector<Edge>& edges;
    std::vector<uint64_t> distance;
    std::vector<uint32_t> seen;
    uint32_t stamp = 1;
    RadixHeap heap;
    Worker(const std::vector<uint32_t>& off, const std::vector<Edge>& e)
        : offsets(off), edges(e), distance(off.size() - 1), seen(off.size() - 1, 0) {}
    int64_t shortest(uint32_t source, uint32_t target) {
        if (source == target) return 0;
        if (++stamp == 0) { std::fill(seen.begin(), seen.end(), 0); stamp = 1; }
        heap.clear();
        seen[source] = stamp; distance[source] = 0; heap.push(0, source);
        while (!heap.empty()) {
            const auto cur = heap.pop();
            if (seen[cur.value] != stamp || distance[cur.value] != cur.key) continue;
            if (cur.value == target) return static_cast<int64_t>(cur.key);
            for (uint32_t e = offsets[cur.value]; e < offsets[cur.value + 1]; ++e) {
                const Edge edge = edges[e];
                const uint64_t nd = cur.key + edge.w;
                if (seen[edge.v] != stamp || nd < distance[edge.v]) {
                    seen[edge.v] = stamp; distance[edge.v] = nd; heap.push(nd, edge.v);
                }
            }
        }
        return -1;
    }
};

int main() {
    Mapping graph_file("/data/graph.txt");
    const char* gp = graph_file.data;
    const char* gend = gp + graph_file.size;

    const uint32_t n = fast_parse_uint(gp, gend);
    const uint32_t m = fast_parse_uint(gp, gend);
    while (gp < gend && *gp != '\n') ++gp;
    if (gp < gend) ++gp;
    const char* edges_start = gp;
    const size_t edges_bytes = static_cast<size_t>(gend - edges_start);

    constexpr unsigned WORKERS = 12;
    const char* starts[WORKERS + 1];
    starts[0] = edges_start;
    starts[WORKERS] = gend;
    for (unsigned w = 1; w < WORKERS; ++w) {
        const char* probe = edges_start + (edges_bytes * w / WORKERS);
        const char* nl = static_cast<const char*>(std::memchr(probe, '\n', static_cast<size_t>(gend - probe)));
        starts[w] = nl ? nl + 1 : gend;
    }

    struct RawEdge { uint32_t u, v, w; };
    std::vector<std::vector<RawEdge>> local_edges(WORKERS);
    std::vector<std::vector<uint32_t>> local_deg(WORKERS, std::vector<uint32_t>(n, 0));

    auto parse_chunk = [&](unsigned w) {
        const char* p = starts[w];
        const char* limit = starts[w + 1];
        auto& edges = local_edges[w];
        edges.reserve((m / WORKERS) + 5000);
        auto& deg = local_deg[w];

        while (p < limit) {
            uint32_t u = fast_parse_uint(p, limit);
            if (p >= limit) break;
            uint32_t v = fast_parse_uint(p, limit);
            uint32_t weight = fast_parse_uint(p, limit);
            edges.push_back({u, v, weight});
            deg[u]++;
        }
    };
    {
        std::vector<std::thread> pool; pool.reserve(WORKERS);
        for (unsigned w = 0; w < WORKERS; ++w) pool.emplace_back(parse_chunk, w);
        for (auto& t : pool) t.join();
    }

    std::vector<uint32_t> offsets(static_cast<size_t>(n) + 1, 0);
    for (uint32_t u = 0; u < n; ++u) {
        uint32_t d = 0;
        for (unsigned w = 0; w < WORKERS; ++w) d += local_deg[w][u];
        offsets[u + 1] = offsets[u] + d;
    }

    std::vector<std::vector<uint32_t>> worker_slot(WORKERS, std::vector<uint32_t>(n));
    for (uint32_t u = 0; u < n; ++u) {
        uint32_t cur = offsets[u];
        for (unsigned w = 0; w < WORKERS; ++w) {
            worker_slot[w][u] = cur;
            cur += local_deg[w][u];
        }
    }

    std::vector<Edge> edges(m);
    auto fill_csr = [&](unsigned w) {
        auto& slot = worker_slot[w];
        const auto& raw = local_edges[w];
        for (const auto& e : raw) {
            const uint32_t idx = slot[e.u]++;
            edges[idx] = {e.v, e.w};
        }
    };
    {
        std::vector<std::thread> pool; pool.reserve(WORKERS);
        for (unsigned w = 0; w < WORKERS; ++w) pool.emplace_back(fill_csr, w);
        for (auto& t : pool) t.join();
    }
    for (unsigned w = 0; w < WORKERS; ++w) {
        local_edges[w].clear();
        local_edges[w].shrink_to_fit();
        local_deg[w].clear();
        local_deg[w].shrink_to_fit();
    }

    Mapping query_file("/data/queries.txt");
    const char* qp = query_file.data;
    const char* qend = qp + query_file.size;
    std::vector<std::pair<uint32_t, uint32_t>> queries;
    queries.reserve(2500);
    while (qp < qend) {
        uint32_t s = fast_parse_uint(qp, qend);
        if (qp >= qend) break;
        uint32_t t = fast_parse_uint(qp, qend);
        queries.push_back({s, t});
    }

    std::unordered_map<uint64_t, uint32_t> unique_index;
    std::vector<std::pair<uint32_t, uint32_t>> unique;
    std::vector<uint32_t> remap(queries.size());
    for (size_t i = 0; i < queries.size(); ++i) {
        const uint64_t key = (static_cast<uint64_t>(queries[i].first) << 32) | queries[i].second;
        auto [it, inserted] = unique_index.emplace(key, static_cast<uint32_t>(unique.size()));
        if (inserted) unique.push_back(queries[i]);
        remap[i] = it->second;
    }

    std::vector<int64_t> unique_answer(unique.size()), answers(queries.size());
    std::atomic<size_t> next{0};
    {
        std::vector<std::thread> pool; pool.reserve(WORKERS);
        for (unsigned t = 0; t < WORKERS; ++t) {
            pool.emplace_back([&] {
                Worker worker(offsets, edges);
                for (;;) {
                    const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= unique.size()) break;
                    unique_answer[i] = worker.shortest(unique[i].first, unique[i].second);
                }
            });
        }
        for (auto& t : pool) t.join();
    }
    for (size_t i = 0; i < answers.size(); ++i) answers[i] = unique_answer[remap[i]];

    std::string output; output.reserve(answers.size() * 12);
    for (int64_t a : answers) {
        char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), a);
        output.append(buf, p);
        output.push_back('\n');
    }
    const int out_fd = open("/out/distances.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) _exit(2);
    const char* wp = output.data();
    size_t rem = output.size();
    while (rem) {
        ssize_t written = write(out_fd, wp, rem);
        if (written <= 0) _exit(2);
        wp += written;
        rem -= static_cast<size_t>(written);
    }
    close(out_fd);
    return 0;
}
