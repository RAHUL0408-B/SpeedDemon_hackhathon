/*  Track B – TECHBUILDER – ultra-optimised C++23
 *
 *  Key improvements:
 *   • Two-phase design: first intern all query terms (single-threaded, tiny),
 *     then index documents in parallel with zero lock contention.
 *   • Posting lists are built with thread-local vectors → merged once.
 *   • SIMD-friendly token scanner using lookup table (avoids branch per char).
 *   • Output built into a single string, single write() call.
 *   • MAP_POPULATE + MADV_SEQUENTIAL on both mmap'd files.
 *   • Queries evaluated with sorted posting lists (already sorted by construction).
 *   • Phrase search avoids building words[] vector – uses positional scan.
 *   • No global std::unordered_map contention during indexing.
 */
#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
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

/* ── character class table ─────────────────────────────────────────────────── */
static alignas(64) uint8_t is_alnum[256];

static void init_alnum() {
    for (int c = 'a'; c <= 'z'; ++c) is_alnum[c] = 1;
    for (int c = 'A'; c <= 'Z'; ++c) is_alnum[c] = 1;
    for (int c = '0'; c <= '9'; ++c) is_alnum[c] = 1;
}

/* ── fast custom FNV-based hash ────────────────────────────────────────────── */
struct SVHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : sv) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }
    size_t operator()(const std::string& s) const noexcept {
        return operator()(std::string_view(s));
    }
};
struct SVEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

/* ── global term dictionary ─────────────────────────────────────────────────── */
static std::unordered_map<std::string, uint32_t, SVHash, SVEqual> term_dict;
static std::vector<std::string> term_strings; // term_strings[id] = canonical string

static uint32_t intern_new(std::string_view word) {
    auto it = term_dict.find(word);
    if (it != term_dict.end()) return it->second;
    uint32_t id = static_cast<uint32_t>(term_dict.size());
    term_dict.emplace(std::string(word), id);
    term_strings.emplace_back(word);
    return id;
}

/* lowercase + intern (for document tokens) – returns -1 if unknown */
static inline int lookup_lower(const char* begin, const char* end) {
    // stack buffer – tokens bounded by size
    char buf[128]; size_t n = 0;
    for (const char* p = begin; p < end && n < sizeof(buf); ++p, ++n) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        buf[n] = c;
    }
    auto it = term_dict.find(std::string_view(buf, n));
    return it == term_dict.end() ? -1 : static_cast<int>(it->second);
}

/* ── document boundary ──────────────────────────────────────────────────────── */
struct Doc { const char* begin; const char* end; };

/* ── query ─────────────────────────────────────────────────────────────────── */
struct Query {
    bool phrase = false;
    std::vector<uint32_t> terms;
    std::vector<uint8_t>  ops;     // 0=AND,1=OR,2=NOT
    std::vector<uint32_t> matches; // filled during search
};

/* ── posting-list merge operations ─────────────────────────────────────────── */
static void set_and_into(std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t i = 0, j = 0, w = 0;
    while (i < a.size() && j < b.size()) {
        if      (a[i] < b[j]) ++i;
        else if (b[j] < a[i]) ++j;
        else   { a[w++] = a[i]; ++i; ++j; }
    }
    a.resize(w);
}
static std::vector<uint32_t> set_or(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out; out.reserve(a.size() + b.size());
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j == b.size() || (i < a.size() && a[i] < b[j]))      out.push_back(a[i++]);
        else if (i == a.size() || b[j] < a[i])                     out.push_back(b[j++]);
        else { out.push_back(a[i]); ++i; ++j; }
    }
    return out;
}
static void set_not_into(std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t i = 0, j = 0, w = 0;
    while (i < a.size()) {
        while (j < b.size() && b[j] < a[i]) ++j;
        if (j == b.size() || a[i] != b[j]) a[w++] = a[i];
        ++i;
    }
    a.resize(w);
}

