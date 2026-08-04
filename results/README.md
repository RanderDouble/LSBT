# Results

- `raw.csv`: one row per trace, capacity ratio, and implementation.
- `summary.csv`: request-weighted aggregate hit ratios and total policy time.
- `conclusions.md`: interpretation constrained to the measured experiment.

The timing excludes CSV parsing and measures only cache-policy replay. It does
not model storage I/O.

