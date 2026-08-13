# PRD Cache Replacement

This repository contains the PRD cache policy, LSBT baselines, an S3FIFO
baseline, and the runners used for controlled comparisons on versioned
Ethereum-derived traces.

PRD separates entries by reuse evidence. New entries are admitted through a
Cold path (optionally ordered by a tree-depth prior); an observed resident or
ghost reuse proves an entry and moves it to Hot. Hot entries have a bounded
turnover lease, so evidence that is not renewed becomes eligible for eviction.
The implementation starts with an 8C ghost directory and can adapt the ghost
directory and lease independently. Here `C` always denotes resident cache
capacity; it is not a version or epoch length.

## Repository layout

| Path | Purpose |
| --- | --- |
| `code/PRD.hpp` | Main PRD implementation only. |
| `code/runner.cpp` | Minimal runner for the main PRD policy. |
| `code/LSBTdiscrete.hpp` | Bucketed/discrete LSBT baseline. |
| `code/LSBTconsecutive.hpp` | Consecutive LSBT baseline. |
| `code/S3FifoBaseline.hpp` | S3FIFO baseline and controlled extensions/audit hooks. |
| `code/PRDAblations.hpp` | Named PRD configurations used only by ablations. |
| `code/ablation_runner.cpp` | Block-trace comparison runner. |
| `code/ablation_path_runner.cpp` | Raw transaction-path comparison runner. |
| `code/lease_sweep_runner.cpp` | Lease sensitivity runner. |
| `code/s3_audit_runner.cpp` | S3FIFO Main eviction/reaccess audit runner. |
| `code/synthetic_lease_runner.cpp` | Synthetic lease stress test. |
| `scripts/` | Build, sweep, audit, and plotting scripts. |
| `tools/hierarchy_trace_convert.cpp` | Converts Raw/LSBT/Twitter/CloudPhysics CSV inputs to fixed-64B logical-node CSV. |
| `workload/` | Small local workload location used by block scripts; traces are not bundled. |
| `results/README.md` | Result-directory convention. Generated outputs are intentionally ignored by Git. |

Only source code and scripts belong in version control. Trace data, generated
CSV/figures, and executable files are local artifacts and are excluded by
`.gitignore`.

## Requirements

- Windows PowerShell 5+.
- A C++17 compiler available as `g++` (MinGW-w64 works for the native runners).
- Python 3 with `pandas` and `matplotlib` for plotting scripts.

The default build scripts emit executables in the repository root.

## Trace formats

The native runners consume CSV with a header and the following records:

```text
version,depth,key
```

- `version` defines a logical epoch. The runner calls `FinishBlock()` once
  when this value changes.
- `depth` is a positive tree depth. It is used by PRD's Cold prior and LSBT.
- `key` is an arbitrary node identifier.

`runner.cpp` and `ablation_runner.cpp` use this format directly. The Raw path
runner uses transaction rows and reconstructs the 20 prefix nodes of each
Ethereum address internally.

## Run the main PRD

Build the minimal runner:

```powershell
cd C:\Users\ROG\Desktop\CacheGhost\lrd_cache\LSBTreplacement
.\scripts\build.ps1
```

Run a block/LSBT CSV at a capacity ratio:

```powershell
.\runner.exe --trace .\workload\lsbt_ethereum_2024\trace.csv --capacity-ratio 0.01
```

Or specify the resident capacity directly:

```powershell
.\runner.exe --trace .\workload\lsbt_ethereum_2024\trace.csv --cache-size 10000
```

The output is one CSV row containing request, hit, miss, hit-ratio, and timing
statistics. Optional arguments are `--max-depth` and `--history-retention`.

## Comparisons and ablations

Build the two comparison runners and run their self-tests:

```powershell
.\scripts\build_ablation.ps1
```

Available comparison policies include `PRD`, `LSBTdiscrete`,
`LSBTconsecutive`, `S3FIFO`, and `S3FIFOGhost8`. The runners also expose
controlled PRD and S3FIFO variants for ghost, lease, and Cold-path ablations;
these are experimental controls, not separate main algorithms.

Run the full Raw/Block sweep (expects local trace directories adjacent to this
repository, as referenced by the script):

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1
```

Useful smaller modes:

```powershell
.\scripts\run_evidence_fair_ablation_full.ps1 -Quick
.\scripts\run_evidence_fair_ablation_full.ps1 -AdaptiveOnly
.\scripts\run_evidence_fair_ablation_full.ps1 -S3AdaptiveOnly
.\scripts\run_evidence_fair_ablation_full.ps1 -ColdAblationOnly
```

Additional focused experiments:

```powershell
.\scripts\run_lease_sweep.ps1
.\scripts\run_granularity_sweep.ps1
.\scripts\build_s3_audit.ps1
.\scripts\run_s3_main_cycle_audit.ps1
.\scripts\run_s3_turnover_audit_full.ps1
```

## Plotting

The plotting scripts read locally generated CSV files under `results/` and
write figures there:

```powershell
python .\scripts\plot_prd_lsbt_s3_full.py
python .\scripts\plot_adaptive_lease_full.py
```

## Optional libCacheSim adapter

`tools/hierarchy_trace_convert.cpp` converts supported source traces into the
CSV form used by the external libCacheSim adapter. Compile it with C++20, for
example in WSL:

```bash
g++ -std=c++20 -O3 tools/hierarchy_trace_convert.cpp -o tools/hierarchy_trace_convert
```

The script `scripts/run_libcachesim_hierarchy.ps1` assumes a separately built,
locally patched libCacheSim tree that provides the `prd-epoch` policy. That
adapter and libCacheSim build directory are deliberately not vendored in this
repository. `scripts/run_twitter_libcachesim_prd.ps1` has the same external
dependency.

## Reproducibility notes

- The cache capacity `C`, ghost capacity (for example `8C`), turnover lease,
  and logical epoch are different quantities.
- A ghost hit is a cache **miss** with retained metadata; it is never counted
  as a resident cache hit.
- Generated results are not committed. Keep the commands, trace source,
  capacity definition, and epoch definition alongside any reported result.
