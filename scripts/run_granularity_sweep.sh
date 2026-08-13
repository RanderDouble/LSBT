#!/usr/bin/env bash
# Shell translation of scripts/run_granularity_sweep.ps1.
#
# Usage: ./scripts/run_granularity_sweep.sh [TRACE.csv]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"

exe="$root/s3_audit_runner"
if [[ ! -x "$exe" ]]; then
  echo "Build s3_audit_runner first (./scripts/build_s3_audit.sh)." >&2
  exit 1
fi

trace="${1:-$root/../../raw_ethereum_2024/trace_2024-01-01_0w_5w.csv}"
out="$root/results/granularity_sweep_pilot.csv"

mkdir -p "$root/results"
printf '%s\n' \
  'algorithm,batch_size,capacity_ratio,unique_keys,capacity,requests,hits,misses,hit_ratio' \
  > "$out"
for b in 1 2 4 8 16 32; do
  "$exe" --format raw --trace "$trace" --capacity-ratio 0.01 --batch-size "$b" \
    | tail -n +2 >> "$out"
done

echo "Wrote $out"
