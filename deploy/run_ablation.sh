#!/usr/bin/env bash
# Run the 8-policy comparison over every trace found under workloads/.
#
# Usage: ./run_ablation.sh [--quick]
#   --quick  limit to the first 3 traces per workload
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXE="$DIR/ablation_runner"

if [[ ! -x "$EXE" ]]; then
  echo "building runner..."
  g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic \
    "$DIR/code/ablation_runner.cpp" -o "$EXE"
fi

QUICK=false
while (($#)); do
  case "$1" in
    -Quick|--quick) QUICK=true ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
  shift
done

ETH_DIR="$DIR/workloads/ethereum/block"
TW_DIR="$DIR/workloads/twitter/csv"
ETH_RATIOS=(0.005 0.01 0.02 0.05 0.10 0.20)
TW_RATIOS=(0.01 0.05 0.10 0.20)

CASES=(
  "S3FIFO-g09@--algorithm@S3FIFO"
  "S3FIFO-g8@--algorithm@S3FIFOGhost8"
  "LSBT-discrete@--algorithm@LSBTdiscrete"
  "LSBT-consecutive@--algorithm@LSBTconsecutive"
  "PRD@--algorithm@PRD"
  "LSBTSimple@--algorithm@LSBTSimple"
  "TwoTimescale@--algorithm@TwoTimescale"
  "LRU@--algorithm@LRU"
)

HEADER='workload,trace,algorithm,capacity_ratio,capacity,requests,hits,misses,hit_ratio,elapsed_ms'

first_csv_row() {
  local exe=$1; shift
  local out
  if ! out="$("$exe" "$@")"; then echo "runner failed: $exe $*" >&2; return 1; fi
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
    if (( limit > 0 )); then printf '%s\n' "$files" | head -n "$limit"
    else printf '%s\n' "$files"; fi
  fi
}

run_set() {
  local workload=$1 dir=$2; shift 2
  local -a ratios=("$@")
  local limit=0; [[ $QUICK == true ]] && limit=3
  local trace case_entry parts ratio row
  while IFS= read -r trace; do
    echo "$workload: $(basename "$trace")" >&2
    for ratio in "${ratios[@]}"; do
      for case_entry in "${CASES[@]}"; do
        IFS='@' read -r -a parts <<<"$case_entry"
        if ! row="$(first_csv_row "$EXE" --trace "$trace" --capacity-ratio "$ratio" "${parts[@]:1}")"; then
          exit 1
        fi
        emit_row "$workload" "$(basename "$trace")" "${parts[0]}" "$ratio" "$row"
      done
    done
  done < <(list_traces "$dir" "$limit")
}

SUFFIX="full"; [[ $QUICK == true ]] && SUFFIX="quick"
mkdir -p "$DIR/results"
{
  printf '%s\n' "$HEADER"
  run_set ethereum "$ETH_DIR" "${ETH_RATIOS[@]}"
  run_set twitter "$TW_DIR" "${TW_RATIOS[@]}"
} > "$DIR/results/expanded_ablation_${SUFFIX}.csv"

echo "Wrote $DIR/results/expanded_ablation_${SUFFIX}.csv"
