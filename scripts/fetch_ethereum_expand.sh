#!/usr/bin/env bash
# Fetch additional Ethereum 2024 trace slices from the AWS Public Blockchain
# dataset and convert them to the repository's LSBT block CSV format.
#
# Usage: ./scripts/fetch_ethereum_expand.sh [DATE ...]
#
# Default dates: 2024-02-15 2024-06-20 2024-08-15 2024-10-15.
# Each date is fetched once (250k raw transaction rows) and split into five
# 50k-row windows named lsbt_<date>_0w_5w.csv .. lsbt_<date>_20w_25w.csv,
# matching the naming of the existing slices.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
tools_dir="$root/tools"
workload_dir="$root/workload/lsbt_ethereum_2024"

if (($#)); then
  dates=("$@")
else
  dates=(2024-02-15 2024-06-20 2024-08-15 2024-10-15)
fi

rows_per_window=50000
window_count=5
window_starts=(0 5 10 15 20)

mkdir -p "$workload_dir"

for date in "${dates[@]}"; do
  raw="$(mktemp /tmp/eth_raw_${date}_XXXXXX.csv)"
  echo "==> fetching $date (${window_count} windows x ${rows_per_window} rows)"
  python3 "$tools_dir/get_trace.py" --date "$date" --start-row 0 \
    --row-count $((rows_per_window * window_count)) --output "$raw"

  total_rows=$(($(wc -l < "$raw") - 1))
  for i in "${!window_starts[@]}"; do
    start_w=${window_starts[$i]}
    end_w=$((start_w + 5))
    from_line=$((i * rows_per_window + 2))
    to_line=$(((i + 1) * rows_per_window + 1))
    if (( from_line > total_rows + 1 )); then
      echo "    skip ${date}_${start_w}w_${end_w}w (no rows)"
      continue
    fi
    window_csv="$(mktemp /tmp/eth_win_${date}_${start_w}w_XXXXXX.csv)"
    { head -n 1 "$raw"; sed -n "${from_line},${to_line}p" "$raw"; } > "$window_csv"
    out="$workload_dir/lsbt_${date}_${start_w}w_${end_w}w.csv"
    python3 "$tools_dir/convert_to_lsbt.py" --input "$window_csv" --output "$out"
    rm -f "$window_csv"
  done
  rm -f "$raw"
done

echo "Done. New slices are in $workload_dir"
