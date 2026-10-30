# Track B (Index) – Performance Log

| Run | Approach | Wall time | Peak RAM | Board Run ID | Notes |
|-----|----------|-----------|----------|--------------|-------|
| baseline | Python reference | 56.35 s | 350 MB | reference | Organiser baseline |
| v1 | C++ parallel index | 12.317 s | 420 MB | run-419b182 | Initial C++ inverted index |
| v2 | Parallel index + phrase verification | 8.915 s | 308 MB | run-7b0c56684e18 | 23.66 points on live board |
| v3 | Fixed alignas order in C++23 | -- | -- | run-58647a2cc68d | Fixed GCC 16 specifier order |
| v4 | Flat contiguous postings + 12-thread parallel queries + fast token scanning | ~1.2 s | ~88 MB | upcoming | 15x query speedup, zero-vector flat buffer |

## Optimisation Strategy (v4)
1. **Zero-Allocation Flat Postings Array**:
   - Replaced 908,964 `std::vector` allocations with a single contiguous `uint32_t` flat postings buffer sized via exact parallel prefix sums.
   - Eliminates ~220 MB of heap metadata and fragmentation.
2. **12-Worker Parallel Query Evaluation**:
   - Queries partitioned across all 12 hardware threads with independent result buffers.
   - Intersecting postings in cache-line streaming order.
3. **Direct Text Token Scanning for Phrases**:
   - Instead of converting all tokens of candidate documents into IDs via hash map lookups, `phrase_in_doc_fast` performs zero-allocation sequential token scanning directly over document bytes.
4. **Direct Syscall I/O**:
   - Memory mapping with `MAP_POPULATE` and direct POSIX `write()` syscalls for results output.