/* ── output helpers ─────────────────────────────────────────────────────────── */
static void append_uint(std::string& out, uint64_t n) {
    char buf[22];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, p);
}
static void emit_result(std::string& out, const std::vector<uint32_t>& ids) {
    append_uint(out, ids.size()); out.push_back(' ');
    uint64_t sum = 0; for (uint32_t id : ids) sum += id;
    append_uint(out, sum);
    for (size_t i = 0; i < ids.size() && i < 10; ++i) {
        out.push_back(' '); append_uint(out, ids[i]);
    }
    out.push_back('\n');
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main() {
    init_alnum();

    term_dict.reserve(200'000);
    term_dict.max_load_factor(0.6f);

    /* ── 1. Read and parse queries, interning all query terms first ───── */
    Mapping qfile("/data/queries.txt");
    const char* qp  = qfile.data;
    const char* qend = qp + qfile.size;

    std::vector<Query> queries;
    queries.reserve(60'000);

    while (qp < qend) {
        const char* line = qp;
        while (qp < qend && *qp != '\n') ++qp;
        const char* lend = qp;
        if (qp < qend) ++qp;
        if (line == lend) continue;

        Query q;
        if (lend - line > 8 && line[0] == 'P') {
            // PHRASE "..."
            q.phrase = true;
            const char* inner = line + 8; // after 'PHRASE "'
            const char* inner_end = lend - 1; // before '"'
            // split on space
            const char* wp = inner;
            while (wp <= inner_end) {
                const char* ws = wp;
                while (wp < inner_end && *wp != ' ') ++wp;
                q.terms.push_back(intern_new(std::string_view(ws, wp - ws)));
                if (wp < inner_end) ++wp;
            }
        } else {
            // BOOL t1 OP t2 OP ...
            q.phrase = false;
            const char* rest = line + 5; // past "BOOL "
            bool is_term = true;
            while (rest < lend) {
                const char* ws = rest;
                while (rest < lend && *rest != ' ') ++rest;
                std::string_view tok(ws, rest - ws);
                if (is_term) {
                    q.terms.push_back(intern_new(tok));
                } else {
                    if (tok == "AND") q.ops.push_back(0);
                    else if (tok == "OR") q.ops.push_back(1);
                    else q.ops.push_back(2); // NOT
                }
                is_term = !is_term;
                if (rest < lend) ++rest;
            }
        }
        queries.push_back(std::move(q));
    }

    const size_t term_count = term_dict.size();

    /* ── 2. Load documents ─────────────────────────────────────────────── */
    Mapping dfile("/data/docs.txt");
    const char* dp  = dfile.data;
    const char* dend = dp + dfile.size;

    std::vector<Doc> docs;
    docs.reserve(300'000);
    while (dp < dend) {
        // format: "id\tcontent\n"
        const char* tab = static_cast<const char*>(memchr(dp, '\t', static_cast<size_t>(dend - dp)));
        if (!tab) break;
        const char* content = tab + 1;
        const char* nl = static_cast<const char*>(memchr(content, '\n', static_cast<size_t>(dend - content)));
        const char* content_end = nl ? nl : dend;
        docs.push_back({content, content_end});
        dp = nl ? nl + 1 : dend;
    }
    const uint32_t doc_count = static_cast<uint32_t>(docs.size());

    /* ── 3. Build inverted index in parallel ──────────────────────────── */
    // Each worker builds its own posting lists (per-term vectors).
    // Then we merge in sorted order.
    constexpr unsigned WORKERS = 12;
    const unsigned workers = std::min<unsigned>(WORKERS, doc_count);

    // posting[term] = sorted doc_ids (built per worker, merged)
    std::vector<std::vector<uint32_t>> postings(term_count);
    for (auto& p : postings) p.reserve(64);

    // Per-worker local posting lists to avoid contention
    std::vector<std::vector<std::vector<uint32_t>>> local_post(
        workers, std::vector<std::vector<uint32_t>>(term_count));

    auto index_range = [&](unsigned w) {
        const uint32_t first = doc_count * w / workers;
        const uint32_t last  = doc_count * (w + 1) / workers;
        auto& lp = local_post[w];
        // We need a "last_seen" per term to avoid duplicates within same doc
        std::vector<uint32_t> last_seen(term_count, UINT32_MAX);

        for (uint32_t di = first; di < last; ++di) {
            const Doc& d = docs[di];
            const char* p = d.begin;
            while (p < d.end) {
                // skip non-alnum
                while (p < d.end && !is_alnum[static_cast<unsigned char>(*p)]) ++p;
                const char* tok_start = p;
                while (p < d.end &&  is_alnum[static_cast<unsigned char>(*p)]) ++p;
                if (tok_start == p) continue;
                const int tid = lookup_lower(tok_start, p);
                if (tid >= 0 && last_seen[static_cast<size_t>(tid)] != di) {
                    last_seen[static_cast<size_t>(tid)] = di;
                    lp[static_cast<size_t>(tid)].push_back(di);
                }
            }
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (unsigned i = 0; i < workers; ++i) pool.emplace_back(index_range, i);
        for (auto& t : pool) t.join();
    }

    // Merge local postings → global postings (already sorted by doc_id across workers)
    for (unsigned w = 0; w < workers; ++w) {
        for (size_t t = 0; t < term_count; ++t) {
            auto& lp = local_post[w][t];
            if (!lp.empty())
                postings[t].insert(postings[t].end(), lp.begin(), lp.end());
        }
    }
    // free local storage
    local_post.clear();
    local_post.shrink_to_fit();

    /* ── 4. Phrase queries: compute matches ─────────────────────────────── */
    // For each phrase query, intersect postings of all unique terms,
    // then verify phrase order in those candidate docs.
    auto phrase_matches = [&](Query& q) {
        // find unique terms sorted by posting size (rarest first)
        std::vector<uint32_t> unique_terms = q.terms;
        std::sort(unique_terms.begin(), unique_terms.end());
        unique_terms.erase(std::unique(unique_terms.begin(), unique_terms.end()), unique_terms.end());
        std::sort(unique_terms.begin(), unique_terms.end(),
                  [&](uint32_t a, uint32_t b){ return postings[a].size() < postings[b].size(); });

        // check for any absent term
        for (uint32_t t : unique_terms)
            if (postings[t].empty()) return;

        // intersect
        std::vector<uint32_t> candidates = postings[unique_terms[0]];
        for (size_t i = 1; i < unique_terms.size() && !candidates.empty(); ++i)
            set_and_into(candidates, postings[unique_terms[i]]);

        // verify phrase within each candidate doc
        const size_t plen = q.terms.size();
        std::vector<int> word_ids;
        word_ids.reserve(2048);
        for (uint32_t di : candidates) {
            const Doc& d = docs[di];
            word_ids.clear();
            const char* p = d.begin;
            while (p < d.end) {
                while (p < d.end && !is_alnum[static_cast<unsigned char>(*p)]) ++p;
                const char* ts = p;
                while (p < d.end &&  is_alnum[static_cast<unsigned char>(*p)]) ++p;
                if (ts == p) continue;
                word_ids.push_back(lookup_lower(ts, p));
            }
            // substring search for phrase
            if (word_ids.size() < plen) continue;
            bool found = false;
            for (size_t start = 0; start + plen <= word_ids.size() && !found; ++start) {
                bool ok = true;
                for (size_t j = 0; j < plen; ++j) {
                    if (word_ids[start + j] != static_cast<int>(q.terms[j])) { ok = false; break; }
                }
                if (ok) found = true;
            }
            if (found) q.matches.push_back(di);
        }
    };

    // Run phrase matching in parallel
    std::atomic<size_t> next_phrase{0};
    // collect phrase query indices
    std::vector<size_t> phrase_idx;
    for (size_t i = 0; i < queries.size(); ++i)
        if (queries[i].phrase) phrase_idx.push_back(i);

    {
        const unsigned pw = std::min<unsigned>(WORKERS, static_cast<unsigned>(phrase_idx.size() + 1));
        std::atomic<size_t> pi{0};
        std::vector<std::thread> pool;
        pool.reserve(pw);
        for (unsigned i = 0; i < pw; ++i) {
            pool.emplace_back([&]() {
                for (;;) {
                    const size_t idx = pi.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= phrase_idx.size()) break;
                    phrase_matches(queries[phrase_idx[idx]]);
                }
            });
        }
        for (auto& t : pool) t.join();
    }

    /* ── 5. Boolean queries & output ────────────────────────────────────── */
    std::string output;
    output.reserve(4 * 1024 * 1024);

    for (Query& q : queries) {
        if (q.phrase) {
            emit_result(output, q.matches);
            continue;
        }
        // Boolean: evaluate left-to-right
        std::vector<uint32_t> result = postings[q.terms[0]];
        for (size_t i = 0; i < q.ops.size(); ++i) {
            const auto& rhs = postings[q.terms[i + 1]];
            if      (q.ops[i] == 0) set_and_into(result, rhs);
            else if (q.ops[i] == 1) result = set_or(result, rhs);
            else                    set_not_into(result, rhs);
        }
        emit_result(output, result);
    }

    /* ── 6. Write output ─────────────────────────────────────────────────── */
    const int fd = open("/out/results.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
