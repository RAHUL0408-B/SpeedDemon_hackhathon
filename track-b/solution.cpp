#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

/* ── Memory-mapped file helper ────────────────────────────────────────────── */
struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY); struct stat st {};
        if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
        size = static_cast<size_t>(st.st_size);
        if (size) {
            data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0));
            if (data == MAP_FAILED) _exit(2);
            madvise(const_cast<char*>(data), size, MADV_SEQUENTIAL);
        }
    }
    ~Mapping() { if (data && data != MAP_FAILED) munmap(const_cast<char*>(data), size); if (fd >= 0) close(fd); }
};

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view text) const {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : text) { h ^= c; h *= 1099511628211ULL; }
        return static_cast<size_t>(h);
    }
    size_t operator()(const std::string& text) const { return operator()(std::string_view(text)); }
};
struct StringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const { return a == b; }
};

struct Document { const char* begin; const char* end; };

struct Query {
    bool phrase = false;
    std::vector<uint32_t> terms;
    std::vector<std::string> phrase_words;
    std::vector<uint8_t> ops; // 0 AND, 1 OR, 2 NOT
    std::vector<uint32_t> matches;
};

static std::unordered_map<std::string, uint32_t, StringHash, StringEqual> term_ids;

static uint32_t intern(std::string_view word) {
    auto it = term_ids.find(word);
    if (it != term_ids.end()) return it->second;
    const uint32_t id = static_cast<uint32_t>(term_ids.size());
    term_ids.emplace(std::string(word), id);
    return id;
}

static std::vector<std::string> read_lines(const char* path) {
    Mapping file(path);
    std::vector<std::string> lines; lines.reserve(50'000);
    const char* p = file.data; const char* end = p + file.size;
    while (p < end) {
        const char* begin = p; while (p < end && *p != '\n') ++p;
        lines.emplace_back(begin, static_cast<size_t>(p - begin));
        if (p < end) ++p;
    }
    return lines;
}

static Query parse_query(std::string_view line) {
    Query q;
    if (line.starts_with("PHRASE \"")) {
        q.phrase = true;
        std::string_view phrase_str = line.substr(8, line.size() - 9);
        size_t begin = 0;
        while (begin < phrase_str.size()) {
            size_t end = phrase_str.find(' ', begin);
            if (end == std::string_view::npos) end = phrase_str.size();
            std::string_view word = phrase_str.substr(begin, end - begin);
            q.terms.push_back(intern(word));
            q.phrase_words.emplace_back(word);
            begin = end + 1;
        }
        return q;
    }
    std::string_view rest = line.substr(5);
    size_t begin = 0; bool term = true;
    while (begin < rest.size()) {
        size_t end = rest.find(' ', begin);
        if (end == std::string_view::npos) end = rest.size();
        const std::string_view item = rest.substr(begin, end - begin);
        if (term) q.terms.push_back(intern(item));
        else if (item == "AND") q.ops.push_back(0);
        else if (item == "OR") q.ops.push_back(1);
        else q.ops.push_back(2);
        term = !term; begin = end + 1;
    }
    return q;
}

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

static int token_id(const char* begin, const char* end) noexcept {
    char lowered[64]; size_t n = 0;
    for (const char* p = begin; p < end && n < 63; ++p) {
        char c = *p; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        lowered[n++] = c;
    }
    auto it = term_ids.find(std::string_view(lowered, n));
    return it == term_ids.end() ? -1 : static_cast<int>(it->second);
}

template <class Callback>
static inline void scan_tokens(const Document& doc, Callback&& callback) {
    const char* p = doc.begin;
    while (p < doc.end) {
        while (p < doc.end && !token_char(static_cast<unsigned char>(*p))) ++p;
        const char* begin = p;
        while (p < doc.end && token_char(static_cast<unsigned char>(*p))) ++p;
        if (begin != p) callback(token_id(begin, p));
    }
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
                    all_matched = false;
                    break;
                }
            }
            if (all_matched) return true;
        }
    }
    return false;
}

static inline std::vector<uint32_t> set_and(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out; out.reserve(std::min(a.size(), b.size()));
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) ++i;
        else if (b[j] < a[i]) ++j;
        else { out.push_back(a[i]); ++i; ++j; }
    }
    return out;
}

static inline std::vector<uint32_t> set_or(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out; out.reserve(a.size() + b.size());
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j == b.size() || (i < a.size() && a[i] < b[j])) out.push_back(a[i++]);
        else if (i == a.size() || b[j] < a[i]) out.push_back(b[j++]);
        else { out.push_back(a[i]); ++i; ++j; }
    }
    return out;
}

