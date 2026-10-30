#include <algorithm>
#include <charconv>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY); struct stat st {};
        if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
        size = static_cast<size_t>(st.st_size);
        if (size) { data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0)); if (data == MAP_FAILED) _exit(2); }
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

static std::vector<uint32_t> split_terms(std::string_view text) {
    std::vector<uint32_t> answer;
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find(' ', begin); if (end == std::string_view::npos) end = text.size();
        answer.push_back(intern(text.substr(begin, end - begin)));
        begin = end + 1;
    }
    return answer;
}

static Query parse_query(std::string_view line) {
    Query q;
    if (line.starts_with("PHRASE \"")) {
        q.phrase = true;
        q.terms = split_terms(line.substr(8, line.size() - 9));
        return q;
    }
    std::string_view rest = line.substr(5); // BOOL and a following space
    size_t begin = 0; bool term = true;
    while (begin < rest.size()) {
        size_t end = rest.find(' ', begin); if (end == std::string_view::npos) end = rest.size();
        const std::string_view item = rest.substr(begin, end - begin);
        if (term) q.terms.push_back(intern(item));
        else if (item == "AND") q.ops.push_back(0);
        else if (item == "OR") q.ops.push_back(1);
        else q.ops.push_back(2);
        term = !term; begin = end + 1;
    }
    return q;
}

static inline bool token_char(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

static int token_id(const char* begin, const char* end) {
    char lowered[33]; size_t n = 0;
    for (const char* p = begin; p < end; ++p) {
        char c = *p; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        lowered[n++] = c;
    }
    auto it = term_ids.find(std::string_view(lowered, n));
    return it == term_ids.end() ? -1 : static_cast<int>(it->second);
}

template <class Callback>
static void scan_tokens(const Document& doc, Callback&& callback) {
    const char* p = doc.begin;
    while (p < doc.end) {
        while (p < doc.end && !token_char(static_cast<unsigned char>(*p))) ++p;
        const char* begin = p;
        while (p < doc.end && token_char(static_cast<unsigned char>(*p))) ++p;
        if (begin != p) callback(token_id(begin, p));
    }
}

static void token_ids(const Document& doc, std::vector<int>& out) {
    out.clear();
    scan_tokens(doc, [&out](int id) { out.push_back(id); });
}

static std::vector<uint32_t> set_and(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out; out.reserve(std::min(a.size(), b.size()));
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) { if (a[i] < b[j]) ++i; else if (b[j] < a[i]) ++j; else { out.push_back(a[i]); ++i; ++j; } }
    return out;
}
static std::vector<uint32_t> set_or(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out; out.reserve(a.size() + b.size());
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (j == b.size() || (i < a.size() && a[i] < b[j])) out.push_back(a[i++]);
        else if (i == a.size() || b[j] < a[i]) out.push_back(b[j++]);
        else { out.push_back(a[i]); ++i; ++j; }
    }
    return out;
}
static std::vector<uint32_t> set_not(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> out; out.reserve(a.size());
    size_t i = 0, j = 0;
    while (i < a.size()) { while (j < b.size() && b[j] < a[i]) ++j; if (j == b.size() || a[i] != b[j]) out.push_back(a[i]); ++i; }
    return out;
}

static bool phrase_in_document(const Document& doc, const std::vector<uint32_t>& phrase) {
    std::vector<int> words; token_ids(doc, words);
    if (words.size() < phrase.size()) return false;
    for (size_t i = 0; i + phrase.size() <= words.size(); ++i) {
        size_t j = 0; while (j < phrase.size() && words[i + j] == static_cast<int>(phrase[j])) ++j;
        if (j == phrase.size()) return true;
    }
    return false;
}

struct Watch { uint32_t query; uint8_t anchor; };

