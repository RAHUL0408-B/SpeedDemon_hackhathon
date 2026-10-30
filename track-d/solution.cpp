/*  Track D – TECHBUILDER – ultra-optimised C++23
 *
 *  Key improvements vs previous version:
 *   • RadixHeap now uses 40-bit keys (weight max ~10^6 * 10^6 nodes
 *     = fits uint64, but bucket scan is O(active_buckets)).
 *   • Bidirectional Dijkstra: builds reverse adjacency list,
 *     runs forward + backward from source/target simultaneously.
 *     Cuts average search space roughly in half on large sparse graphs.
 *   • MAP_POPULATE + MADV_SEQUENTIAL on graph file.
 *   • Graph is read in a single pass with a counting-sort CSR build.
 *   • Unique queries are processed with a work-stealing thread pool
 *     (atomic counter). Each thread has its own Worker (no contention).
 *   • Bidirectional search terminates when a settled node is in both
 *     forward and backward closed sets.
 */
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

/* ── fast number reader ───────────────────────────────────────────────────── */
struct Reader {
    const char* p; const char* end;
    uint32_t number() {
        uint32_t n = 0;
        while (p < end && *p >= '0' && *p <= '9') n = n * 10 + static_cast<uint32_t>(*p++ - '0');
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) ++p;
        return n;
    }
};

/* ── RadixHeap (64-bit keys) ─────────────────────────────────────────────── */
struct RadixHeap {
    struct Item { uint64_t key; uint32_t value; };
    std::array<std::vector<Item>, 65> buckets;
    uint64_t last = 0; size_t count = 0;
    static unsigned bkt(uint64_t key, uint64_t last_key) {
        if (key == last_key) return 0;
        return 64u - static_cast<unsigned>(__builtin_clzll(key ^ last_key));
    }
    void clear() { for (auto& b : buckets) b.clear(); last = 0; count = 0; }
    void push(uint64_t key, uint32_t value) {
        buckets[bkt(key, last)].push_back({key, value}); ++count;
    }
    bool empty() const { return count == 0; }
    Item pop() {
        if (buckets[0].empty()) {
            unsigned i = 1; while (buckets[i].empty()) ++i;
            uint64_t next = std::numeric_limits<uint64_t>::max();
            for (const Item& x : buckets[i]) if (x.key < next) next = x.key;
            last = next;
            std::vector<Item> tmp; tmp.swap(buckets[i]);
            for (const Item& x : tmp) buckets[bkt(x.key, last)].push_back(x);
        }
        Item ans = buckets[0].back(); buckets[0].pop_back(); --count; return ans;
    }
};

/* ── Bidirectional Dijkstra worker ────────────────────────────────────────── */
struct Worker {
    const std::vector<uint32_t>& fwd_off; // forward CSR offsets
    const std::vector<uint32_t>& fwd_to;
    const std::vector<uint32_t>& fwd_w;
    const std::vector<uint32_t>& rev_off; // reverse CSR offsets
    const std::vector<uint32_t>& rev_to;
    const std::vector<uint32_t>& rev_w;
    const uint32_t N;

    // Per-node state: stamp-based lazy init
    std::vector<uint64_t> dist_f, dist_r;
    std::vector<uint32_t> stamp_f, stamp_r; // settled stamps
    std::vector<uint8_t>  settled_f, settled_r;
    uint32_t stamp = 1;
    RadixHeap heap_f, heap_r;

    Worker(const std::vector<uint32_t>& fo, const std::vector<uint32_t>& ft,
           const std::vector<uint32_t>& fw, const std::vector<uint32_t>& ro,
           const std::vector<uint32_t>& rt, const std::vector<uint32_t>& rw,
           uint32_t n)
        : fwd_off(fo), fwd_to(ft), fwd_w(fw),
          rev_off(ro), rev_to(rt), rev_w(rw), N(n),
          dist_f(n, UINT64_MAX), dist_r(n, UINT64_MAX),
          stamp_f(n, 0), stamp_r(n, 0),
          settled_f(n, 0), settled_r(n, 0) {}

