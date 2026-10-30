#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

struct Mapping {
    int fd = -1;
    size_t size = 0;
    const char* data = nullptr;
    explicit Mapping(const char* path) {
        fd = open(path, O_RDONLY);
        struct stat st {};
        if (fd < 0 || fstat(fd, &st) != 0) _exit(2);
        size = static_cast<size_t>(st.st_size);
        if (size) {
            data = static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
            if (data == MAP_FAILED) _exit(2);
        }
    }
    ~Mapping() { if (data && data != MAP_FAILED) munmap(const_cast<char*>(data), size); if (fd >= 0) close(fd); }
};

struct Group {
    std::vector<uint32_t> latency;
    uint32_t errors = 0;
};

static uint32_t parse_u32(const char* p, const char* end) {
    uint32_t out = 0;
    while (p < end) out = out * 10u + static_cast<uint32_t>(*p++ - '0');
    return out;
}

static void append_u64(std::string& out, uint64_t n) {
    char buf[24];
    auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), n);
    out.append(buf, p);
}

int main() {
    Mapping input("/data/requests.log");
    std::unordered_map<std::string_view, int> endpoints;
    endpoints.reserve(2048);
    std::vector<Group> groups;
    std::string hours[72];

    const char* p = input.data;
    const char* finish = input.data + input.size;
    while (p < finish) {
        const char* line = p;
        const char* nl = p;
        while (nl < finish && *nl != '\n') ++nl;
        p = nl + (nl < finish);

        const int day = (line[8] - '0') * 10 + (line[9] - '0');
        const int hour = (line[11] - '0') * 10 + (line[12] - '0');
        const int bucket = (line[6] == '0' ? day - 30 : day + 1) * 24 + hour - 18;
        if (hours[bucket].empty()) hours[bucket].assign(line, 13);

        const char* q = line + 28;                 // timestamp and its following space
        while (*q != ' ') ++q;                      // method
        const char* ep_begin = ++q;
        while (*q != ' ') ++q;
        const std::string_view endpoint(ep_begin, static_cast<size_t>(q - ep_begin));
        auto found = endpoints.find(endpoint);
        int endpoint_id;
        if (found == endpoints.end()) {
            endpoint_id = static_cast<int>(endpoints.size());
            endpoints.emplace(endpoint, endpoint_id);
            groups.resize(static_cast<size_t>(endpoint_id + 1) * 72);
        } else {
            endpoint_id = found->second;
        }
        const char* status = q + 1;
        const char* latency = status + 4;
        Group& g = groups[static_cast<size_t>(endpoint_id) * 72 + bucket];
        g.latency.push_back(parse_u32(latency, nl));
        g.errors += static_cast<uint32_t>(status[0] == '5');
    }

    std::vector<int> order(endpoints.size());
    for (int i = 0; i < static_cast<int>(order.size()); ++i) order[i] = i;
    std::vector<std::string_view> names(order.size());
    for (const auto& [key, id] : endpoints) names[id] = key;
    std::sort(order.begin(), order.end(), [&names](int a, int b) { return names[a] < names[b]; });

    std::string output;
    output.reserve(11 * 1024 * 1024);
    for (int h = 0; h < 72; ++h) {
        for (int id : order) {
            Group& g = groups[static_cast<size_t>(id) * 72 + h];
            const size_t n = g.latency.size();
            if (!n) continue;
            const size_t r50 = (50 * n + 99) / 100 - 1;
            const size_t r95 = (95 * n + 99) / 100 - 1;
            const size_t r99 = (99 * n + 99) / 100 - 1;
            std::nth_element(g.latency.begin(), g.latency.begin() + r50, g.latency.end());
            const uint32_t p50 = g.latency[r50];
            std::nth_element(g.latency.begin(), g.latency.begin() + r95, g.latency.end());
            const uint32_t p95 = g.latency[r95];
            std::nth_element(g.latency.begin(), g.latency.begin() + r99, g.latency.end());
            const uint32_t p99 = g.latency[r99];
            output.append(hours[h]).append(":00:00Z ").append(names[id]).push_back(' ');
            append_u64(output, n); output.push_back(' ');
            append_u64(output, p50); output.push_back(' ');
            append_u64(output, p95); output.push_back(' ');
            append_u64(output, p99); output.push_back(' ');
            append_u64(output, g.errors); output.push_back(' ');
            append_u64(output, static_cast<uint64_t>(g.errors) * 1000000 / n);
            output.push_back('\n');
        }
    }
    std::ofstream out("/out/summary.txt", std::ios::binary);
    out.write(output.data(), static_cast<std::streamsize>(output.size()));
    return out ? 0 : 2;
}
