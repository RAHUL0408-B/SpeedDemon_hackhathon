# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

The graph is parsed twice from a memory map into compact CSR adjacency arrays. Queries are deduplicated, then independent shortest-path searches run in parallel. Each worker has stamped distance storage, avoiding an O(nodes) reset per query, and a monotone radix heap for non-negative 64-bit distances. This preserves zero-weight edges, parallel edges, directionality, and paths longer than `uint32_t`.

## Dependencies

None.

## With another 24 hours

Profile the scoring graph's query distribution and evaluate a custom road-network preprocessing index while preserving holdout correctness.
