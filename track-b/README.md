# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

Queries are read first. The corpus pass tokenizes bytes exactly as specified and builds postings only for terms that occur in the supplied queries, rather than retaining the full corpus index. Boolean operations are sorted-vector set operations applied strictly left to right. Phrase queries intersect all distinct term postings before re-tokenizing only their candidates; unusually broad phrases are handled together in a single corpus pass anchored on their rarest term. Results are already sorted by document id, so count, id sum, and the first ten ids are produced without a result sort.

## Dependencies

None.

## With another 24 hours

Use board profiles to choose bitmap cutoffs for the very highest-frequency postings and tune the broad-phrase threshold.
