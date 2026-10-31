#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>

struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY); struct stat st {};
        if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
        size = static_cast<size_t>(st.st_size);
        if (size) {
            data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
            if (data == MAP_FAILED) _exit(2);
        }
    }
    ~Mapping() { if (data && data != MAP_FAILED) munmap(const_cast<char*>(data), size); if (fd >= 0) close(fd); }
};

alignas(64) static uint8_t IS_TOKEN[256];
alignas(64) static uint8_t TO_LOWER[256];

static void init_tables() noexcept {
    for (int i = 0; i < 256; ++i) {
        TO_LOWER[i] = (i >= 'A' && i <= 'Z') ? static_cast<uint8_t>(i + 32) : static_cast<uint8_t>(i);
        IS_TOKEN[i] = ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || (i >= '0' && i <= '9')) ? 1 : 0;
    }
}

static inline bool eq_ignore_case(const char* p, const char* word, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) {
        if (TO_LOWER[static_cast<unsigned char>(p[i])] != static_cast<unsigned char>(word[i])) return false;
    }
    return true;
}

static inline uint64_t fold(uint64_t h, uint64_t w) noexcept {
    h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
    return h ^ (h >> 32);
}

// Hash of an alphanumeric token, case-folded with OR 0x20 (exact for [A-Za-z0-9]), eight bytes per step.
// Tokens up to 8 bytes map one-to-one onto their hash. Reads up to 7 bytes past the token.
static inline uint64_t hash_token(const char* p, size_t len) noexcept {
    uint64_t h = 0x243F6A8885A308D3ULL;
    for (;;) {
        uint64_t w; std::memcpy(&w, p, 8);
        const uint64_t keep = len >= 8 ? ~0ULL : ((1ULL << (len * 8)) - 1);
        h = fold(h, (w | 0x2020202020202020ULL) & keep);
        if (len <= 8) return h ? h : 1;
        p += 8; len -= 8;
    }
}

// Calls emit(start, len) for each maximal [A-Za-z0-9] run in [b, e), 32 bytes at a time.
// Reads up to 31 bytes past e.
template <class F> static inline void scan_tokens(const char* b, const char* e, F&& emit) {
    const __m256i c0 = _mm256_set1_epi8('0'), c9 = _mm256_set1_epi8(9), c20 = _mm256_set1_epi8(0x20);
    const __m256i ca = _mm256_set1_epi8('a'), c25 = _mm256_set1_epi8(25);
    const char* tstart = nullptr;
    uint32_t prev = 0;
    for (const char* blk = b; blk < e; blk += 32) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(blk));
        const __m256i d = _mm256_sub_epi8(v, c0);
        const __m256i l = _mm256_sub_epi8(_mm256_or_si256(v, c20), ca);
        const __m256i hit = _mm256_or_si256(_mm256_cmpeq_epi8(_mm256_min_epu8(d, c9), d),
                                            _mm256_cmpeq_epi8(_mm256_min_epu8(l, c25), l));
        uint32_t m = static_cast<uint32_t>(_mm256_movemask_epi8(hit));
        const size_t rem = static_cast<size_t>(e - blk);
        if (rem < 32) m &= (1u << rem) - 1u;
        uint32_t ev = m ^ ((m << 1) | prev);  // bit i set where byte i starts or ends a run
        prev = m >> 31;
        while (ev) {
            const unsigned i = static_cast<unsigned>(__builtin_ctz(ev));
            ev &= ev - 1;
            if ((m >> i) & 1u) tstart = blk + i;
            else emit(tstart, static_cast<size_t>(blk + i - tstart));
        }
    }
    if (prev) emit(tstart, static_cast<size_t>(e - tstart));
}

alignas(64) static uint64_t bloom[16384] = {};
static inline void bloom_add(uint64_t h) noexcept {
    uint32_t b1 = static_cast<uint32_t>(h) & 0xFFFFF;
    uint32_t b2 = static_cast<uint32_t>(h >> 32) & 0xFFFFF;
    uint32_t b3 = static_cast<uint32_t>((h * 0x517cc1b727220a95ULL) >> 32) & 0xFFFFF;
    bloom[b1 >> 6] |= (1ULL << (b1 & 63));
    bloom[b2 >> 6] |= (1ULL << (b2 & 63));
    bloom[b3 >> 6] |= (1ULL << (b3 & 63));
}
static inline bool bloom_test(uint64_t h) noexcept {
    uint32_t b1 = static_cast<uint32_t>(h) & 0xFFFFF;
    uint32_t b2 = static_cast<uint32_t>(h >> 32) & 0xFFFFF;
    uint32_t b3 = static_cast<uint32_t>((h * 0x517cc1b727220a95ULL) >> 32) & 0xFFFFF;
    return ((bloom[b1 >> 6] & (1ULL << (b1 & 63))) != 0) &&
           ((bloom[b2 >> 6] & (1ULL << (b2 & 63))) != 0) &&
           ((bloom[b3 >> 6] & (1ULL << (b3 & 63))) != 0);
}

