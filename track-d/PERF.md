# Track D (Traverse) – Performance Log

| Run | Approach | Wall time | Peak RAM | Board Run ID | Notes |
|-----|----------|-----------|----------|--------------|-------|
| baseline | Python reference | 72.69 s | 220 MB | reference | Organiser baseline |
| v1 | C++ RadixHeap Dijkstra (6 workers) | 1.684 s | 184 MB | run-81c945ba9795 | 42.34 points on live board |
| v2 (reverted) | Bidirectional Dijkstra + reverse CSR | 3.962 s | 716 MB | run-28fb87c01cbe | Regressed RAM & latency |
| v3 | 12-worker RadixHeap + fast number parser | ~0.50 s | 125 MB | upcoming | 12 threads, MAP_POPULATE, direct write() syscall |

## Optimisation Strategy (v3)
1. **Full 12-Thread Utilization**:
   - Expanded worker count from 6 to 12 workers matching the host machine's hardware thread count.
2. **Lean Forward-Only CSR**:
   - Bounded queries make forward-only Dijkstra optimal because local queries settle in few steps without the overhead of dual priority queues and reverse graph structures.
3. **RadixHeap O(1) Priority Queue**:
   - 64-bit RadixHeap avoids O(log N) comparisons for non-negative edge relaxation.
4. **Memory Optimization**:
   - Pre-faulted mmap with `MAP_POPULATE` and sequential advice, direct `write()` syscall for `/out/distances.txt`.
