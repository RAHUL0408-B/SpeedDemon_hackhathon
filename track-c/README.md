# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

The solution memory-maps both CSV inputs. A purpose-built byte parser handles quoted fields, doubled quotes, embedded line feeds, CRLF/LF boundaries, and record-boundary BOMs without decoding or allocating records. Regions use a direct integer lookup table. Country aggregates are updated in one pass, while distinct names are represented by non-owning unescaped byte views whose hash and equality preserve exact CSV semantics.

## Dependencies

None.

## With another 24 hours

Tune the region lookup and distinct-name hash table against the board's cache and memory profile.