static constexpr uint32_t TABLE_CAP = 262144;
static constexpr uint32_t TABLE_MASK = TABLE_CAP - 1;

struct TableEntry {
    uint64_t hash = 0; uint32_t str_offset = 0; uint32_t len_and_id = 0;
    uint16_t len() const noexcept { return static_cast<uint16_t>(len_and_id >> 24); }
    uint32_t id() const noexcept { return len_and_id & 0x00FFFFFF; }
};

static TableEntry term_table[TABLE_CAP];
static std::string string_pool;
static uint32_t num_terms = 0;

static inline int lookup_term(const char* p, size_t len, uint64_t h) noexcept {
    uint32_t idx = static_cast<uint32_t>(h) & TABLE_MASK;
    while (term_table[idx].hash != 0) {
        // A full 64-bit hash plus length is unique among this corpus's few hundred thousand tokens.
        if (term_table[idx].hash == h && term_table[idx].len() == len) return static_cast<int>(term_table[idx].id());
        idx = (idx + 1) & TABLE_MASK;
    }
    return -1;
}

static uint32_t insert_term(std::string_view word) {
    char stack_buf[72];
    std::string heap_buf;
    char* padded = stack_buf;
    if (word.size() > 64) { heap_buf.resize(word.size() + 8); padded = heap_buf.data(); }
    for (size_t i = 0; i < word.size(); ++i) padded[i] = static_cast<char>(TO_LOWER[static_cast<unsigned char>(word[i])]);
    std::memset(padded + word.size(), 0, 8);
    const uint64_t h = hash_token(padded, word.size());
    bloom_add(h);
    uint32_t idx = static_cast<uint32_t>(h) & TABLE_MASK;
    while (term_table[idx].hash != 0) {
        if (term_table[idx].hash == h && term_table[idx].len() == word.size()) {
            const char* term_ptr = string_pool.data() + term_table[idx].str_offset;
            if (eq_ignore_case(word.data(), term_ptr, word.size())) return term_table[idx].id();
        }
        idx = (idx + 1) & TABLE_MASK;
    }
    const uint32_t id = num_terms++;
    term_table[idx].hash = h;
    term_table[idx].str_offset = static_cast<uint32_t>(string_pool.size());
    term_table[idx].len_and_id = (static_cast<uint32_t>(word.size()) << 24) | id;
    for (char c : word) string_pool.push_back(static_cast<char>(TO_LOWER[static_cast<unsigned char>(c)]));
    return id;
}

struct Document { const char* begin; const char* end; };
struct Query {
    bool phrase = false;
    std::vector<uint32_t> terms;
    std::vector<std::string_view> phrase_words;
    std::vector<uint8_t> ops;
};

struct QueryResult {
    uint32_t count = 0;
    uint64_t sum = 0;
    uint32_t first10[10] = {};
    uint8_t num10 = 0;
};

static Query parse_query(std::string_view line) {
    Query q;
    if (line.starts_with("PHRASE \"")) {
        q.phrase = true;
        std::string_view s = line.substr(8, line.size() - 9);
        size_t b = 0;
        while (b < s.size()) {
            size_t e = s.find(' ', b); if (e == std::string_view::npos) e = s.size();
            std::string_view w = s.substr(b, e - b);
            q.terms.push_back(insert_term(w));
            q.phrase_words.push_back(w);
            b = e + 1;
        }
        return q;
    }
    std::string_view rest = line.substr(5);
    size_t b = 0; bool term = true;
    while (b < rest.size()) {
        size_t e = rest.find(' ', b); if (e == std::string_view::npos) e = rest.size();
        std::string_view item = rest.substr(b, e - b);
        if (term) q.terms.push_back(insert_term(item));
        else if (item == "AND") q.ops.push_back(0);
        else if (item == "OR") q.ops.push_back(1);
        else q.ops.push_back(2);
        term = !term; b = e + 1;
    }
    return q;
}

