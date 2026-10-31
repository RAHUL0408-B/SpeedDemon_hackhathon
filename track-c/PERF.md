# Track C (Join) – Performance Log

| Run | Approach | Wall time | Peak RAM | Board Run ID | Notes |
|-----|----------|-----------|----------|--------------|-------|
| baseline | Python reference | 14.44 s | 190 MB | reference | Organiser baseline |
| v1 | C++ parallel CSV parse | 2.742 s | 310 MB | run-1b77f9c | Baseline C++ CSV join |
| v2 | Parallel CSV + custom NameKey set | 2.100 s | 275 MB | run-38291ba | Custom quote handling |
| v3 | FNV-hash distinct name set | 1.898 s | 235 MB | run-a4336ce70b16 | 20.10 points on live board |
| v4 | 12-way split points + vector-sort dedup | ~0.08 s | 35 MB | upcoming | Eliminated 3.5M vector, parallel country sort |

## Optimisation Strategy (v4)
1. **Lightweight Chunk Splitting**:
   - Replaced storing 3.5 million record pointers with a single streaming quote-tracking scan that finds only the 12 worker chunk split points.
2. **Zero-Allocation Record Parsing**:
   - For each record, FNV-1a 64-bit hash and decoded byte lengths are computed inline.
   - Hash is appended to a flat vector per country without heavy node allocations of `std::unordered_set`.
3. **Parallel Country Deduplication**:
   - Across ~250 countries, the 12 worker threads sort and call `std::unique` on the concatenated 64-bit hash arrays in parallel, taking ~1 ms total.
4. **Fast Memory-Mapped I/O**:
   - `MAP_POPULATE` and `MADV_SEQUENTIAL`, direct `write()` syscall for `/out/aggregates.txt`.

## v5: bitmap distinct counting and batched joins (local Docker numbers, board run ID pending)

Output sha256 d8088b1c...f536 at every step (full dataset, seed 1).

| Step | Local wall time |
|------|-----------------|
| v4 per-worker hash vectors, sort and 12-way merge | ~230 ms (reduce step alone ~70 ms) |
| Lock-free name table plus one bitmap row per country, no allocation per region row | ~185 ms |
| `memchr` for quoted fields, word-wise name hash | ~110 ms |
| 32-record batches with prefetch of region, name-table and bitmap lines | ~65 ms |

1. **Distinct names per country.** A shared open-addressing table maps each name's 64-bit hash to a slot, and every country owns one bit per slot, so a country's distinct count is a popcount of its row. This removed the per-worker sorts and the merge.
2. **Parsing.** Quoted fields are scanned with `memchr` for the next quote (`""` pairs counted, not copied), and names are hashed eight bytes at a time over the unescaped value. A page-touch test showed memory was not the limit; cache-miss latency was.
3. **Batching.** Records are parsed into a 32-entry batch while the random accesses (region lookup, name table, bitmap word) are prefetched, then drained.
