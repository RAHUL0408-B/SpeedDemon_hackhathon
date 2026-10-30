#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
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

static inline uint32_t insert_term(std::string_view word) {
    uint64_t h = fnv1a_case(word.data(), word.size());
    int existing = lookup_term(word.data(), word.size(), h);
    if (existing >= 0) return static_cast<uint32_t>(existing);

    uint32_t idx = static_cast<uint32_t>(h) & TABLE_MASK;
    while (term_table[idx].hash != 0) idx = (idx + 1) & TABLE_MASK;

    uint32_t new_id = num_terms++;
    term_table[idx].hash = h;
    term_table[idx].str_offset = static_cast<uint32_t>(string_pool.size());
    term_table[idx].len_and_id = (static_cast<uint32_t>(word.size()) << 24) | new_id;

    for (char c : word) {
        string_pool.push_back(static_cast<char>(TO_LOWER[static_cast<unsigned char>(c)]));
    }
    bloom_add(h);
    return new_id;
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

static inline bool phrase_in_doc_fast(const char* p, const char* end, const std::vector<std::string>& phrase) noexcept {
    const size_t phrase_len = phrase.size();
    if (phrase_len == 0) return true;
    const char c0_lower = static_cast<char>(TO_LOWER[static_cast<unsigned char>(phrase[0][0])]);
    const char c0_upper = static_cast<char>((c0_lower >= 'a' && c0_lower <= 'z') ? (c0_lower - 32) : c0_lower);
    const size_t w0_len = phrase[0].size();

    while (p < end) {
        while (p < end && !IS_TOKEN[static_cast<unsigned char>(*p)]) ++p;
        if (p >= end) break;
        char c = *p;
        if (c != c0_lower && c != c0_upper) {
            while (p < end && IS_TOKEN[static_cast<unsigned char>(*p)]) ++p;
            continue;
        }
        const char* w_start = p;
        while (p < end && IS_TOKEN[static_cast<unsigned char>(*p)]) ++p;
        const size_t w_len = static_cast<size_t>(p - w_start);
        if (w_len == w0_len && eq_ignore_case(w_start, phrase[0].data(), w_len)) {
            const char* cur = p;
            bool all_matched = true;
            for (size_t k = 1; k < phrase_len; ++k) {
                while (cur < end && !IS_TOKEN[static_cast<unsigned char>(*cur)]) ++cur;
                if (cur >= end) { all_matched = false; break; }
                const char* next_start = cur;
                while (cur < end && IS_TOKEN[static_cast<unsigned char>(*cur)]) ++cur;
                const size_t next_len = static_cast<size_t>(cur - next_start);
                if (next_len != phrase[k].size() || !eq_ignore_case(next_start, phrase[k].data(), next_len)) {
                    all_matched = false; break;
                }
            }
            if (all_matched) return true;
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

    std::vector<std::thread> pool; pool.reserve(workers);
    std::vector<std::vector<uint32_t>> worker_tokens(workers);
    std::vector<std::vector<uint16_t>> worker_doc_tok_count(workers);
    std::vector<std::vector<uint32_t>> term_counts(workers, std::vector<uint32_t>(term_count, 0));

    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&, w]() {
            const char* p = starts[w]; const char* limit = starts[w + 1];
            uint32_t cur_doc = doc_start[w];
            const uint32_t num_my_docs = doc_start[w + 1] - doc_start[w];
            auto& my_counts = term_counts[w];
            auto& my_tokens = worker_tokens[w]; my_tokens.reserve(2'000'000);
            auto& my_doc_counts = worker_doc_tok_count[w]; my_doc_counts.resize(num_my_docs);
            std::vector<uint32_t> last_seen(term_count, std::numeric_limits<uint32_t>::max());

            while (p < limit) {
                const char* tab = static_cast<const char*>(std::memchr(p, '\t', static_cast<size_t>(limit - p)));
                if (!tab) break;
                const char* tb = tab + 1;
                const char* nl = static_cast<const char*>(std::memchr(tb, '\n', static_cast<size_t>(limit - tb)));
                if (!nl) nl = limit;
                documents[cur_doc] = {tb, nl};

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
                my_doc_counts[cur_doc - doc_start[w]] = doc_toks;
                cur_doc++; p = nl < limit ? nl + 1 : limit;
            }
        });
    }
    for (auto& t : pool) t.join(); pool.clear();

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

    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&, w]() {
            const uint32_t first = doc_start[w]; const uint32_t last = doc_start[w + 1];
            auto& my_pos = worker_write_pos[w];
            const auto& my_tokens = worker_tokens[w];
            const auto& my_counts = worker_doc_tok_count[w];
            uint32_t tok_idx = 0;

            for (uint32_t cur_doc = first; cur_doc < last; ++cur_doc) {
                const uint16_t num = my_counts[cur_doc - first];
                for (uint16_t k = 0; k < num; ++k) {
                    uint32_t tid = my_tokens[tok_idx++];
                    flat_postings[my_pos[tid]++] = cur_doc;
                }
            }
        });
    }
    for (auto& t : pool) t.join(); pool.clear();
    worker_tokens.clear(); worker_tokens.shrink_to_fit();
    worker_doc_tok_count.clear(); worker_doc_tok_count.shrink_to_fit();

    auto get_postings = [&](uint32_t tid) -> std::pair<const uint32_t*, size_t> {
        if (tid >= term_count) return {nullptr, 0};
        return {&flat_postings[term_offsets[tid]], term_total[tid]};
    };

    std::vector<std::string> local_out(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&, w]() {
            const size_t first = queries.size() * w / workers;
            const size_t last = queries.size() * (w + 1) / workers;
            std::vector<uint32_t> cur_res, tmp_res;
            cur_res.reserve(32768); tmp_res.reserve(32768);
            local_out[w].reserve(512 * 1024);

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
                        append_result(local_out[w], qr);
                        continue;
                    }

                    std::sort(needed.begin(), needed.end(), [&](uint32_t a, uint32_t b) {
                        return term_total[a] < term_total[b];
                    });

                    auto [p0, len0] = get_postings(needed[0]);
                    cur_res.assign(p0, p0 + len0);

                    for (size_t k = 1; k < needed.size() && !cur_res.empty(); ++k) {
                        auto [pk, lenk] = get_postings(needed[k]);
                        set_and(cur_res, pk, lenk, tmp_res);
                        cur_res.swap(tmp_res);
                    }

                    for (uint32_t id : cur_res) {
                        const Document& doc = documents[id];
                        if (phrase_in_doc_fast(doc.begin, doc.end, q.phrase_words)) {
                            qr.count++;
                            qr.sum += id;
                            if (qr.num10 < 10) qr.first10[qr.num10++] = id;
                        }
                    }
                } else {
                    auto [p0, len0] = get_postings(q.terms[0]);
                    cur_res.assign(p0, p0 + len0);
                    for (size_t k = 0; k < q.ops.size(); ++k) {
                        auto [pk, lenk] = get_postings(q.terms[k + 1]);
                        if (q.ops[k] == 0) set_and(cur_res, pk, lenk, tmp_res);
                        else if (q.ops[k] == 1) set_or(cur_res, pk, lenk, tmp_res);
                        else set_not(cur_res, pk, lenk, tmp_res);
                        cur_res.swap(tmp_res);
                    }
                    qr.count = static_cast<uint32_t>(cur_res.size());
                    for (uint32_t id : cur_res) qr.sum += id;
                    qr.num10 = static_cast<uint8_t>(std::min<size_t>(cur_res.size(), 10));
                    for (size_t i = 0; i < qr.num10; ++i) qr.first10[i] = cur_res[i];
                }
                append_result(local_out[w], qr);
            }
        });
    }
    for (auto& t : pool) t.join(); pool.clear();

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
