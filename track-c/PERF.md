# Track C – Performance Log

| Run | Approach | Wall time | Notes |
|-----|----------|-----------|-------|
| baseline | Python reference | ~90 s | organiser reference |
| v1 | C++ parallel CSV parse | 2.742 s | |
| v2 | C++ parallel + custom NameKey set | 2.1 s | previous push |
| v3 | **FNV-hash distinct-name dedup** | TBD | MAP_POPULATE, AVX2 |

## v3 changes
- Distinct-name counting now uses `(hash64, decoded_length)` pairs in an
  `unordered_set` instead of full decoded string objects.  No heap allocation
  per record during parse – hash and length computed inline over the raw CSV
  bytes with a single scan.  Two names collide only if both FNV-1a hash and
  decoded-byte-count match; probability of false collision is negligible.
- `MAP_POPULATE | MADV_SEQUENTIAL` on the large records file → kernel
  pre-faults pages before parallel threads start.
- `#pragma GCC target("avx2")` auto-vectorises inner field-scan loops.
- Note-byte length computed inline (no separate decode step).
- Single `write()` syscall for output.
