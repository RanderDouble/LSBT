#!/usr/bin/env bash
# Compare LSBT-Simple against pure depth-first eviction and standard baselines.
# Usage: ./scripts/run_dfe_comparison.sh [--quick]
set -euo pipefail

quick=false
if (($#)); then
  [[ $1 == "--quick" ]] || { echo "Usage: $0 [--quick]" >&2; exit 1; }
  quick=true
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
exe="$root/ablation_runner"
trace_dir="$root/workload/lsbt_ethereum_2024"
suffix=full
[[ $quick == true ]] && suffix=quick
output="$root/results/dfe_comparison_${suffix}.csv"
summary="$root/results/dfe_comparison_${suffix}_summary.csv"
lrfu_lambda="${LRFU_LAMBDA:-0.001}"

[[ -x $exe ]] || { echo "Build ablation_runner first (./scripts/build_ablation.sh)." >&2; exit 1; }

ratios=(0.005 0.01 0.02 0.05 0.10 0.20)
cases=(
  "LSBTSimple"
  "DFE"
  "S3FIFO"
  "LRU"
  "LRFU"
  "EpochLRFU"
)

mkdir -p "$root/results"
printf 'trace,algorithm,capacity_ratio,capacity,requests,hits,misses,hit_ratio,elapsed_ms\n' > "$output"

limit=0
[[ $quick == true ]] && limit=3
count=0
while IFS= read -r trace; do
  ((++count))
  echo "[$count] $(basename "$trace")" >&2
  for ratio in "${ratios[@]}"; do
    for entry in "${cases[@]}"; do
      label=${entry%%:*}
      algorithm=${entry##*:}
      [[ $entry == *:* ]] || algorithm=$label
      args=(--trace "$trace" --capacity-ratio "$ratio" --algorithm "$algorithm")
      [[ $algorithm == LRFU || $algorithm == EpochLRFU ]] && args+=(--lrfu-lambda "$lrfu_lambda")
      row=$("$exe" "${args[@]}" | awk 'NR == 2 { print; exit }')
      IFS=',' read -r _ capacity requests hits misses hit_ratio elapsed_ms _ <<< "$row"
      printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$(basename "$trace")" "$label" "$ratio" "$capacity" "$requests" "$hits" "$misses" "$hit_ratio" "$elapsed_ms" >> "$output"
    done
  done
  (( limit == 0 || count < limit )) || break
done < <(find "$trace_dir" -maxdepth 1 -type f -name '*.csv' | sort)

echo "Wrote $output"

awk -F, 'BEGIN { OFS=","; print "algorithm,capacity_ratio,requests,hits,hit_ratio" }
  NR > 1 { k = $2 SUBSEP $3; requests[k] += $5; hits[k] += $6 }
  END { for (k in requests) { split(k, p, SUBSEP); print p[1], p[2], requests[k], hits[k], hits[k] / requests[k] } }' \
  "$output" | {
    IFS= read -r header
    printf '%s\n' "$header"
    sort -t, -k1,1 -k2,2g
  } > "$summary"
echo "Wrote $summary"
MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/matplotlib-pfe}" \
  python3 "$script_dir/plot_dfe_comparison.py" --input "$summary" --raw-input "$output"
