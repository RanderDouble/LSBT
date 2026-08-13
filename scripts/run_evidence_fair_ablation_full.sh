#!/usr/bin/env bash
# Shell translation of scripts/run_evidence_fair_ablation_full.ps1.
#
# Usage: ./scripts/run_evidence_fair_ablation_full.sh [--quick]
#            [--adaptive-only] [--s3-adaptive-only] [--cold-ablation-only]
set -euo pipefail

quick=false
adaptive_only=false
s3_adaptive_only=false
cold_ablation_only=false

usage() {
  cat <<'EOF'
Usage: run_evidence_fair_ablation_full.sh [options]

  --quick               limit to the first 3 traces per workload
  --adaptive-only       PRD adaptive ghost/lease controls only
  --s3-adaptive-only    S3FIFO adaptive ghost/lease controls only
  --cold-ablation-only  PRD cold-path depth/FIFO ablations only
EOF
}

while (($#)); do
  case "$1" in
    -Quick|--quick) quick=true ;;
    -AdaptiveOnly|--adaptive-only) adaptive_only=true ;;
    -S3AdaptiveOnly|--s3-adaptive-only) s3_adaptive_only=true ;;
    -ColdAblationOnly|--cold-ablation-only) cold_ablation_only=true ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
  shift
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
workspace_root="$(cd "$root/../.." && pwd)"
ratios=(0.005 0.01 0.02 0.05 0.10 0.20)

csv_header='workload,trace,algorithm,capacity_ratio,capacity,requests,hits,misses,hit_ratio,elapsed_ms'

# Run the executable and print the first CSV data record (the PowerShell
# original takes ConvertFrom-Csv | Select-Object -First 1).
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

# Rebuild one row in the schema expected by the plotting scripts.
emit_row() { # workload trace label ratio csv_line
  local workload=$1 trace=$2 label=$3 ratio=$4 line=$5
  local alg cap req hits miss hr ms
  IFS=',' read -r alg cap req hits miss hr ms _ <<<"$line"
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$workload" "$trace" "$label" "$ratio" "$cap" "$req" "$hits" "$miss" "$hr" "$ms"
}

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

run_raw() {
  local exe="$root/ablation_path_runner"
  local limit=0
  [[ $quick == true ]] && limit=3

  local -a cases=()
  if [[ $adaptive_only == true ]]; then
    cases=(
      "PRD-adaptive-ghost@--algorithm@lsbt_ghost_hot_turnover8_adaptive_ghost"
      "PRD-adaptive-lease@--algorithm@lsbt_ghost_hot_turnover8_adaptive_lease"
      "PRD-adaptive-ghost-lease@--algorithm@lsbt_ghost_hot_turnover8_adaptive_ghost_lease"
    )
  elif [[ $s3_adaptive_only == true ]]; then
    cases=(
      "S3-adaptive-ghost@--algorithm@s3fifo_adaptive_ghost"
      "S3-adaptive-lease@--algorithm@s3fifo_adaptive_lease"
      "S3-adaptive-ghost-lease@--algorithm@s3fifo_adaptive_ghost_lease"
    )
  elif [[ $cold_ablation_only == true ]]; then
    cases=(
      "PRD-adaptive-null-depth@--algorithm@lsbt_ghost_hot_turnover8_adaptive_ghost_lease_null_depth"
      "PRD-adaptive-pure-depth@--algorithm@lsbt_ghost_hot_turnover8_adaptive_ghost_lease_pure_depth"
      "PRD-adaptive-cold-fifo@--algorithm@lsbt_ghost_hot_turnover8_adaptive_ghost_lease_cold_fifo"
    )
  else
    cases=(
      "S3FIFO-g09@--algorithm@s3fifo"
      "S3FIFO-g8@--algorithm@s3fifo_ghost8"
      "LSBT-discrete@--algorithm@lsbt_discrete"
      "LSBT-consecutive@--algorithm@lsbt_consecutive"
      "PRD@--algorithm@prd"
    )
  fi

  local dir="$workspace_root/raw_ethereum_2024"
  local trace ratio case_entry parts row
  while IFS= read -r trace; do
    echo "raw: $(basename "$trace")"
    for ratio in "${ratios[@]}"; do
      for case_entry in "${cases[@]}"; do
        IFS='@' read -r -a parts <<<"$case_entry"
        if ! row="$(first_csv_row "$exe" --trace "$trace" --capacity-ratio "$ratio" "${parts[@]:1}")"; then
          exit 1
        fi
        emit_row raw "$(basename "$trace")" "${parts[0]}" "$ratio" "$row"
      done
    done
  done < <(list_traces "$dir" "$limit")
}

run_block() {
  local exe="$root/ablation_runner"
  local limit=0
  [[ $quick == true ]] && limit=3

  local -a cases=()
  if [[ $adaptive_only == true ]]; then
    cases=(
      "PRD-adaptive-ghost@--algorithm@PRDAdaptiveGhost@--layer-protection@false"
      "PRD-adaptive-lease@--algorithm@PRDAdaptiveLease@--layer-protection@false"
      "PRD-adaptive-ghost-lease@--algorithm@PRD@--layer-protection@false"
    )
  elif [[ $s3_adaptive_only == true ]]; then
    cases=(
      "S3-adaptive-ghost@--algorithm@S3FIFOAdaptiveGhost"
      "S3-adaptive-lease@--algorithm@S3FIFOAdaptiveLease"
      "S3-adaptive-ghost-lease@--algorithm@S3FIFOAdaptiveGhostLease"
    )
  elif [[ $cold_ablation_only == true ]]; then
    cases=(
      "PRD-adaptive-null-depth@--algorithm@PRDNullDepth@--layer-protection@false"
      "PRD-adaptive-pure-depth@--algorithm@PRDPureDepth@--layer-protection@false"
      "PRD-adaptive-cold-fifo@--algorithm@PRDColdFifo@--layer-protection@false"
    )
  else
    cases=(
      "S3FIFO-g09@--algorithm@S3FIFO"
      "S3FIFO-g8@--algorithm@S3FIFOGhost8"
      "LSBT-discrete@--algorithm@LSBTdiscrete"
      "LSBT-consecutive@--algorithm@LSBTconsecutive"
      "PRD@--algorithm@PRD"
    )
  fi

  local dir="$root/workload/lsbt_ethereum_2024"
  local trace ratio case_entry parts row
  while IFS= read -r trace; do
    echo "block: $(basename "$trace")"
    for ratio in "${ratios[@]}"; do
      for case_entry in "${cases[@]}"; do
        IFS='@' read -r -a parts <<<"$case_entry"
        if ! row="$(first_csv_row "$exe" --trace "$trace" --capacity-ratio "$ratio" "${parts[@]:1}")"; then
          exit 1
        fi
        emit_row block "$(basename "$trace")" "${parts[0]}" "$ratio" "$row"
      done
    done
  done < <(list_traces "$dir" "$limit")
}

tag="evidence_fair_ablation"
[[ $cold_ablation_only == true ]] && tag="adaptive_cold_ablation"
[[ $s3_adaptive_only == true ]] && tag="s3_adaptive_controls"
[[ $adaptive_only == true ]] && tag="adaptive_ghost_start8"
suffix="full"
[[ $quick == true ]] && suffix="quick"
output="$root/results/${tag}_${suffix}.csv"

mkdir -p "$root/results"
{
  printf '%s\n' "$csv_header"
  run_raw
  run_block
} > "$output"

echo "Wrote $output"
