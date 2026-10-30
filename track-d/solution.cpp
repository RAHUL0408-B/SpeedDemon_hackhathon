#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
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

struct Reader {
    const char* p; const char* end;
    inline uint32_t number() noexcept {
        while (p < end && static_cast<unsigned char>(*p) <= ' ') ++p;
        uint32_t n = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            n = n * 10u + static_cast<uint32_t>(*p++ - '0');
        }
        return n;
    }
};

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

struct Worker {
    const std::vector<uint32_t>& offsets;
    const std::vector<uint32_t>& heads;
    const std::vector<uint32_t>& weights;
    std::vector<uint64_t> distance;
    std::vector<uint32_t> seen;
    uint32_t stamp = 1;
    RadixHeap heap;
    Worker(const std::vector<uint32_t>& off, const std::vector<uint32_t>& to, const std::vector<uint32_t>& weight)
        : offsets(off), heads(to), weights(weight), distance(off.size() - 1), seen(off.size() - 1, 0) {}
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
                const uint32_t v = heads[e]; const uint64_t nd = cur.key + weights[e];
                if (seen[v] != stamp || nd < distance[v]) {
                    seen[v] = stamp; distance[v] = nd; heap.push(nd, v);
                }
            }
        }
        return -1;
    }
};

static inline void append_i64(std::string& out, int64_t n) {
    char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n); out.append(buf, p);
}

int main() {
    Mapping graph_file("/data/graph.txt");
    Reader first{graph_file.data, graph_file.data + graph_file.size};
    const uint32_t n = first.number(); const uint32_t m = first.number();
    std::vector<uint32_t> offsets(static_cast<size_t>(n) + 1, 0);
    for (uint32_t i = 0; i < m; ++i) { const uint32_t u = first.number(); first.number(); first.number(); ++offsets[u + 1]; }
    for (uint32_t i = 1; i <= n; ++i) offsets[i] += offsets[i - 1];
    std::vector<uint32_t> write_pos = offsets;
    std::vector<uint32_t> heads(m), weights(m);
    Reader second{graph_file.data, graph_file.data + graph_file.size}; second.number(); second.number();
    for (uint32_t i = 0; i < m; ++i) {
        const uint32_t u = second.number(), v = second.number(), w = second.number();
        const uint32_t slot = write_pos[u]++; heads[slot] = v; weights[slot] = w;
    }

    Mapping query_file("/data/queries.txt");
    Reader qr{query_file.data, query_file.data + query_file.size};
    std::vector<std::pair<uint32_t, uint32_t>> queries;
    queries.reserve(2500);
    while (qr.p < qr.end) {
        uint32_t s = qr.number();
        if (qr.p >= qr.end) break;
        uint32_t t = qr.number();
        queries.push_back({s, t});
    }
    std::unordered_map<uint64_t, uint32_t> unique_index;
    unique_index.reserve(queries.size() * 2);
    std::vector<std::pair<uint32_t, uint32_t>> unique;
    std::vector<uint32_t> remap(queries.size());
    for (uint32_t i = 0; i < queries.size(); ++i) {
        const uint64_t key = (static_cast<uint64_t>(queries[i].first) << 32) | queries[i].second;
        auto [it, inserted] = unique_index.emplace(key, static_cast<uint32_t>(unique.size()));
        if (inserted) unique.push_back(queries[i]);
        remap[i] = it->second;
    }
    std::vector<int64_t> unique_answer(unique.size()), answers(queries.size());
    std::atomic<size_t> next{0};
    constexpr unsigned WORKERS = 12;
    const unsigned workers = std::min<unsigned>(WORKERS, static_cast<unsigned>(unique.size()));
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned t = 0; t < workers; ++t) {
        pool.emplace_back([&] {
            Worker worker(offsets, heads, weights);
            for (;;) {
                const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= unique.size()) break;
                unique_answer[i] = worker.shortest(unique[i].first, unique[i].second);
            }
        });
    }
    for (auto& thread : pool) thread.join();
    for (size_t i = 0; i < answers.size(); ++i) answers[i] = unique_answer[remap[i]];
    std::string output; output.reserve(answers.size() * 12);
    for (int64_t answer : answers) { append_i64(output, answer); output.push_back('\n'); }

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
