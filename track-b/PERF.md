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

## v5: tokenizer, batching, common-term bitmaps (local Docker numbers, board run ID pending)

Profile of v4 on the full dataset (250k docs, 50k queries): parse queries 42 ms, scan 280 ms, scatter 120-200 ms, dense build 55 ms, queries 230 ms, about 800 ms in total. Output sha256 f86973c2...0927 at every step.

| Step | Local wall time |
|------|-----------------|
| v4 | ~800 ms |
| AVX2 tokenizer, word-wise hash, batched and prefetched term lookups | ~600 ms |
| SIMD phrase verification, parallel dense build, prefetched scatter, uninitialised postings array | ~350 ms |
| Common terms found by sampling go straight to bitmaps in the scan | ~320-350 ms |

1. **Scan.** Token boundaries come from a 32-byte classify and `movemask`. A token of up to eight bytes hashes in one multiply and maps one-to-one onto its hash. Candidates that pass the Bloom filter are resolved 64 at a time: prefetch the table slots, look them up, prefetch the per-term state, then update. That turned per-token dependent cache misses into overlapped ones. Pre-faulting the mmap with `MADV_POPULATE_READ` changed nothing (measured), which showed the scan was cache-miss-bound, not fault-bound.
2. **Phrase queries** were 80% of query time. The verifier now finds the anchor word with a two-byte SIMD search and runs the exact neighbour checks only at hits.
3. **Common terms.** 714 terms hold 54% of the postings. The first 800 documents give a document-frequency sample; the 1,202 terms seen in at least 8 of them get a bitmap row that the scan sets directly through a small cache-resident table. They never enter the postings lists. A wrong guess only costs speed. Result sums over a bitmap use six weighted popcounts per word instead of visiting each bit.
4. Tried and dropped: `MADV_POPULATE_READ` on the docs mapping (no gain).

## v6: where the time and memory go (local Docker; board run ID pending)

- Memory by phase (cgroup): 14 MB after parsing queries, 94 MB after the scan (38 MB bitmaps, ~38 MB token lists), 132 MB peak in the scatter, 100 MB once the token lists are freed. The token lists plus the postings array are inherent to the counting-sort design, so the floor here is about 100 MB.
- Cycle counters in the scan showed ~100 cycles per table lookup and ~45% of the time outside lookups. A per-thread token cache (42% hit rate at 2K entries) and a single-lookup flush were neutral locally. Cache sizes of 16K and 64K entries were slower. The scan is bounded by shared-memory contention across 12 threads, not per-token compute.
- Exit: the two big arrays (postings and bitmaps, ~80 MB) are now released with `madvise(MADV_DONTNEED)` from all 12 threads after the answer is written, then `_exit(0)`. Median wall time dropped by about 22 ms in two interleaved comparisons.
