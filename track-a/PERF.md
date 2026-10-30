# Track A – Performance Log

| Run | Approach | Wall time | Notes |
|-----|----------|-----------|-------|
| baseline | Python reference | ~25 s | organiser reference |
| v1 | C++ parallel parse + nth_element | 0.588 s | |
| v2 | C++ parallel + nth_element×3 | 0.450 s | previous push |
| v3 | **Counting-sort histogram O(n)** | TBD | MAP_POPULATE, MADV_SEQUENTIAL, AVX2 |

## v3 changes
- Replace `nth_element` (called 3×) with a counting-sort histogram over
  latency values 0–999 999. One O(n) pass; percentile is a prefix-sum scan
  over 1 M buckets (fits in L2 cache on Falkenstein machine).
- `MAP_POPULATE | MAP_PRIVATE` + `MADV_SEQUENTIAL` so the kernel pre-faults
  all pages before parallel parse starts.
- `#pragma GCC target("avx2")` lets the compiler auto-vectorise inner loops.
- Single `write()` syscall instead of `ofstream` for output.
- Zero heap allocation per log line: endpoints stored as `string_view` into
  the mmap region; groups addressed by flat array index.
