# Results

- `raw.csv`: one row per trace, capacity ratio, and implementation.
- `summary.csv`: request-weighted aggregate hit ratios and total policy time.
- `conclusions.md`: interpretation constrained to the measured experiment.

The timing excludes CSV parsing and measures only cache-policy replay. It does
not model storage I/O.

## Expanded dataset runs

The `test` branch adds an expanded experiment over 52 traces:

- `expanded_ablation_full.csv`: one row per (trace, capacity ratio, policy) for
  the 47 Ethereum block slices and 5 Twitter (OSDI'20) clusters.
- `expanded_hit_ratio.png`: request-weighted hit-ratio curves per workload.
- `expanded_summary.md`: aggregate hit ratios by workload and policy.

Workloads: `workload/lsbt_ethereum_2024/` (47 slices) and
`workload/twitter_osdi20/csv/` (5 clusters, 10M requests each, gitignored).
Reproduce with `scripts/run_expanded_ablation.sh` and
`python scripts/plot_expanded.py`.
