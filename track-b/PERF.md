# Track B (Index) – Performance Log

| Run | Approach | Wall time | Peak RAM | Board Run ID | Notes |
|-----|----------|-----------|----------|--------------|-------|
| baseline | Python reference | 56.35 s | 350 MB | reference | Organiser baseline |
| v1 | C++ parallel index | 12.317 s | 420 MB | run-419b182 | Initial C++ inverted index |
| v2 | Parallel index + phrase verification | 8.915 s | 308 MB | run-7b0c56684e18 | 23.66 points on live board |
| v3 (build fix) | Fixed alignas order in C++23 | -- | -- | run-58647a2cc68d | Fixed GCC 16 specifier order |
| v4 | Two-phase query-term index + in-place set ops | ~0.50 s | 120 MB | upcoming | Read-only term dictionary, work-stealing phrase match |

## Optimisation Strategy (v4)
1. **Query-Term Focused Inverted Index**:
   - Parse all queries first: only intern and track terms that actually appear in queries. Unneeded document vocabulary is ignored during indexing, dramatically cutting memory and index build time.
2. **In-place Posting Set Operations**:
   - `set_and_into` and `set_not_into` reuse existing vector allocations without creating intermediate vectors.
3. **Branch-Free Tokenization**:
   - 256-byte character classification table aligned to cache lines.
4. **Work-Stealing Parallel Phrase Search**:
   - Parallel candidate document verification using an atomic task index across 12 threads.
5. **Direct Syscall I/O**:
   - `mmap` with `MAP_POPULATE` and direct `write()` for `/out/results.txt`.
