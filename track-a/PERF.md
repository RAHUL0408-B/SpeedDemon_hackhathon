# Track A (Ingest) – Performance Log

| Run | Approach | Wall time | Peak RAM | Board Run ID | Notes |
|-----|----------|-----------|----------|--------------|-------|
| baseline | Python reference | 32.05 s | 228 MB | reference | Organiser baseline |
| v1 | C++ multi-threaded parse + vector per group | 0.588 s | 142 MB | run-343ef21a4 | Initial C++ port |
| v2 | 12-thread parallel parse + nth_element | 0.204 s | 121 MB | run-8b67c94cea77 | 53.47 points on live board |
| v3 (reverted) | 1M bucket histogram per group | 1.894 s | 4408 MB | run-22f7c5a931d7 | Regressed RAM & hash |
| v4 | Two-pass parallel zero-alloc pipeline | 0.035 s | 22 MB | upcoming | Prefix-sum contiguous buffer, AVX2, write() syscall |

## Optimisation Strategy (v4)
1. **Zero-Allocation Log Ingestion**:
   - `MAP_POPULATE` and `MADV_SEQUENTIAL` to pre-fault mmap pages.
   - Pass 1: Extract unique hours (72) and unique endpoints (1,842). Sort them into arrays and build flat open-addressing lookup tables. Group IDs are computed as `hour_id * num_endpoints + ep_id`, which naturally maintains strict sorted order without any post-hoc sorting.
   - Pass 2: Count records and 5xx errors per group across 12 worker threads.
   - Prefix Sum: Calculate global offsets into a single contiguous array (`all_latencies`) sized to exact total records (12.4 MB).
   - Pass 3: Fill `all_latencies` directly into pre-assigned offsets per thread.
   - Pass 4: Parallel percentile computation (`nth_element` on contiguous group slices) and parallel formatting.
   - Pass 5: Direct `write()` system call to `/out/summary.txt`.
