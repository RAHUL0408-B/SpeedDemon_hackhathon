#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
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

static inline uint64_t fnv1a_case(const char* p, size_t len) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= TO_LOWER[static_cast<unsigned char>(p[i])];
        h *= 1099511628211ULL;
    }
    return h == 0 ? 1 : h;
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
        if (term_table[idx].hash == h && term_table[idx].len() == len) {
            const char* term_ptr = string_pool.data() + term_table[idx].str_offset;
            if (eq_ignore_case(p, term_ptr, len)) return static_cast<int>(term_table[idx].id());
        }
        idx = (idx + 1) & TABLE_MASK;
    }
    return -1;
}

static uint32_t insert_term(std::string_view word) {
    const uint64_t h = fnv1a_case(word.data(), word.size());
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
    std::vector<std::string> phrase_words;
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
            q.phrase_words.emplace_back(w);
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

static inline bool phrase_anchored(const char* doc_start, const char* doc_end, const std::vector<std::string>& phrase, size_t anchor_idx) noexcept {
    const size_t phrase_len = phrase.size();
    if (phrase_len == 0) return true;
    const std::string& anc = phrase[anchor_idx];
    const size_t anc_len = anc.size();
    const char anc_lower = static_cast<char>(TO_LOWER[static_cast<unsigned char>(anc[0])]);
    const char anc_upper = static_cast<char>((anc_lower >= 'a' && anc_lower <= 'z') ? (anc_lower - 32) : anc_lower);

    const char* p = doc_start;
    while (p < doc_end) {
        while (p < doc_end && !IS_TOKEN[static_cast<unsigned char>(*p)]) ++p;
        if (p >= doc_end) break;
        char c = *p;
        if (c != anc_lower && c != anc_upper) {
            while (p < doc_end && IS_TOKEN[static_cast<unsigned char>(*p)]) ++p;
            continue;
        }
        const char* w_start = p;
        while (p < doc_end && IS_TOKEN[static_cast<unsigned char>(*p)]) ++p;
        const size_t w_len = static_cast<size_t>(p - w_start);
        if (w_len == anc_len && eq_ignore_case(w_start, anc.data(), w_len)) {
            bool left_ok = true;
            const char* q = w_start;
            for (int k = static_cast<int>(anchor_idx) - 1; k >= 0; --k) {
                while (q > doc_start && !IS_TOKEN[static_cast<unsigned char>(q[-1])]) --q;
                if (q <= doc_start) { left_ok = false; break; }
                const char* prev_end = q;
                while (q > doc_start && IS_TOKEN[static_cast<unsigned char>(q[-1])]) --q;
                const char* prev_start = q;
                const size_t prev_len = static_cast<size_t>(prev_end - prev_start);
                if (prev_len != phrase[k].size() || !eq_ignore_case(prev_start, phrase[k].data(), prev_len)) {
                    left_ok = false; break;
                }
            }
            if (!left_ok) continue;

            bool right_ok = true;
            const char* cur = p;
            for (size_t k = anchor_idx + 1; k < phrase_len; ++k) {
                while (cur < doc_end && !IS_TOKEN[static_cast<unsigned char>(*cur)]) ++cur;
                if (cur >= doc_end) { right_ok = false; break; }
                const char* next_start = cur;
                while (cur < doc_end && IS_TOKEN[static_cast<unsigned char>(*cur)]) ++cur;
                const size_t next_len = static_cast<size_t>(cur - next_start);
                if (next_len != phrase[k].size() || !eq_ignore_case(next_start, phrase[k].data(), next_len)) {
                    right_ok = false; break;
                }
            }
            if (right_ok) return true;
        }
    }
    return false;
}

static inline void set_and(const std::vector<uint32_t>& a, const uint32_t* b, size_t b_len, std::vector<uint32_t>& out) {
    const size_t a_len = a.size();
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
    };

    auto pass1_worker = [](void* arg_ptr) -> void* {
        Pass1Context* ctx = static_cast<Pass1Context*>(arg_ptr);
        const char* p = ctx->p; const char* limit = ctx->limit;
        uint32_t cur_doc = ctx->cur_doc;
        auto& my_counts = *(ctx->my_counts);
        auto& my_tokens = *(ctx->my_tokens); my_tokens.reserve(2'000'000);
        auto& my_doc_counts = *(ctx->my_doc_counts); my_doc_counts.resize(ctx->num_my_docs);
        std::vector<uint32_t> last_seen(ctx->term_count, std::numeric_limits<uint32_t>::max());

        while (p < limit) {
            const char* tab = static_cast<const char*>(std::memchr(p, '\t', static_cast<size_t>(limit - p)));
            if (!tab) break;
            const char* tb = tab + 1;
            const char* nl = static_cast<const char*>(std::memchr(tb, '\n', static_cast<size_t>(limit - tb)));
            if (!nl) nl = limit;
            ctx->documents[cur_doc] = {tb, nl};

            uint16_t doc_toks = 0;
            const char* tp = tb;
            while (tp < nl) {
                while (tp < nl && !IS_TOKEN[static_cast<unsigned char>(*tp)]) ++tp;
                const char* wb = tp;
                uint64_t h = 1469598103934665603ULL;
                while (tp < nl && IS_TOKEN[static_cast<unsigned char>(*tp)]) {
                    h ^= TO_LOWER[static_cast<unsigned char>(*tp++)];
                    h *= 1099511628211ULL;
                }
                const size_t len = static_cast<size_t>(tp - wb);
                if (len > 0 && bloom_test(h)) {
                    int tid = lookup_term(wb, len, h);
                    if (tid >= 0 && last_seen[static_cast<size_t>(tid)] != cur_doc) {
                        last_seen[static_cast<size_t>(tid)] = cur_doc;
                        my_counts[static_cast<size_t>(tid)]++;
                        my_tokens.push_back(static_cast<uint32_t>(tid));
                        doc_toks++;
                    }
                }
            }
            my_doc_counts[cur_doc - ctx->cur_doc] = doc_toks;
            cur_doc++; p = nl < limit ? nl + 1 : limit;
        }
        return nullptr;
    };

    Pass1Context p1_contexts[workers];
    for (unsigned w = 0; w < workers; ++w) {
        p1_contexts[w] = {w, starts[w], starts[w + 1], doc_start[w], doc_start[w + 1] - doc_start[w], term_count, documents.data(), &term_counts[w], &worker_tokens[w], &worker_doc_tok_count[w]};
        pthread_create(&threads[w], &attr, pass1_worker, &p1_contexts[w]);
    }
    for (unsigned w = 0; w < workers; ++w) pthread_join(threads[w], nullptr);

    std::vector<uint32_t> term_total(term_count, 0);
    for (size_t t = 0; t < term_count; ++t) {
        for (unsigned w = 0; w < workers; ++w) term_total[t] += term_counts[w][t];
    }
    std::vector<uint32_t> term_offsets(term_count + 1, 0);
    for (size_t t = 0; t < term_count; ++t) term_offsets[t + 1] = term_offsets[t] + term_total[t];

    std::vector<uint32_t> flat_postings(term_offsets[term_count]);
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
        uint32_t tok_idx = 0;

        for (uint32_t cur_doc = first; cur_doc < last; ++cur_doc) {
            const uint16_t num = my_counts[cur_doc - first];
            for (uint16_t k = 0; k < num; ++k) {
                uint32_t tid = my_tokens[tok_idx++];
                flat_postings[my_pos[tid]++] = cur_doc;
            }
        }
        return nullptr;
    };

    Pass2Context p2_contexts[workers];
    for (unsigned w = 0; w < workers; ++w) {
        p2_contexts[w] = {doc_start[w], doc_start[w + 1], &worker_write_pos[w], &worker_tokens[w], &worker_doc_tok_count[w], flat_postings.data()};
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
    std::vector<uint64_t> dense_bits;
    for (uint32_t term = 0; term < term_count; ++term) {
        if (term_total[term] <= 4096) continue;
        dense_index[term] = static_cast<int32_t>(dense_bits.size() / bitmap_words);
        const size_t base = dense_bits.size(); dense_bits.resize(base + bitmap_words, 0);
        const uint32_t* ids = flat_postings.data() + term_offsets[term];
        for (uint32_t i = 0; i < term_total[term]; ++i) dense_bits[base + (ids[i] >> 6)] |= 1ULL << (ids[i] & 63);
    }

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
        q_contexts[w] = {w, queries.size() * w / workers, queries.size() * (w + 1) / workers, &queries, documents.data(), flat_postings.data(), term_offsets.data(), term_total.data(), term_count, dense_index.data(), dense_bits.data(), bitmap_words, &local_out[w]};
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