static inline std::vector<uint32_t> set_not(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out; out.reserve(a.size());
    size_t i = 0, j = 0;
    while (i < a.size()) {
        while (j < b.size() && b[j] < a[i]) ++j;
        if (j == b.size() || a[i] != b[j]) out.push_back(a[i]);
        ++i;
    }
    return out;
}

static inline void append_uint(std::string& out, uint64_t n) {
    char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n); out.append(buf, p);
}
static inline void append_result(std::string& out, const std::vector<uint32_t>& ids) {
    append_uint(out, ids.size()); out.push_back(' ');
    uint64_t sum = 0; for (uint32_t id : ids) sum += id;
    append_uint(out, sum);
    for (size_t i = 0; i < ids.size() && i < 10; ++i) { out.push_back(' '); append_uint(out, ids[i]); }
    out.push_back('\n');
}

int main() {
    term_ids.reserve(160'000); term_ids.max_load_factor(0.7f);
    const std::vector<std::string> lines = read_lines("/data/queries.txt");
    std::vector<Query> queries; queries.reserve(lines.size());
    for (const std::string& line : lines) queries.push_back(parse_query(line));
    const size_t term_count = term_ids.size();

    Mapping docs_file("/data/docs.txt");
    constexpr unsigned workers = 12;

    std::vector<std::vector<Document>> local_docs(workers);
    auto scan_docs_slice = [&](unsigned w) {
        const char* p = docs_file.data + (docs_file.size * w / workers);
        const char* limit = docs_file.data + (docs_file.size * (w + 1) / workers);
        if (w > 0) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(docs_file.data + docs_file.size - p)));
            p = nl ? nl + 1 : limit;
        }
        if (w + 1 < workers) {
            const char* nl = static_cast<const char*>(std::memchr(limit, '\n', static_cast<size_t>(docs_file.data + docs_file.size - limit)));
            limit = nl ? nl + 1 : docs_file.data + docs_file.size;
        } else {
            limit = docs_file.data + docs_file.size;
        }

        local_docs[w].reserve(250'000 / workers + 1000);
        while (p < limit) {
            const char* tab = static_cast<const char*>(std::memchr(p, '\t', static_cast<size_t>(limit - p)));
            if (!tab) break;
            const char* begin = tab + 1;
            const char* nl = static_cast<const char*>(std::memchr(begin, '\n', static_cast<size_t>(limit - begin)));
            if (!nl) nl = limit;
            local_docs[w].push_back({begin, nl});
            p = nl < limit ? nl + 1 : limit;
        }
    };

    std::vector<std::thread> pool; pool.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) pool.emplace_back(scan_docs_slice, i);
    for (auto& t : pool) t.join();
    pool.clear();

    size_t total_docs = 0;
    for (unsigned w = 0; w < workers; ++w) total_docs += local_docs[w].size();
    std::vector<Document> documents; documents.reserve(total_docs);
    for (unsigned w = 0; w < workers; ++w) {
        documents.insert(documents.end(), local_docs[w].begin(), local_docs[w].end());
        local_docs[w].clear();
        local_docs[w].shrink_to_fit();
    }

    std::vector<std::vector<uint32_t>> term_counts(workers, std::vector<uint32_t>(term_count, 0));
    auto count_range = [&](unsigned w) {
        const size_t first = documents.size() * w / workers;
        const size_t last = documents.size() * (w + 1) / workers;
        std::vector<uint32_t> last_seen(term_count, std::numeric_limits<uint32_t>::max());
        auto& my_counts = term_counts[w];
        for (size_t i = first; i < last; ++i) {
            scan_tokens(documents[i], [&](int id) {
                if (id >= 0 && last_seen[static_cast<size_t>(id)] != i) {
                    last_seen[static_cast<size_t>(id)] = static_cast<uint32_t>(i);
                    my_counts[static_cast<size_t>(id)]++;
                }
            });
        }
    };
    for (unsigned i = 0; i < workers; ++i) pool.emplace_back(count_range, i);
    for (auto& t : pool) t.join();
    pool.clear();

    std::vector<uint32_t> term_total(term_count, 0);
    for (size_t t = 0; t < term_count; ++t) {
        for (unsigned w = 0; w < workers; ++w) {
            term_total[t] += term_counts[w][t];
        }
    }

    std::vector<uint32_t> term_offsets(term_count + 1, 0);
    for (size_t t = 0; t < term_count; ++t) {
        term_offsets[t + 1] = term_offsets[t] + term_total[t];
    }
    const uint32_t total_postings = term_offsets[term_count];
    std::vector<uint32_t> flat_postings(total_postings);
    std::vector<std::vector<uint32_t>> worker_write_pos(workers, std::vector<uint32_t>(term_count));
    for (size_t t = 0; t < term_count; ++t) {
        uint32_t cur = term_offsets[t];
        for (unsigned w = 0; w < workers; ++w) {
            worker_write_pos[w][t] = cur;
            cur += term_counts[w][t];
        }
    }

    auto fill_range = [&](unsigned w) {
        const size_t first = documents.size() * w / workers;
        const size_t last = documents.size() * (w + 1) / workers;
        std::vector<uint32_t> last_seen(term_count, std::numeric_limits<uint32_t>::max());
        auto& my_pos = worker_write_pos[w];
        for (size_t i = first; i < last; ++i) {
            scan_tokens(documents[i], [&](int id) {
                if (id >= 0 && last_seen[static_cast<size_t>(id)] != i) {
                    last_seen[static_cast<size_t>(id)] = static_cast<uint32_t>(i);
                    flat_postings[my_pos[static_cast<size_t>(id)]++] = static_cast<uint32_t>(i);
                }
            });
        }
    };
    for (unsigned i = 0; i < workers; ++i) pool.emplace_back(fill_range, i);
    for (auto& t : pool) t.join();
    pool.clear();

    auto get_postings = [&](uint32_t term_id) -> std::pair<const uint32_t*, size_t> {
        if (term_id >= term_count) return {nullptr, 0};
        return {&flat_postings[term_offsets[term_id]], term_total[term_id]};
    };

    auto eval_query_range = [&](unsigned w) {
        const size_t first = queries.size() * w / workers;
        const size_t last = queries.size() * (w + 1) / workers;

        for (size_t qi = first; qi < last; ++qi) {
            Query& q = queries[qi];
            if (q.phrase) {
                std::vector<uint32_t> needed = q.terms;
                std::sort(needed.begin(), needed.end());
                needed.erase(std::unique(needed.begin(), needed.end()), needed.end());

                bool absent = false;
                for (uint32_t id : needed) {
                    if (term_total[id] == 0) { absent = true; break; }
                }
                if (absent) continue;

                std::sort(needed.begin(), needed.end(), [&](uint32_t a, uint32_t b) {
                    return term_total[a] < term_total[b];
                });

                auto [p0, len0] = get_postings(needed[0]);
                std::vector<uint32_t> candidates(p0, p0 + len0);

                for (size_t k = 1; k < needed.size() && !candidates.empty(); ++k) {
                    auto [pk, lenk] = get_postings(needed[k]);
                    std::vector<uint32_t> next_c;
                    next_c.reserve(std::min(candidates.size(), lenk));
                    size_t i = 0, j = 0;
                    while (i < candidates.size() && j < lenk) {
                        if (candidates[i] < pk[j]) ++i;
                        else if (pk[j] < candidates[i]) ++j;
                        else { next_c.push_back(candidates[i]); ++i; ++j; }
                    }
                    candidates = std::move(next_c);
                }

                q.matches.reserve(candidates.size());
                for (uint32_t id : candidates) {
                    const Document& doc = documents[id];
                    if (phrase_in_doc_fast(doc.begin, doc.end, q.phrase_words)) {
                        q.matches.push_back(id);
                    }
                }
            } else {
                auto [p0, len0] = get_postings(q.terms[0]);
                std::vector<uint32_t> result(p0, p0 + len0);
                for (size_t k = 0; k < q.ops.size(); ++k) {
                    auto [pk, lenk] = get_postings(q.terms[k + 1]);
                    std::vector<uint32_t> rhs(pk, pk + lenk);
                    if (q.ops[k] == 0) result = set_and(result, rhs);
                    else if (q.ops[k] == 1) result = set_or(result, rhs);
                    else result = set_not(result, rhs);
                }
                q.matches = std::move(result);
            }
        }
    };
    for (unsigned i = 0; i < workers; ++i) pool.emplace_back(eval_query_range, i);
    for (auto& t : pool) t.join();
    pool.clear();

    std::string output; output.reserve(4 * 1024 * 1024);
    for (const Query& q : queries) {
        append_result(output, q.matches);
    }
    const int out_fd = open("/out/results.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) _exit(2);
    const char* wp = output.data();
    size_t rem = output.size();
    while (rem > 0) {
        ssize_t written = write(out_fd, wp, rem);
        if (written <= 0) _exit(2);
        wp += written;
        rem -= static_cast<size_t>(written);
    }
    close(out_fd);
    return 0;
}