static inline bool phrase_anchored(const char* doc_start, const char* doc_end, const std::vector<std::string_view>& phrase, size_t anchor_idx) noexcept {
    const size_t phrase_len = phrase.size();
    if (phrase_len == 0) return true;
    const std::string_view anc = phrase[anchor_idx];
    const size_t anc_len = anc.size();
    const auto is_tok = [](char c) { return IS_TOKEN[static_cast<unsigned char>(c)] != 0; };
    const auto word_eq = [](const char* s, size_t n, std::string_view w) {
        return n == w.size() && eq_ignore_case(s, w.data(), n);
    };

    // The anchor word sits at [w_start, w_end) as a whole token; check the words around it.
    const auto around = [&](const char* w_start, const char* w_end) {
        const char* q = w_start;
        for (int k = static_cast<int>(anchor_idx) - 1; k >= 0; --k) {
            while (q > doc_start && !is_tok(q[-1])) --q;
            if (q <= doc_start) return false;
            const char* prev_end = q;
            while (q > doc_start && is_tok(q[-1])) --q;
            if (!word_eq(q, static_cast<size_t>(prev_end - q), phrase[static_cast<size_t>(k)])) return false;
        }
        const char* cur = w_end;
        for (size_t k = anchor_idx + 1; k < phrase_len; ++k) {
            while (cur < doc_end && !is_tok(*cur)) ++cur;
            if (cur >= doc_end) return false;
            const char* next_start = cur;
            while (cur < doc_end && is_tok(*cur)) ++cur;
            if (!word_eq(next_start, static_cast<size_t>(cur - next_start), phrase[k])) return false;
        }
        return true;
    };
    const auto at = [&](const char* s) {
        if (static_cast<size_t>(doc_end - s) < anc_len || !eq_ignore_case(s, anc.data(), anc_len)) return false;
        if (s > doc_start && is_tok(s[-1])) return false;
        if (s + anc_len < doc_end && is_tok(s[anc_len])) return false;
        return around(s, s + anc_len);
    };

    // Vector pass: flag every offset whose first two bytes match the anchor (either case), then verify exactly.
    const bool two = anc_len >= 2;
    const __m256i c20 = _mm256_set1_epi8(0x20);
    const __m256i b0 = _mm256_set1_epi8(static_cast<char>(TO_LOWER[static_cast<unsigned char>(anc[0])] | 0x20));
    const __m256i b1 = _mm256_set1_epi8(static_cast<char>(two ? (TO_LOWER[static_cast<unsigned char>(anc[1])] | 0x20) : 0));
    const char* s = doc_start;
    for (; s + 33 <= doc_end; s += 32) {
        __m256i m = _mm256_cmpeq_epi8(_mm256_or_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s)), c20), b0);
        if (two) m = _mm256_and_si256(m, _mm256_cmpeq_epi8(_mm256_or_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 1)), c20), b1));
        for (uint32_t bits = static_cast<uint32_t>(_mm256_movemask_epi8(m)); bits; bits &= bits - 1) {
            if (at(s + __builtin_ctz(bits))) return true;
        }
    }
    for (; s < doc_end; ++s) if (at(s)) return true;
    return false;
}

static inline void set_and(const std::vector<uint32_t>& a, const uint32_t* b, size_t b_len, std::vector<uint32_t>& out) {
    const size_t a_len = a.size();
    // Phrase anchors are usually U/R terms.  Do not linearly walk a huge
    // VC/C posting list just to filter a few hundred anchor candidates.
    if (b_len > a_len * 8) {
        out.clear(); out.reserve(a_len);
        size_t lo = 0;
        for (uint32_t id : a) {
            const uint32_t* it = std::lower_bound(b + lo, b + b_len, id);
            lo = static_cast<size_t>(it - b);
            if (it != b + b_len && *it == id) out.push_back(id);
        }
        return;
    }
    if (a_len > b_len * 8) {
        out.clear(); out.reserve(b_len);
        size_t lo = 0;
        for (size_t j = 0; j < b_len; ++j) {
            const uint32_t id = b[j];
            const auto it = std::lower_bound(a.begin() + static_cast<std::ptrdiff_t>(lo), a.end(), id);
            lo = static_cast<size_t>(it - a.begin());
            if (it != a.end() && *it == id) out.push_back(id);
        }
        return;
    }
    out.resize(std::min(a_len, b_len));
    uint32_t* dst = out.data();
    size_t i = 0, j = 0, cnt = 0;
    while (i < a_len && j < b_len) {
        if (a[i] < b[j]) ++i;
        else if (b[j] < a[i]) ++j;
        else { dst[cnt++] = a[i]; ++i; ++j; }
    }
    out.resize(cnt);
}

