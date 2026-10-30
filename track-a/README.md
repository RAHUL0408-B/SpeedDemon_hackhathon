# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

The solution memory-maps `requests.log` and parses its fixed ASCII layout directly. It indexes endpoints by `string_view`, computes the UTC-hour bucket without date-library work, and stores only the latency values required for each `(hour, endpoint)` group. At output time it uses three `nth_element` selections for the nearest-rank percentiles instead of sorting every latency list. Groups are emitted in the required hour and bytewise-endpoint order.

## Dependencies

None.

## With another 24 hours

Use board measurements to tune allocation growth and percentile selection on the full scoring CPU.