static void append_uint(std::string& out, uint64_t n) {
    char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n); out.append(buf, p);
}
static void append_result(std::string& out, const std::vector<uint32_t>& ids) {
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
    std::vector<std::vector<uint32_t>> postings(term_count);
    Mapping docs_file("/data/docs.txt");
    std::vector<Document> documents; documents.reserve(250'000);
    std::vector<uint32_t> last_seen(term_count, std::numeric_limits<uint32_t>::max());
    const char* p = docs_file.data; const char* end = p + docs_file.size;
    uint32_t doc_id = 0;
    while (p < end) {
        while (*p++ != '\t') {}
        const char* begin = p; while (p < end && *p != '\n') ++p;
        documents.push_back({begin, p});
        const Document doc{begin, p};
        scan_tokens(doc, [&](int id) {
            if (id >= 0 && last_seen[static_cast<size_t>(id)] != doc_id) {
                last_seen[static_cast<size_t>(id)] = doc_id;
                postings[static_cast<size_t>(id)].push_back(doc_id);
            }
        });
        if (p < end) ++p;
        ++doc_id;
    }

    std::vector<std::vector<Watch>> watches(term_count);
    std::vector<uint32_t> difficult;
    for (uint32_t qi = 0; qi < queries.size(); ++qi) {
        Query& q = queries[qi]; if (!q.phrase) continue;
        std::vector<uint32_t> needed = q.terms;
        std::sort(needed.begin(), needed.end()); needed.erase(std::unique(needed.begin(), needed.end()), needed.end());
        bool absent = false;
        for (uint32_t id : needed) if (postings[id].empty()) { absent = true; break; }
        if (absent) continue;
        std::sort(needed.begin(), needed.end(), [&postings](uint32_t a, uint32_t b) { return postings[a].size() < postings[b].size(); });
        std::vector<uint32_t> candidates = postings[needed[0]];
        for (size_t i = 1; i < needed.size() && !candidates.empty(); ++i) candidates = set_and(candidates, postings[needed[i]]);
        if (candidates.size() <= 4096) {
            q.matches.reserve(candidates.size());
            for (uint32_t id : candidates) if (phrase_in_document(documents[id], q.terms)) q.matches.push_back(id);
        } else {
            uint32_t anchor = needed[0]; uint8_t at = 0;
            for (; at < q.terms.size(); ++at) if (q.terms[at] == anchor) break;
            watches[anchor].push_back({qi, at}); difficult.push_back(qi);
        }
    }
    if (!difficult.empty()) {
        std::vector<uint32_t> last_matched(queries.size(), std::numeric_limits<uint32_t>::max());
        std::vector<int> words;
        for (uint32_t id = 0; id < documents.size(); ++id) {
            token_ids(documents[id], words);
            for (size_t pos = 0; pos < words.size(); ++pos) {
                const int token = words[pos]; if (token < 0 || watches[static_cast<size_t>(token)].empty()) continue;
                for (const Watch watch : watches[static_cast<size_t>(token)]) {
                    Query& q = queries[watch.query];
                    if (last_matched[watch.query] == id || pos < watch.anchor || pos - watch.anchor + q.terms.size() > words.size()) continue;
                    const size_t start = pos - watch.anchor;
                    size_t j = 0; while (j < q.terms.size() && words[start + j] == static_cast<int>(q.terms[j])) ++j;
                    if (j == q.terms.size()) { q.matches.push_back(id); last_matched[watch.query] = id; }
                }
            }
        }
    }

    std::string output; output.reserve(2 * 1024 * 1024);
    for (const Query& q : queries) {
        if (q.phrase) { append_result(output, q.matches); continue; }
        std::vector<uint32_t> result = postings[q.terms[0]];
        for (size_t i = 0; i < q.ops.size(); ++i) {
            const auto& right = postings[q.terms[i + 1]];
            if (q.ops[i] == 0) result = set_and(result, right);
            else if (q.ops[i] == 1) result = set_or(result, right);
            else result = set_not(result, right);
        }
        append_result(output, result);
    }
    std::ofstream out("/out/results.txt", std::ios::binary);
    out.write(output.data(), static_cast<std::streamsize>(output.size()));
    return out ? 0 : 2;
}