static inline void set_or(const std::vector<uint32_t>& a, const uint32_t* b, size_t b_len, std::vector<uint32_t>& out) {
    const size_t a_len = a.size();
    out.resize(a_len + b_len);
    uint32_t* dst = out.data();
    size_t i = 0, j = 0, cnt = 0;
    while (i < a_len || j < b_len) {
        if (j == b_len || (i < a_len && a[i] < b[j])) dst[cnt++] = a[i++];
        else if (i == a_len || b[j] < a[i]) dst[cnt++] = b[j++];
        else { dst[cnt++] = a[i]; ++i; ++j; }
    }
    out.resize(cnt);
}

static inline void set_not(const std::vector<uint32_t>& a, const uint32_t* b, size_t b_len, std::vector<uint32_t>& out) {
    const size_t a_len = a.size();
    out.resize(a_len);
    uint32_t* dst = out.data();
    size_t i = 0, j = 0, cnt = 0;
    while (i < a_len) {
        while (j < b_len && b[j] < a[i]) ++j;
        if (j == b_len || a[i] != b[j]) dst[cnt++] = a[i];
        ++i;
    }
    out.resize(cnt);
}

static inline void append_uint(std::string& out, uint64_t n) {
    char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n); out.append(buf, p);
}

static inline void append_result(std::string& out, const QueryResult& qr) {
    append_uint(out, qr.count); out.push_back(' ');
    append_uint(out, qr.sum);
    for (size_t i = 0; i < qr.num10; ++i) {
        out.push_back(' ');
        append_uint(out, qr.first10[i]);
    }
    out.push_back('\n');
}

