#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
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

struct Field { const char* begin; const char* end; bool quoted; };

static uint32_t read_uint(const char*& p) {
    uint32_t n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + static_cast<uint32_t>(*p++ - '0');
    return n;
}

static Field read_field(const char*& p) {
    if (*p != '"') {
        const char* begin = p;
        while (*p != ',' && *p != '\r' && *p != '\n') ++p;
        return {begin, p, false};
    }
    const char* begin = ++p;
    for (;;) {
        if (*p != '"') { ++p; continue; }
        if (p[1] == '"') { p += 2; continue; }
        const char* end = p++;
        return {begin, end, true};
    }
}

static int64_t read_cents(const char*& p) {
    bool negative = *p == '-';
    if (negative) ++p;
    int64_t whole = 0;
    while (*p != '.') whole = whole * 10 + (*p++ - '0');
    ++p;
    const int64_t cents = whole * 100 + (*p++ - '0') * 10 + (*p++ - '0');
    return negative ? -cents : cents;
}

static size_t value_size(Field field) {
    if (!field.quoted) return static_cast<size_t>(field.end - field.begin);
    size_t n = 0;
    for (const char* p = field.begin; p < field.end; ++p, ++n) if (*p == '"') ++p;
    return n;
}

struct NameKey { const char* begin; const char* end; bool quoted; };
static const char* next_byte(const NameKey& key, const char* p, char& byte) {
    byte = *p++;
    if (key.quoted && byte == '"') ++p;
    return p;
}
struct NameHash {
    size_t operator()(const NameKey& key) const {
        uint64_t hash = 1469598103934665603ULL;
        for (const char* p = key.begin; p < key.end;) {
            char byte; p = next_byte(key, p, byte);
            hash ^= static_cast<unsigned char>(byte);
            hash *= 1099511628211ULL;
        }
        return static_cast<size_t>(hash);
    }
};
struct NameEqual {
    bool operator()(const NameKey& a, const NameKey& b) const {
        const char* pa = a.begin; const char* pb = b.begin;
        while (pa < a.end && pb < b.end) {
            char ca, cb; pa = next_byte(a, pa, ca); pb = next_byte(b, pb, cb);
            if (ca != cb) return false;
        }
        return pa == a.end && pb == b.end;
    }
};

struct Aggregate {
    uint64_t count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
    int64_t amount_sum = 0, minimum = 0, maximum = 0;
    std::unordered_set<NameKey, NameHash, NameEqual> names;
    void add(uint32_t id, Field name, Field note, int64_t amount) {
        if (count++ == 0) minimum = maximum = amount;
        else { if (amount < minimum) minimum = amount; if (amount > maximum) maximum = amount; }
        id_sum += id; amount_sum += amount;
        name_bytes += value_size(name); note_bytes += value_size(note);
        names.insert({name.begin, name.end, name.quoted});
    }
};

static uint32_t pack_code(const char* p) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(p[2]));
}
static void append_uint(std::string& out, uint64_t n) {
    char buf[24]; auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n); out.append(buf, p);
}
static void append_cents(std::string& out, int64_t cents) {
    if (cents < 0) { out.push_back('-'); cents = -cents; }
    append_uint(out, static_cast<uint64_t>(cents / 100)); out.push_back('.');
    out.push_back(static_cast<char>('0' + (cents / 10) % 10));
    out.push_back(static_cast<char>('0' + cents % 10));
}

int main() {
    Mapping regions_file("/data/regions.csv");
    std::vector<uint16_t> region_to_country(10'000'000, 0);
    std::unordered_map<uint32_t, uint16_t> country_ids;
    country_ids.reserve(256);
    std::vector<uint32_t> countries;
    const char* p = regions_file.data;
    const char* regions_end = p + regions_file.size;
    while (p < regions_end && *p++ != '\n') {}
    while (p < regions_end) {
        const uint32_t region = read_uint(p);
        ++p;
        const uint32_t code = pack_code(p);
        p += 3;
        while (p < regions_end && *p++ != '\n') {}
        auto it = country_ids.find(code);
        uint16_t id;
        if (it == country_ids.end()) {
            id = static_cast<uint16_t>(countries.size());
            country_ids.emplace(code, id); countries.push_back(code);
        } else id = it->second;
        region_to_country[region] = static_cast<uint16_t>(id + 1);
    }

    std::vector<Aggregate> totals(countries.size() + 1);
    Mapping records_file("/data/records.csv");
    p = records_file.data;
    const char* records_end = p + records_file.size;
    if (records_end - p >= 3 && static_cast<unsigned char>(p[0]) == 0xef && static_cast<unsigned char>(p[1]) == 0xbb && static_cast<unsigned char>(p[2]) == 0xbf) p += 3;
    while (p < records_end && *p++ != '\n') {}
    while (p < records_end) {
        if (records_end - p >= 3 && static_cast<unsigned char>(p[0]) == 0xef && static_cast<unsigned char>(p[1]) == 0xbb && static_cast<unsigned char>(p[2]) == 0xbf) p += 3;
        const uint32_t record_id = read_uint(p); ++p;
        const Field name = read_field(p); ++p;
        const Field note = read_field(p); ++p;
        const uint32_t region = read_uint(p); ++p;
        const int64_t amount = read_cents(p);
        if (*p == '\r') ++p;
        if (p < records_end && *p == '\n') ++p;
        const uint16_t mapped = region < region_to_country.size() ? region_to_country[region] : 0;
        const size_t group = mapped ? static_cast<size_t>(mapped - 1) : countries.size();
        totals[group].add(record_id, name, note, amount);
    }

    std::vector<uint16_t> order;
    for (uint16_t i = 0; i < countries.size(); ++i) if (totals[i].count) order.push_back(i);
    std::sort(order.begin(), order.end(), [&countries](uint16_t a, uint16_t b) { return countries[a] < countries[b]; });
    std::string output; output.reserve(20 * 1024);
    auto write = [&output, &totals, &countries](uint16_t id, bool unmatched) {
        const Aggregate& a = totals[id];
        if (unmatched) output.append("_UNMATCHED");
        else { const uint32_t code = countries[id]; output.push_back(static_cast<char>(code >> 16)); output.push_back(static_cast<char>(code >> 8)); output.push_back(static_cast<char>(code)); }
        output.push_back(' '); append_uint(output, a.count); output.push_back(' '); append_uint(output, a.id_sum); output.push_back(' ');
        append_cents(output, a.amount_sum); output.push_back(' '); append_cents(output, a.minimum); output.push_back(' '); append_cents(output, a.maximum); output.push_back(' ');
        append_uint(output, a.names.size()); output.push_back(' '); append_uint(output, a.name_bytes); output.push_back(' '); append_uint(output, a.note_bytes); output.push_back('\n');
    };
    for (uint16_t id : order) write(id, false);
    if (totals.back().count) write(static_cast<uint16_t>(totals.size() - 1), true);
    std::ofstream out("/out/aggregates.txt", std::ios::binary);
    out.write(output.data(), static_cast<std::streamsize>(output.size()));
    return out ? 0 : 2;
}
