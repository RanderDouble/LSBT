#!/usr/bin/env bash
# Shell translation of scripts/run_experiments.ps1.
#
# Usage: ./scripts/run_experiments.sh [--workload-dir DIR] [--ratios R1,R2,...]
#            [--output FILE]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"

workload_dir="$root/workload/lsbt_ethereum_2024"
output="$root/results/raw.csv"
ratios=(0.01 0.05 0.10 0.20)

usage() {
  cat <<'EOF'
Usage: run_experiments.sh [options]

  --workload-dir DIR   directory containing trace CSVs
                       (default: workload/lsbt_ethereum_2024)
  --ratios LIST        comma-separated capacity ratios
                       (default: 0.01,0.05,0.10,0.20)
  --output FILE        destination CSV (default: results/raw.csv)
EOF
}

while (($#)); do
  case "$1" in
    --workload-dir) workload_dir=$2; shift 2 ;;
    --ratios) IFS=',' read -r -a ratios <<<"$2"; shift 2 ;;
    --output) output=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

[[ -x "$root/runner" ]] || "$script_dir/build.sh"

traces=()
while IFS= read -r trace; do
  traces+=("$trace")
done < <(find "$workload_dir" -maxdepth 1 -type f -name '*.csv' ! -name 'manifest.csv' | sort)

if (( ${#traces[@]} == 0 )); then
  echo "no trace CSV files found in $workload_dir" >&2
  exit 1
fi

mkdir -p "$(dirname "$output")"

wrote_header=false
for trace in "${traces[@]}"; do
  for ratio in "${ratios[@]}"; do
    echo "$(basename "$trace"): capacity ratio $ratio"
    out=""
    if ! out="$("$root/runner" --trace "$trace" --capacity-ratio "$ratio")"; then
      echo "runner failed for $trace (ratio $ratio)" >&2
      exit 1
    fi
    if [[ $wrote_header == false ]]; then
      printf '%s\n' "$out" > "$output"
      wrote_header=true
    else
      printf '%s\n' "$out" | tail -n +2 >> "$output"
    fi
  done
done

echo "Raw results: $output"
