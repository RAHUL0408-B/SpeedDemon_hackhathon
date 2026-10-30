# Track D – Performance Log

| Run | Approach | Wall time | Notes |
|-----|----------|-----------|-------|
| baseline | Python reference | ~300 s | organiser reference |
| v1 | C++ RadixHeap Dijkstra | 1.684 s | |
| v2 | C++ RadixHeap + dedup queries | 1.2 s | previous push |
| v3 | **Bidirectional Dijkstra + reverse CSR** | TBD | MAP_POPULATE, AVX2 |

## v3 changes
- **Bidirectional Dijkstra**: builds a reverse adjacency list (CSR) at
  startup. Each query runs forward search from source and backward search
  from target in alternating steps, halving the effective search radius.
  Terminates when the best candidate path ≤ min(forward_top, backward_top).
- Reverse CSR built with a single counting-sort pass (no extra sort).
- Forward and reverse `RadixHeap`s per worker – no heap allocation across
  queries (re-used objects).
- `MAP_POPULATE | MADV_SEQUENTIAL` on graph file.
- Edge arrays freed after CSR construction to reduce peak RSS.
- Single `write()` syscall for output.
