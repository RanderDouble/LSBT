# PRD Cache Replacement

> 中文版：[README.zh-CN.md](README.zh-CN.md)

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
| `scripts/` | Build, sweep, audit, fetch, and plotting scripts (PowerShell and bash variants). |
| `tools/get_trace.py` | Fetches Ethereum trace slices from the AWS Public Blockchain dataset. |
| `tools/convert_to_lsbt.py` | Converts raw Ethereum transaction CSV into the LSBT block CSV format. |
| `tools/twitter_bin_to_block.cpp` | Converts OSDI'20 Twitter binary traces to the block CSV format. |
| `tools/download_range.py` | Parallel range downloader for slow HTTP mirrors. |
| `tools/hierarchy_trace_convert.cpp` | Converts Raw/LSBT/Twitter/CloudPhysics CSV inputs to fixed-64B logical-node CSV. |
| `workload/` | Local workload location used by block scripts. |
| `results/README.md` | Result-directory convention. Generated outputs are intentionally ignored by Git. |

Only source code and scripts belong in version control. Trace data, generated
CSV/figures, and executable files are local artifacts and are excluded by
`.gitignore`.

## Requirements

- Windows PowerShell 5+.
- A C++17 compiler available as `g++` (MinGW-w64 works for the native runners).
- Python 3 with `pandas` and `matplotlib` for plotting scripts.

The default build scripts emit executables in the repository root.

On Linux/macOS the same workflow is available as a bash translation of every
PowerShell script (`scripts/*.sh`). The shell scripts require bash 4+, use the
same `g++` toolchain, and build extension-less executables in the repository
root (`runner`, `ablation_runner`, ...).

## Shell usage (Linux/macOS)

Each PowerShell script has a bash counterpart with the same base name. Flags
use `--` instead of `-` and binaries are built without the `.exe` suffix:

```bash
./scripts/build.sh                    # release; or build.sh debug
./scripts/build_ablation.sh           # builds both ablation runners + self-tests
./scripts/build_s3_audit.sh

./runner --trace workload/lsbt_ethereum_2024/lsbt_2024-01-01_0w_5w.csv --capacity-ratio 0.01
./runner --trace workload/lsbt_ethereum_2024/lsbt_2024-01-01_0w_5w.csv --cache-size 10000

./scripts/run_evidence_fair_ablation_full.sh --quick
./scripts/run_evidence_fair_ablation_full.sh --adaptive-only
./scripts/run_evidence_fair_ablation_full.sh --s3-adaptive-only
./scripts/run_evidence_fair_ablation_full.sh --cold-ablation-only

./scripts/run_experiments.sh --workload-dir DIR --ratios 0.01,0.05,0.10,0.20 --output results/raw.csv
./scripts/run_lease_sweep.sh [TRACE.csv]
./scripts/run_granularity_sweep.sh [TRACE.csv]
./scripts/run_s3_main_cycle_audit.sh [TRACE.csv]
./scripts/run_s3_turnover_audit_full.sh --quick
./scripts/summarize.sh --input results/raw.csv --output results/summary.csv

python scripts/plot_prd_lsbt_s3_full.py
python scripts/plot_adaptive_lease_full.py
```

The plotting scripts are Python and run unchanged.

Notes:

- The shell scripts expect the same external layout as the PowerShell
  versions: raw Ethereum traces adjacent two directories above this
  repository (used by `run_evidence_fair_ablation_full.sh` and
  `run_s3_turnover_audit_full.sh`), and a separately built libCacheSim tree
  three directories above it (libcachesim scripts).
- `run_experiments.sh` invokes the current `runner` interface directly. The
  PowerShell original passes `--algorithm both`, which the current
  `runner.cpp` no longer accepts, so the shell version omits that flag.

## Expanded dataset experiments

The `test` branch adds two dataset expansions:

1. **More Ethereum 2024 slices.** `./scripts/fetch_ethereum_expand.sh` fetches
   additional dates from the AWS Public Blockchain dataset
   (`s3://aws-public-blockchain/v1.0/eth/traces/`) and converts them into the
   same `workload/lsbt_ethereum_2024/` layout. Four new dates were added
   (2024-02-15, 2024-06-20, 2024-08-15, 2024-10-15), each with five 50k-row
   windows (`0w_5w` ... `20w_25w`), bringing the workload from 27 to 47
   slices. `workload/manifest.csv` records size and SHA-256 for every slice.
2. **Twitter (OSDI'20) sample100 clusters** used by the S3FIFO paper:

   ```bash
   ./scripts/fetch_twitter.sh          # clusters 12 24 47 48 52 (gitignored)
   ./scripts/prepare_twitter.sh        # decompress + convert to block CSV
   ./scripts/run_expanded_ablation.sh  # PRD vs LSBT vs S3FIFO on both workloads
   python scripts/plot_expanded.py     # hit-ratio figure into results/
   ```

   `prepare_twitter.sh` keeps the first 10 million requests per cluster by
   default (`--max-requests N`) and emits flat `version,depth,key` rows
   (`depth=1`, logical version per timestamp change).

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
