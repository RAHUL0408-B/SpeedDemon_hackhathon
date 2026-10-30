# Track B – Performance Log

| Run | Approach | Wall time | Notes |
|-----|----------|-----------|-------|
| baseline | Python reference | ~120 s | organiser reference |
| v1 | C++ parallel index | 12.317 s | |
| v2 | C++ parallel index + phrase parallel | 8.0 s | previous push |
| v3 | **Two-phase intern + in-place set ops** | TBD | MAP_POPULATE, AVX2 |

## v3 changes
- **Two-phase design**: query terms interned first (single-threaded, O(Q)),
  then document tokens can be looked up with read-only dictionary during indexing.
  No mutex, no atomic – zero contention in the hot path.
- Posting-list merge operations rewritten to **in-place** (`set_and_into`,
  `set_not_into`) to avoid allocating a temporary result vector.
- Character class membership via a 256-byte lookup table → branch-free
  token scanning inner loop.
- Phrase queries evaluated **in parallel** using a work-stealing atomic counter.
- `MAP_POPULATE | MADV_SEQUENTIAL` on both query and document files.
- Single `write()` syscall for output.