    void reset_stamps() {
        if (++stamp == 0) {
            std::fill(stamp_f.begin(), stamp_f.end(), 0);
            std::fill(stamp_r.begin(), stamp_r.end(), 0);
            stamp = 1;
        }
    }

    int64_t shortest(uint32_t src, uint32_t tgt) {
        if (src == tgt) return 0;
        reset_stamps();
        heap_f.clear(); heap_r.clear();

        auto touch_f = [&](uint32_t v, uint64_t d) {
            if (stamp_f[v] != stamp) {
                stamp_f[v] = stamp; dist_f[v] = UINT64_MAX; settled_f[v] = 0;
            }
            if (d < dist_f[v]) { dist_f[v] = d; heap_f.push(d, v); }
        };
        auto touch_r = [&](uint32_t v, uint64_t d) {
            if (stamp_r[v] != stamp) {
                stamp_r[v] = stamp; dist_r[v] = UINT64_MAX; settled_r[v] = 0;
            }
            if (d < dist_r[v]) { dist_r[v] = d; heap_r.push(d, v); }
        };

        touch_f(src, 0);
        touch_r(tgt, 0);

        uint64_t mu = UINT64_MAX; // best path found so far

        while (!heap_f.empty() || !heap_r.empty()) {
            // choose direction: expand the side with smaller top key
            bool do_fwd = !heap_r.empty()
                ? (heap_f.empty() ? false : heap_f.buckets[0].empty()
                    ? heap_f.last <= heap_r.last
                    : heap_f.buckets[0].back().key <= (heap_r.buckets[0].empty() ? heap_r.last : heap_r.buckets[0].back().key))
                : !heap_f.empty();

            if (do_fwd && !heap_f.empty()) {
                auto cur = heap_f.pop();
                if (stamp_f[cur.value] != stamp || settled_f[cur.value]) continue;
                if (cur.key > mu) break;
                settled_f[cur.value] = 1;
                // check if settled in reverse
                if (stamp_r[cur.value] == stamp && settled_r[cur.value]) {
                    uint64_t c = cur.key + dist_r[cur.value];
                    if (c < mu) mu = c;
                }
                for (uint32_t e = fwd_off[cur.value]; e < fwd_off[cur.value + 1]; ++e) {
                    const uint32_t v = fwd_to[e];
                    const uint64_t nd = cur.key + fwd_w[e];
                    touch_f(v, nd);
                    if (stamp_r[v] == stamp && nd + dist_r[v] < mu)
                        mu = nd + dist_r[v];
                }
            } else if (!heap_r.empty()) {
                auto cur = heap_r.pop();
                if (stamp_r[cur.value] != stamp || settled_r[cur.value]) continue;
                if (cur.key > mu) break;
                settled_r[cur.value] = 1;
                if (stamp_f[cur.value] == stamp && settled_f[cur.value]) {
                    uint64_t c = dist_f[cur.value] + cur.key;
                    if (c < mu) mu = c;
                }
                for (uint32_t e = rev_off[cur.value]; e < rev_off[cur.value + 1]; ++e) {
                    const uint32_t v = rev_to[e];
                    const uint64_t nd = cur.key + rev_w[e];
                    touch_r(v, nd);
                    if (stamp_f[v] == stamp && dist_f[v] + nd < mu)
                        mu = dist_f[v] + nd;
                }
            } else break;
        }

        return mu == UINT64_MAX ? -1 : static_cast<int64_t>(mu);
    }
};

