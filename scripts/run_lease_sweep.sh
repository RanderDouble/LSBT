#!/usr/bin/env bash
# Shell translation of scripts/run_lease_sweep.ps1.
#
# Usage: ./scripts/run_lease_sweep.sh [TRACE.csv]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"

exe="$root/lease_sweep_runner"
if [[ ! -x "$exe" ]]; then
  echo "Build lease_sweep_runner first, e.g.:" >&2
  echo "  g++ -std=c++17 -O3 -DNDEBUG $root/code/lease_sweep_runner.cpp -o $exe" >&2
  exit 1
fi

trace="${1:-$root/../../raw_ethereum_2024/trace_2024-01-01_0w_5w.csv}"
result="$root/results/lease_sweep_pilot.csv"
control="$root/results/lease_adaptive_pilot.csv"

mkdir -p "$root/results"
printf '%s\n' \
  'mode,batch_size,lease_factor,capacity_ratio,capacity,requests,hits,misses,hit_ratio,active_hits,late_reuses,proof_expiries' \
  > "$result"
for lease in 1 2 4 8 16; do
  "$exe" --trace "$trace" --capacity-ratio 0.01 --batch-size 1 --lease-factor "$lease" \
    | tail -n 1 >> "$result"
done
"$exe" --trace "$trace" --capacity-ratio 0.01 --batch-size 1 --lease-factor 8 \
  --adaptive --control-log "$control" | tail -n 1 >> "$result"

echo "Wrote $result and $control"