int main() {
    init_tables();
    string_pool.reserve(1024 * 1024);
    Mapping qfile("/data/queries.txt");
    std::vector<Query> queries; queries.reserve(50'000);
    const char* qp = qfile.data; const char* qend = qp + qfile.size;
    while (qp < qend) {
        const char* qb = qp; while (qp < qend && *qp != '\n') ++qp;
        queries.push_back(parse_query(std::string_view(qb, static_cast<size_t>(qp - qb))));
        if (qp < qend) ++qp;
    }
    const uint32_t term_count = num_terms;

    Mapping docs_file("/data/docs.txt");
    constexpr unsigned workers = 12;
    const char* starts[workers + 1];
    starts[0] = docs_file.data; starts[workers] = docs_file.data + docs_file.size;
    for (unsigned w = 1; w < workers; ++w) {
        const char* p = docs_file.data + (docs_file.size * w / workers);
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(starts[workers] - p)));
        starts[w] = nl ? nl + 1 : starts[workers];
    }

    uint32_t doc_start[workers + 1];
    doc_start[0] = 0;
    for (unsigned w = 1; w < workers; ++w) {
        const char* p = starts[w];
        uint32_t n = 0;
        while (p < starts[workers] && *p >= '0' && *p <= '9') n = n * 10u + (*p++ - '0');
        doc_start[w] = n;
    }
    {
        const char* p = starts[workers] - 2;
        while (p > docs_file.data && *p != '\n') --p;
        if (*p == '\n') ++p;
        uint32_t n = 0;
        while (p < starts[workers] && *p >= '0' && *p <= '9') n = n * 10u + (*p++ - '0');
        doc_start[workers] = n + 1;
    }
    const uint32_t total_docs = doc_start[workers];
    std::vector<Document> documents(total_docs);

    std::vector<std::vector<uint32_t>> worker_tokens(workers);
    std::vector<std::vector<uint16_t>> worker_doc_tok_count(workers);
    std::vector<std::vector<uint32_t>> term_counts(workers, std::vector<uint32_t>(term_count, 0));

    pthread_t threads[workers];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 524288); // 512 KB stack size to minimize virtual memory

    struct Pass1Context {
        unsigned w;
        const char* p;
        const char* limit;
        uint32_t cur_doc;
        uint32_t num_my_docs;
        uint32_t term_count;
        Document* documents;
        std::vector<uint32_t>* my_counts;
        std::vector<uint32_t>* my_tokens;
        std::vector<uint16_t>* my_doc_counts;
        const char* file_end;
    };

    auto pass1_worker = [](void* arg_ptr) -> void* {
        Pass1Context* ctx = static_cast<Pass1Context*>(arg_ptr);
        const char* p = ctx->p; const char* limit = ctx->limit;
        uint32_t cur_doc = ctx->cur_doc;
        auto& my_counts = *(ctx->my_counts);
        auto& my_tokens = *(ctx->my_tokens); my_tokens.reserve(2'000'000);
        auto& my_doc_counts = *(ctx->my_doc_counts); my_doc_counts.resize(ctx->num_my_docs);
        struct LastCount { uint32_t last, count; };  // one cache line per term touch
        std::vector<LastCount> lc(ctx->term_count, LastCount{std::numeric_limits<uint32_t>::max(), 0});
        struct Cand { uint64_t h; uint32_t len; };
        constexpr unsigned BATCH = 64;
        Cand cand[BATCH]; int tids[BATCH];
        {   // Pre-fault this worker's slice in one call instead of one fault per page.
            const uintptr_t lo = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(4095);
            madvise(reinterpret_cast<void*>(lo), reinterpret_cast<uintptr_t>(limit) - lo, 22 /* MADV_POPULATE_READ */);
        }

        while (p < limit) {
            const char* tab = static_cast<const char*>(std::memchr(p, '\t', static_cast<size_t>(limit - p)));
            if (!tab) break;
            const char* tb = tab + 1;
            const char* nl = static_cast<const char*>(std::memchr(tb, '\n', static_cast<size_t>(limit - tb)));
            if (!nl) nl = limit;
            ctx->documents[cur_doc] = {tb, nl};

            uint16_t doc_toks = 0;
            unsigned nc = 0;
            // Resolve a batch: prefetch every table slot, look them up, prefetch the per-term state, then update.
            auto flush = [&]() {
                for (unsigned i = 0; i < nc; ++i) __builtin_prefetch(&term_table[static_cast<uint32_t>(cand[i].h) & TABLE_MASK]);
                for (unsigned i = 0; i < nc; ++i) {
                    tids[i] = lookup_term(nullptr, cand[i].len, cand[i].h);
                    if (tids[i] >= 0) __builtin_prefetch(&lc[static_cast<size_t>(tids[i])], 1);
                }
                for (unsigned i = 0; i < nc; ++i) {
                    if (tids[i] < 0) continue;
                    LastCount& e = lc[static_cast<size_t>(tids[i])];
                    if (e.last == cur_doc) continue;
                    e.last = cur_doc; e.count++;
                    my_tokens.push_back(static_cast<uint32_t>(tids[i]));
                    doc_toks++;
                }
                nc = 0;
            };
            auto emit = [&](const char* s, size_t len) {
                const uint64_t h = hash_token(s, len);
                if (bloom_test(h)) {
                    cand[nc++] = {h, static_cast<uint32_t>(len)};
                    if (nc == BATCH) flush();
                }
            };
            if (nl + 64 <= ctx->file_end) scan_tokens(tb, nl, emit);
            else {  // the vector loads and 8-byte hashes read past the end: use a zero-padded copy
                std::vector<char> pad(static_cast<size_t>(nl - tb) + 64, 0);
                std::memcpy(pad.data(), tb, static_cast<size_t>(nl - tb));
                scan_tokens(pad.data(), pad.data() + (nl - tb), emit);
            }
            flush();
            my_doc_counts[cur_doc - ctx->cur_doc] = doc_toks;
            cur_doc++; p = nl < limit ? nl + 1 : limit;
        }
        for (size_t t = 0; t < lc.size(); ++t) my_counts[t] = lc[t].count;
        return nullptr;
    };

    Pass1Context p1_contexts[workers];
    for (unsigned w = 0; w < workers; ++w) {
        p1_contexts[w] = {w, starts[w], starts[w + 1], doc_start[w], doc_start[w + 1] - doc_start[w], term_count, documents.data(), &term_counts[w], &worker_tokens[w], &worker_doc_tok_count[w], starts[workers]};
        pthread_create(&threads[w], &attr, pass1_worker, &p1_contexts[w]);
    }
    for (unsigned w = 0; w < workers; ++w) pthread_join(threads[w], nullptr);

    std::vector<uint32_t> term_total(term_count, 0);
    for (size_t t = 0; t < term_count; ++t) {
        for (unsigned w = 0; w < workers; ++w) term_total[t] += term_counts[w][t];
    }
    std::vector<uint32_t> term_offsets(term_count + 1, 0);
    for (size_t t = 0; t < term_count; ++t) term_offsets[t + 1] = term_offsets[t] + term_total[t];

    // Left uninitialised: pass 2 writes every slot, and its threads take the page faults in parallel.
    std::unique_ptr<uint32_t[]> flat_postings(new uint32_t[term_offsets[term_count]]);
    std::vector<std::vector<uint32_t>> worker_write_pos(workers, std::vector<uint32_t>(term_count));
    for (size_t t = 0; t < term_count; ++t) {
        uint32_t cur = term_offsets[t];
        for (unsigned w = 0; w < workers; ++w) {
            worker_write_pos[w][t] = cur;
            cur += term_counts[w][t];
        }
    }

    struct Pass2Context {
        uint32_t first;
        uint32_t last;
        std::vector<uint32_t>* my_pos;
        const std::vector<uint32_t>* my_tokens;
        const std::vector<uint16_t>* my_counts;
        uint32_t* flat_postings;
    };

    auto pass2_worker = [](void* arg_ptr) -> void* {
        Pass2Context* ctx = static_cast<Pass2Context*>(arg_ptr);
        const uint32_t first = ctx->first; const uint32_t last = ctx->last;
        auto& my_pos = *(ctx->my_pos);
        const auto& my_tokens = *(ctx->my_tokens);
        const auto& my_counts = *(ctx->my_counts);
        uint32_t* flat_postings = ctx->flat_postings;
        const size_t ntok = my_tokens.size();
        size_t tok_idx = 0;

        for (uint32_t cur_doc = first; cur_doc < last; ++cur_doc) {
            const uint16_t num = my_counts[cur_doc - first];
            for (uint16_t k = 0; k < num; ++k, ++tok_idx) {
                if (tok_idx + 16 < ntok) __builtin_prefetch(&my_pos[my_tokens[tok_idx + 16]], 1);
                if (tok_idx + 8 < ntok) __builtin_prefetch(&flat_postings[my_pos[my_tokens[tok_idx + 8]]], 1);
                flat_postings[my_pos[my_tokens[tok_idx]]++] = cur_doc;
            }
        }
        return nullptr;
    };

    Pass2Context p2_contexts[workers];
    for (unsigned w = 0; w < workers; ++w) {
        p2_contexts[w] = {doc_start[w], doc_start[w + 1], &worker_write_pos[w], &worker_tokens[w], &worker_doc_tok_count[w], flat_postings.get()};
        pthread_create(&threads[w], &attr, pass2_worker, &p2_contexts[w]);
    }
    for (unsigned w = 0; w < workers; ++w) pthread_join(threads[w], nullptr);

    worker_tokens.clear(); worker_tokens.shrink_to_fit();
    worker_doc_tok_count.clear(); worker_doc_tok_count.shrink_to_fit();

    auto get_postings = [&](uint32_t tid) -> std::pair<const uint32_t*, size_t> {
        if (tid >= term_count) return {nullptr, 0};
        return {&flat_postings[term_offsets[tid]], term_total[tid]};
    };

    // Dense bitmaps turn operations involving the very common terms into
    // cache-friendly word operations. Sparse terms keep their compact postings.
    const uint32_t bitmap_words = (total_docs + 63) >> 6;
    std::vector<int32_t> dense_index(term_count, -1);
    std::vector<uint32_t> dense_terms;
    for (uint32_t term = 0; term < term_count; ++term) {
        if (term_total[term] <= 4096) continue;
        dense_index[term] = static_cast<int32_t>(dense_terms.size());
        dense_terms.push_back(term);
    }
    // calloc hands back lazily zeroed pages, so the build threads below also take the zero-fill faults.
    uint64_t* const dense_bits = static_cast<uint64_t*>(std::calloc(dense_terms.size() * bitmap_words + 1, sizeof(uint64_t)));
    struct DenseContext {
        unsigned w; const std::vector<uint32_t>* terms; const uint32_t* postings; const uint32_t* offsets;
        const uint32_t* totals; uint64_t* bits; uint32_t words;
    };
    auto dense_worker = [](void* arg_ptr) -> void* {
        const DenseContext* c = static_cast<const DenseContext*>(arg_ptr);
        for (size_t i = c->w; i < c->terms->size(); i += workers) {
            const uint32_t term = (*c->terms)[i];
            uint64_t* dst = c->bits + i * c->words;
            const uint32_t* ids = c->postings + c->offsets[term];
            for (uint32_t j = 0; j < c->totals[term]; ++j) dst[ids[j] >> 6] |= 1ULL << (ids[j] & 63);
        }
        return nullptr;
    };
    DenseContext d_contexts[workers];
    for (unsigned w = 0; w < workers; ++w) {
        d_contexts[w] = {w, &dense_terms, flat_postings.get(), term_offsets.data(), term_total.data(), dense_bits, bitmap_words};
        pthread_create(&threads[w], &attr, dense_worker, &d_contexts[w]);
    }
    for (unsigned w = 0; w < workers; ++w) pthread_join(threads[w], nullptr);

    std::vector<std::string> local_out(workers);

    struct QueryContext {
        unsigned w;
        size_t first;
        size_t last;
        const std::vector<Query>* queries;
        const Document* documents;
        const uint32_t* flat_postings;
        const uint32_t* term_offsets;
        const uint32_t* term_total;
        uint32_t term_count;
        const int32_t* dense_index;
        const uint64_t* dense_bits;
        uint32_t bitmap_words;
        std::string* out;
    };

    auto query_worker = [](void* arg_ptr) -> void* {
        QueryContext* ctx = static_cast<QueryContext*>(arg_ptr);
        const size_t first = ctx->first; const size_t last = ctx->last;
        const auto& queries = *(ctx->queries);
        const Document* documents = ctx->documents;
        const uint32_t* flat_postings = ctx->flat_postings;
        const uint32_t* term_offsets = ctx->term_offsets;
        const uint32_t* term_total = ctx->term_total;
        const uint32_t term_count = ctx->term_count;
        std::string& out = *(ctx->out);

        auto get_postings_local = [&](uint32_t tid) -> std::pair<const uint32_t*, size_t> {
            if (tid >= term_count) return {nullptr, 0};
            return {&flat_postings[term_offsets[tid]], term_total[tid]};
        };

        std::vector<uint32_t> cur_res, tmp_res;
        cur_res.reserve(32768); tmp_res.reserve(32768);
        std::vector<uint64_t> bits(ctx->bitmap_words), tmp_bits(ctx->bitmap_words);
        out.reserve(512 * 1024);

        for (size_t qi = first; qi < last; ++qi) {
            const Query& q = queries[qi];
            QueryResult qr;
            if (q.phrase) {
                std::vector<uint32_t> needed = q.terms;
                std::sort(needed.begin(), needed.end());
                needed.erase(std::unique(needed.begin(), needed.end()), needed.end());

                bool absent = false;
                for (uint32_t id : needed) if (term_total[id] == 0) { absent = true; break; }
                if (absent) {
                    append_result(out, qr);
                    continue;
                }

                std::sort(needed.begin(), needed.end(), [&](uint32_t a, uint32_t b) {
                    return term_total[a] < term_total[b];
                });

                const uint32_t rarest_tid = needed[0];
                size_t anchor_idx = 0;
                for (size_t k = 0; k < q.terms.size(); ++k) {
                    if (q.terms[k] == rarest_tid) { anchor_idx = k; break; }
                }

                auto [p0, len0] = get_postings_local(needed[0]);
                cur_res.assign(p0, p0 + len0);

                for (size_t k = 1; k < needed.size() && !cur_res.empty(); ++k) {
                    auto [pk, lenk] = get_postings_local(needed[k]);
                    set_and(cur_res, pk, lenk, tmp_res);
                    cur_res.swap(tmp_res);
                }

                for (uint32_t id : cur_res) {
                    const Document& doc = documents[id];
                    if (phrase_anchored(doc.begin, doc.end, q.phrase_words, anchor_idx)) {
                        qr.count++;
                        qr.sum += id;
                        if (qr.num10 < 10) qr.first10[qr.num10++] = id;
                    }
                }
            } else {
                auto [p0, len0] = get_postings_local(q.terms[0]);
                if (q.ops.empty()) {
                    qr.count = static_cast<uint32_t>(len0);
                    qr.num10 = static_cast<uint8_t>(std::min<size_t>(len0, 10));
                    for (size_t i = 0; i < len0; ++i) qr.sum += p0[i];
                    for (size_t i = 0; i < qr.num10; ++i) qr.first10[i] = p0[i];
                    append_result(out, qr);
                    continue;
                }
                bool dense = ctx->dense_index[q.terms[0]] >= 0;
                if (dense) {
                    const uint64_t* src = ctx->dense_bits + static_cast<size_t>(ctx->dense_index[q.terms[0]]) * ctx->bitmap_words;
                    std::memcpy(bits.data(), src, static_cast<size_t>(ctx->bitmap_words) * sizeof(uint64_t));
                } else cur_res.assign(p0, p0 + len0);
                for (size_t k = 0; k < q.ops.size(); ++k) {
                    auto [pk, lenk] = get_postings_local(q.terms[k + 1]);
                    const int32_t right_dense = ctx->dense_index[q.terms[k + 1]];
                    if (dense) {
                        if (right_dense >= 0) {
                            const uint64_t* rhs = ctx->dense_bits + static_cast<size_t>(right_dense) * ctx->bitmap_words;
                            if (q.ops[k] == 0) for (uint32_t w = 0; w < ctx->bitmap_words; ++w) bits[w] &= rhs[w];
                            else if (q.ops[k] == 1) for (uint32_t w = 0; w < ctx->bitmap_words; ++w) bits[w] |= rhs[w];
                            else for (uint32_t w = 0; w < ctx->bitmap_words; ++w) bits[w] &= ~rhs[w];
                        } else if (q.ops[k] == 0) {
                            std::fill(tmp_bits.begin(), tmp_bits.end(), 0);
                            for (size_t i = 0; i < lenk; ++i) if ((bits[pk[i] >> 6] >> (pk[i] & 63)) & 1ULL) tmp_bits[pk[i] >> 6] |= 1ULL << (pk[i] & 63);
                            bits.swap(tmp_bits);
                        } else if (q.ops[k] == 1) {
                            for (size_t i = 0; i < lenk; ++i) bits[pk[i] >> 6] |= 1ULL << (pk[i] & 63);
                        } else {
                            for (size_t i = 0; i < lenk; ++i) bits[pk[i] >> 6] &= ~(1ULL << (pk[i] & 63));
                        }
                    } else if (right_dense >= 0) {
                        const uint64_t* rhs = ctx->dense_bits + static_cast<size_t>(right_dense) * ctx->bitmap_words;
                        if (q.ops[k] == 1) {
                            std::memcpy(bits.data(), rhs, static_cast<size_t>(ctx->bitmap_words) * sizeof(uint64_t));
                            for (uint32_t id : cur_res) bits[id >> 6] |= 1ULL << (id & 63);
                            dense = true;
                        } else {
                            size_t dst = 0;
                            for (uint32_t id : cur_res) {
                                const bool present = (rhs[id >> 6] >> (id & 63)) & 1ULL;
                                if ((q.ops[k] == 0 && present) || (q.ops[k] == 2 && !present)) cur_res[dst++] = id;
                            }
                            cur_res.resize(dst);
                        }
                    } else {
                        if (q.ops[k] == 0) set_and(cur_res, pk, lenk, tmp_res);
                        else if (q.ops[k] == 1) set_or(cur_res, pk, lenk, tmp_res);
                        else set_not(cur_res, pk, lenk, tmp_res);
                        cur_res.swap(tmp_res);
                    }
                }
                if (dense) {
                    for (uint32_t w = 0; w < ctx->bitmap_words; ++w) {
                        uint64_t word = bits[w]; qr.count += static_cast<uint32_t>(__builtin_popcountll(word));
                        while (word) { const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(word)); const uint32_t id = (w << 6) + bit; qr.sum += id; if (qr.num10 < 10) qr.first10[qr.num10++] = id; word &= word - 1; }
                    }
                } else {
                    qr.count = static_cast<uint32_t>(cur_res.size());
                    for (uint32_t id : cur_res) qr.sum += id;
                    qr.num10 = static_cast<uint8_t>(std::min<size_t>(cur_res.size(), 10));
                    for (size_t i = 0; i < qr.num10; ++i) qr.first10[i] = cur_res[i];
                }
            }
            append_result(out, qr);
        }
        return nullptr;
    };

    QueryContext q_contexts[workers];
    for (unsigned w = 0; w < workers; ++w) {
        q_contexts[w] = {w, queries.size() * w / workers, queries.size() * (w + 1) / workers, &queries, documents.data(), flat_postings.get(), term_offsets.data(), term_total.data(), term_count, dense_index.data(), dense_bits, bitmap_words, &local_out[w]};
        pthread_create(&threads[w], &attr, query_worker, &q_contexts[w]);
    }
    for (unsigned w = 0; w < workers; ++w) pthread_join(threads[w], nullptr);
    pthread_attr_destroy(&attr);

    const int out_fd = open("/out/results.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) _exit(2);
    for (unsigned w = 0; w < workers; ++w) {
        const char* wp = local_out[w].data(); size_t rem = local_out[w].size();
        while (rem > 0) {
            ssize_t written = write(out_fd, wp, rem);
            if (written <= 0) _exit(2);
            wp += written; rem -= static_cast<size_t>(written);
        }
    }
    close(out_fd);
    return 0;
}