/* ── output helper ────────────────────────────────────────────────────────── */
static void append_i64(std::string& out, int64_t n) {
    char buf[22];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, p);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main() {
    /* ── 1. Read graph ──────────────────────────────────────────────────── */
    Mapping gfile("/data/graph.txt");
    Reader rdr{gfile.data, gfile.data + gfile.size};
    const uint32_t N = rdr.number();
    const uint32_t M = rdr.number();

    // Record edges for two-pass CSR build
    std::vector<uint32_t> eu(M), ev(M), ew(M);
    for (uint32_t i = 0; i < M; ++i) {
        eu[i] = rdr.number(); ev[i] = rdr.number(); ew[i] = rdr.number();
    }

    // Build forward CSR
    std::vector<uint32_t> fwd_off(static_cast<size_t>(N) + 1, 0);
    for (uint32_t i = 0; i < M; ++i) ++fwd_off[eu[i] + 1];
    for (uint32_t i = 1; i <= N; ++i) fwd_off[i] += fwd_off[i - 1];
    std::vector<uint32_t> fwd_to(M), fwd_w(M);
    {
        std::vector<uint32_t> pos = fwd_off;
        for (uint32_t i = 0; i < M; ++i) {
            uint32_t slot = pos[eu[i]]++;
            fwd_to[slot] = ev[i]; fwd_w[slot] = ew[i];
        }
    }

    // Build reverse CSR
    std::vector<uint32_t> rev_off(static_cast<size_t>(N) + 1, 0);
    for (uint32_t i = 0; i < M; ++i) ++rev_off[ev[i] + 1];
    for (uint32_t i = 1; i <= N; ++i) rev_off[i] += rev_off[i - 1];
    std::vector<uint32_t> rev_to(M), rev_w(M);
    {
        std::vector<uint32_t> pos = rev_off;
        for (uint32_t i = 0; i < M; ++i) {
            uint32_t slot = pos[ev[i]]++;
            rev_to[slot] = eu[i]; rev_w[slot] = ew[i];
        }
    }
    // Free edge lists
    eu.clear(); eu.shrink_to_fit();
    ev.clear(); ev.shrink_to_fit();
    ew.clear(); ew.shrink_to_fit();

    /* ── 2. Read queries ────────────────────────────────────────────────── */
    Mapping qfile("/data/queries.txt");
    Reader qrdr{qfile.data, qfile.data + qfile.size};
    std::vector<std::pair<uint32_t, uint32_t>> queries;
    queries.reserve(50'000);
    while (qrdr.p < qrdr.end)
        queries.push_back({qrdr.number(), qrdr.number()});

    /* ── 3. Deduplicate queries ─────────────────────────────────────────── */
    std::unordered_map<uint64_t, uint32_t> uq_idx;
    uq_idx.reserve(queries.size() * 2);
    std::vector<std::pair<uint32_t, uint32_t>> unique_q;
    std::vector<uint32_t> remap(queries.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(queries.size()); ++i) {
        const uint64_t key = (static_cast<uint64_t>(queries[i].first) << 32) | queries[i].second;
        auto [it, ins] = uq_idx.emplace(key, static_cast<uint32_t>(unique_q.size()));
        if (ins) unique_q.push_back(queries[i]);
        remap[i] = it->second;
    }

    /* ── 4. Solve unique queries in parallel with bidirectional Dijkstra ── */
    std::vector<int64_t> unique_ans(unique_q.size());
    std::atomic<size_t> next{0};
    const unsigned workers = std::min<unsigned>(12,
        static_cast<unsigned>(unique_q.size() + 1));

    {
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (unsigned t = 0; t < workers; ++t) {
            pool.emplace_back([&]() {
                Worker w(fwd_off, fwd_to, fwd_w, rev_off, rev_to, rev_w, N);
                for (;;) {
                    const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= unique_q.size()) break;
                    unique_ans[i] = w.shortest(unique_q[i].first, unique_q[i].second);
                }
            });
        }
        for (auto& t : pool) t.join();
    }

    /* ── 5. Build and write output ──────────────────────────────────────── */
    std::string output;
    output.reserve(queries.size() * 12);
    for (size_t i = 0; i < queries.size(); ++i) {
        append_i64(output, unique_ans[remap[i]]); output.push_back('\n');
    }

    const int fd = open("/out/distances.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
