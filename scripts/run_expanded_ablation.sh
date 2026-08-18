#!/usr/bin/env bash
# Run the ablation suite (PRD, LSBTdiscrete, LSBTconsecutive, S3FIFO,
# S3FIFOGhost8) over the expanded Ethereum block traces and the Twitter
# flat traces, and collect per-trace hit-ratio rows.
#
# Usage: ./scripts/run_expanded_ablation.sh [--quick] [--workloads LIST]
set -euo pipefail

quick=false
workloads=()
while (($#)); do
  case "$1" in
    -Quick|--quick) quick=true; shift ;;
    --workloads) IFS=',' read -r -a workloads <<<"$2"; shift 2 ;;
    -h|--help) echo "Usage: run_expanded_ablation.sh [--quick] [--workloads LIST]"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done
if (( ${#workloads[@]} == 0 )); then
  workloads=(ethereum twitter)
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
exe="$root/ablation_runner"

if [[ ! -x "$exe" ]]; then
  echo "Build ablation_runner first (./scripts/build_ablation.sh)." >&2
  exit 1
fi

eth_dir="$root/workload/lsbt_ethereum_2024"
tw_dir="$root/workload/twitter_osdi20/csv"
eth_ratios=(0.005 0.01 0.02 0.05 0.10 0.20)
tw_ratios=(0.01 0.05 0.10 0.20)

cases=(
  "S3FIFO-g09@--algorithm@S3FIFO"
  "S3FIFO-g8@--algorithm@S3FIFOGhost8"
  "LSBT-discrete@--algorithm@LSBTdiscrete"
  "LSBT-consecutive@--algorithm@LSBTconsecutive"
  "PRD@--algorithm@PRD"
  "LSBTSimple@--algorithm@LSBTSimple"
  "TwoTimescale@--algorithm@TwoTimescale"
  "LRU@--algorithm@LRU"
)

csv_header='workload,trace,algorithm,capacity_ratio,capacity,requests,hits,misses,hit_ratio,elapsed_ms'

first_csv_row() {
  local exe=$1
  shift
  local out
  if ! out="$("$exe" "$@")"; then
    echo "runner failed: $exe $*" >&2
    return 1
  fi
  printf '%s\n' "$out" | awk 'NR==2 { print; exit }'
}

emit_row() {
  local workload=$1 trace=$2 label=$3 ratio=$4 line=$5
  local alg cap req hits miss hr ms
  IFS=',' read -r alg cap req hits miss hr ms _ <<<"$line"
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$workload" "$trace" "$label" "$ratio" "$cap" "$req" "$hits" "$miss" "$hr" "$ms"
}

list_traces() {
  local dir=$1 limit=${2:-0}
  local files=""
  if [[ -d "$dir" ]]; then
    files="$(find "$dir" -maxdepth 1 -type f -name '*.csv' | sort)"
  else
    echo "warning: trace dir not found: $dir" >&2
  fi
  if [[ -n $files ]]; then
    if (( limit > 0 )); then
      printf '%s\n' "$files" | head -n "$limit"
    else
      printf '%s\n' "$files"
    fi
  fi
}

run_set() { # workload dir ratios...
  local workload=$1 dir=$2
  shift 2
  local -a ratios=("$@")
  local limit=0
  [[ $quick == true ]] && limit=3
  local trace case_entry parts ratio row
  while IFS= read -r trace; do
    echo "$workload: $(basename "$trace")" >&2
    for ratio in "${ratios[@]}"; do
      for case_entry in "${cases[@]}"; do
        IFS='@' read -r -a parts <<<"$case_entry"
        if ! row="$(first_csv_row "$exe" --trace "$trace" --capacity-ratio "$ratio" "${parts[@]:1}")"; then
          exit 1
        fi
        emit_row "$workload" "$(basename "$trace")" "${parts[0]}" "$ratio" "$row"
      done
    done
  done < <(list_traces "$dir" "$limit")
}

suffix="full"
[[ $quick == true ]] && suffix="quick"
output="$root/results/expanded_ablation_${suffix}.csv"

mkdir -p "$root/results"
{
  printf '%s\n' "$csv_header"
  for workload in "${workloads[@]}"; do
    case "$workload" in
      ethereum) run_set ethereum "$eth_dir" "${eth_ratios[@]}" ;;
      twitter)  run_set twitter  "$tw_dir"  "${tw_ratios[@]}" ;;
      *) echo "unknown workload: $workload" >&2; exit 1 ;;
    esac
  done
} > "$output"

echo "Wrote $output"
