# Track A (Ingest) – Performance Log

| Run | Approach | Wall time | Peak RAM | Board Run ID | Notes |
|-----|----------|-----------|----------|--------------|-------|
| baseline | Python reference | 32.18 s | 290 MB | reference | Organiser baseline |
| v1 | Parallel C++ log ingest | 0.812 s | 350 MB | run-9bc1e1e | Initial C++ rewrite |
| v2 | In-place group reduction | 0.204 s | 121 MB | run-8b67c94cea77 | 53.47 points on live board |
| v3 | Modulo histogram (failed) | 1.894 s | 4.4 GB | run-22f7c5a931d7 | Reverted due to 4.4GB table allocation |
| v4 | Single-pass memory scatter + fast percentiles | ~0.04 s | ~35 MB | upcoming | Single file pass, compact memory scatter, small-count shortcuts |

## Optimisation Strategy (v4)
1. **Single File Pass with Memory Scatter**:
   - Replaced multiple file read passes with a single pass over the memory-mapped log into compact thread-local records.
   - Global prefix sums compute contiguous slice offsets for all groups.
2. **Small-Count Percentile Shortcuts**:
   - Over 45% of groups contain 5 or fewer items.
   - Single elements require zero sorting; pairs require a single swap; lists of $\le 16$ items use direct in-register insertion sort instead of heavy `nth_element` recursive partitioning.
3. **Cache-Aligned Direct Endpoint Hash Table**:
   - Open-addressing hash table with FNV-1a hash matching string lengths and exact bytes without heap allocations.
4. **Direct POSIX Syscalls**:
   - `mmap` with `MAP_POPULATE` and direct POSIX `write()` syscall for output generation.

## v5: order-independent equal-range scan (local Docker numbers, board run ID pending)

| Step | Local wall time | Output sha256 (full, seed 1) |
|------|-----------------|------------------------------|
| v4 margin + binary-search scan | ~120 ms | 495ca096...1cd3 |
| v5 equal byte ranges, AVX2 line parse, per-hour merge | ~65 ms | 495ca096...1cd3 (unchanged) |

1. **Why the rewrite.** The spec says no bound on out-of-order lines is promised. v4 found hour boundaries by binary search and padded every worker with a 3.5 MB margin, which only works while the generator's disorder stays small, and it scanned about a third more bytes than needed. v5 splits the file into 12 equal line-aligned ranges, so it makes no assumption about order. A randomly shuffled copy of the full log gives the same hash.
2. **Line parse.** The timestamp has a fixed width. The method and endpoint ends are found with a 32-byte compare and `ctz`, the endpoint is hashed eight bytes at a time into a 4096-slot table (hash, length and `memcmp` are all checked), and the latency is read as it is reached, so there is no per-line `memchr`.
3. **Merge.** Each worker buckets `(endpoint id, 5xx flag, latency)` by hour. Threads then take one hour at a time, count per endpoint, scatter latencies into one buffer and select the percentiles. Endpoint order is sorted once, not per hour.
4. Failed idea kept for the record: v3's modulo histogram allocated 4.4 GB.
