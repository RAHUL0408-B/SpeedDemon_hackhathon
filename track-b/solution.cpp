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

static inline bool token_char(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static inline bool eq_ignore_case(const char* p, const char* word, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        if (c != word[i]) return false;
    }
    return true;
}

static inline uint64_t fnv1a_case(const char* p, size_t len) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(p[i]);
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        h ^= c; h *= 1099511628211ULL;
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
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        string_pool.push_back(c);
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
    while (p < end) {
        while (p < end && !token_char(static_cast<unsigned char>(*p))) ++p;
        if (p >= end) break;
        const char* w_start = p;
        while (p < end && token_char(static_cast<unsigned char>(*p))) ++p;
        const size_t w_len = static_cast<size_t>(p - w_start);
        if (w_len == phrase[0].size() && eq_ignore_case(w_start, phrase[0].data(), w_len)) {
            const char* cur = p;
            bool all_matched = true;
            for (size_t k = 1; k < phrase_len; ++k) {
                while (cur < end && !token_char(static_cast<unsigned char>(*cur))) ++cur;
                if (cur >= end) { all_matched = false; break; }
                const char* next_start = cur;
                while (cur < end && token_char(static_cast<unsigned char>(*cur))) ++cur;
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
    out.clear(); out.reserve(std::min(a.size(), b_len));
    size_t i = 0, j = 0;
    while (i < a.size() && j < b_len) {
        if (a[i] < b[j]) ++i;
        else if (b[j] < a[i]) ++j;
        else { out.push_back(a[i]); ++i; ++j; }
    }
}
static inline void set_or(const std::vector<uint32_t>& a, const uint32_t* b, size_t b_len, std::vector<uint32_t>& out) {
    out.clear(); out.reserve(a.size() + b_len);
    size_t i = 0, j = 0;
    while (i < a.size() || j < b_len) {
        if (j == b_len || (i < a.size() && a[i] < b[j])) out.push_back(a[i++]);
        else if (i == a.size() || b[j] < a[i]) out.push_back(b[j++]);
        else { out.push_back(a[i]); ++i; ++j; }
    }
}
static inline void set_not(const std::vector<uint32_t>& a, const uint32_t* b, size_t b_len, std::vector<uint32_t>& out) {
    out.clear(); out.reserve(a.size());
    size_t i = 0, j = 0;
    while (i < a.size()) {
        while (j < b_len && b[j] < a[i]) ++j;
        if (j == b_len || a[i] != b[j]) out.push_back(a[i]);
        ++i;
    }
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

    uint32_t doc_counts[workers] = {};
    std::vector<std::thread> pool; pool.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&, w]() {
            const char* p = starts[w]; const char* limit = starts[w + 1];
            uint32_t c = 0;
            while (p < limit) {
                const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(limit - p)));
                if (!nl) break;
                c++; p = nl + 1;
            }
            doc_counts[w] = c;
        });
    }
    for (auto& t : pool) t.join(); pool.clear();

    uint32_t doc_start[workers + 1]; doc_start[0] = 0;
    for (unsigned w = 0; w < workers; ++w) doc_start[w + 1] = doc_start[w] + doc_counts[w];
    const uint32_t total_docs = doc_start[workers];
    std::vector<Document> documents(total_docs);

    std::vector<std::vector<uint32_t>> term_counts(workers, std::vector<uint32_t>(term_count, 0));
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&, w]() {
            const char* p = starts[w]; const char* limit = starts[w + 1];
            uint32_t cur_doc = doc_start[w];
            auto& my_counts = term_counts[w];
            std::vector<uint32_t> last_seen(term_count, std::numeric_limits<uint32_t>::max());

            while (p < limit) {
                const char* tab = static_cast<const char*>(std::memchr(p, '\t', static_cast<size_t>(limit - p)));
                if (!tab) break;
                const char* tb = tab + 1;
                const char* nl = static_cast<const char*>(std::memchr(tb, '\n', static_cast<size_t>(limit - tb)));
                if (!nl) nl = limit;
                documents[cur_doc] = {tb, nl};

                const char* tp = tb;
                while (tp < nl) {
                    while (tp < nl && !token_char(static_cast<unsigned char>(*tp))) ++tp;
                    const char* wb = tp;
                    uint64_t h = 1469598103934665603ULL;
                    while (tp < nl && token_char(static_cast<unsigned char>(*tp))) {
                        unsigned char c = static_cast<unsigned char>(*tp++);
                        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
                        h ^= c; h *= 1099511628211ULL;
                    }
                    const size_t len = static_cast<size_t>(tp - wb);
                    if (len > 0 && bloom_test(h)) {
                        int tid = lookup_term(wb, len, h);
                        if (tid >= 0 && last_seen[static_cast<size_t>(tid)] != cur_doc) {
                            last_seen[static_cast<size_t>(tid)] = cur_doc;
                            my_counts[static_cast<size_t>(tid)]++;
                        }
                    }
                }
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
            std::vector<uint32_t> last_seen(term_count, std::numeric_limits<uint32_t>::max());
            auto& my_pos = worker_write_pos[w];

            for (uint32_t cur_doc = first; cur_doc < last; ++cur_doc) {
                const Document& doc = documents[cur_doc];
                const char* tp = doc.begin; const char* nl = doc.end;
                while (tp < nl) {
                    while (tp < nl && !token_char(static_cast<unsigned char>(*tp))) ++tp;
                    const char* wb = tp;
                    uint64_t h = 1469598103934665603ULL;
                    while (tp < nl && token_char(static_cast<unsigned char>(*tp))) {
                        unsigned char c = static_cast<unsigned char>(*tp++);
                        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
                        h ^= c; h *= 1099511628211ULL;
                    }
                    const size_t len = static_cast<size_t>(tp - wb);
                    if (len > 0 && bloom_test(h)) {
                        int tid = lookup_term(wb, len, h);
                        if (tid >= 0 && last_seen[static_cast<size_t>(tid)] != cur_doc) {
                            last_seen[static_cast<size_t>(tid)] = cur_doc;
                            flat_postings[my_pos[static_cast<size_t>(tid)]++] = cur_doc;
                        }
                    }
                }
            }
        });
    }
    for (auto& t : pool) t.join(); pool.clear();

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
