#!/usr/bin/env bash
# Shell translation of scripts/run_s3_turnover_audit_full.ps1.
#
# Usage: ./scripts/run_s3_turnover_audit_full.sh [--quick]
set -euo pipefail

quick=false
while (($#)); do
  case "$1" in
    -Quick|--quick) quick=true ;;
    -h|--help) echo "Usage: run_s3_turnover_audit_full.sh [--quick]"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
workspace="$(cd "$root/../.." && pwd)"

exe="$root/s3_audit_runner"
if [[ ! -x "$exe" ]]; then
  echo "Build s3_audit_runner first (./scripts/build_s3_audit.sh)." >&2
  exit 1
fi

ratios=(0.005 0.01 0.02 0.05 0.10 0.20)
ghosts=(0.9 8.0)
batches=(1)
if [[ ${GRANULARITY_SWEEP:-0} == "1" ]]; then
  batches=(1 2 4 8 16 32)
fi

header='algorithm,batch_size,capacity_ratio,unique_keys,capacity,requests,hits,misses,hit_ratio,main_scans,main_requeues,main_evictions,main_eviction_reaccesses,main_eviction_requalifications,reaccess_rate,requalification_rate,requeue_per_scan,trace'

# List sorted trace files under a directory, optionally limited.
list_traces() { # dir [limit]
  local dir=$1 limit=${2:-0}
  local files
  files="$(find "$dir" -maxdepth 1 -type f -name '*.csv' | sort)"
  if (( limit > 0 )); then
    printf '%s\n' "$files" | head -n "$limit"
  else
    printf '%s\n' "$files"
  fi
}

run_set() { # label dir
  local label=$1 dir=$2
  local limit=0
  [[ $quick == true ]] && limit=2

  local trace trace_name batch ratio ghost row
  while IFS= read -r trace; do
    trace_name="$(basename "$trace")"
    echo "$label: $trace_name"
    for batch in "${batches[@]}"; do
      for ratio in "${ratios[@]}"; do
        for ghost in "${ghosts[@]}"; do
          if ! row="$("$exe" --format "$label" --trace "$trace" --batch-size "$batch" \
                      --capacity-ratio "$ratio" --ghost-ratio "$ghost" | awk 'NR==2 { print; exit }')"; then
            echo "audit failed: $trace_name" >&2
            exit 1
          fi
          printf '%s,%s\n' "$row" "$trace_name" >> "$out"
        done
      done
    done
  done < <(list_traces "$dir" "$limit")
}

tag="full"
[[ $quick == true ]] && tag="quick"
out="$root/results/s3_turnover_audit_${tag}.csv"

mkdir -p "$root/results"
printf '%s\n' "$header" > "$out"

run_set raw "$workspace/raw_ethereum_2024"
run_set block "$workspace/lsbt_ethereum_2024"

echo "Wrote $out"
