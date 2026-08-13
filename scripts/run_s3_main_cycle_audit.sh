#!/usr/bin/env bash
# Shell translation of scripts/run_s3_main_cycle_audit.ps1.
#
# Usage: ./scripts/run_s3_main_cycle_audit.sh [TRACE.csv]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"

trace_rel="${1:-workload/lsbt_ethereum_2024/lsbt_2024-01-01_0w_5w.csv}"
trace_path="$root/$trace_rel"

runner="$root/ablation_runner"
if [[ ! -x "$runner" ]]; then
  echo "Build ablation_runner first (./scripts/build_ablation.sh)." >&2
  exit 1
fi

ratios=(0.005 0.01 0.02 0.05 0.10 0.20)
out="$root/results/s3_main_cycle_audit.csv"

mkdir -p "$root/results"
{
  printf '%s\n' \
    'algorithm,capacity,requests,hits,misses,hit_ratio,elapsed_ms,evictions,s3_main_evictions,s3_main_eviction_reaccesses,s3_main_eviction_requalifications'
  for ratio in "${ratios[@]}"; do
    "$runner" --trace "$trace_path" --capacity-ratio "$ratio" \
      --algorithm S3FIFOMainCycleAudit | awk 'NR==2 { print; exit }'
  done
} > "$out"

echo "Wrote $out"
