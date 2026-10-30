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
