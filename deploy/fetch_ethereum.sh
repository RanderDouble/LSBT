#!/usr/bin/env bash
# Fetch Ethereum 2024 trace slices from the AWS Public Blockchain dataset and
# convert them into the block CSV format used by the runner.
#
# Usage: ./fetch_ethereum.sh [DATE ...]
#   DATE format YYYY-MM-DD; default dates below.
# Each date is fetched once (WINDOWS * ROWS_PER_WINDOW raw rows) and split
# into WINDOWS slices named lsbt_<date>_<w>w_<w+5>w.csv.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="$DIR/workloads/ethereum/raw"
BLOCK_DIR="$DIR/workloads/ethereum/block"
PYBIN="$DIR/.venv/bin/python"
[[ -x "$PYBIN" ]] || PYBIN="$(command -v python3)"

# --- config ---
DATES=(2024-02-15 2024-06-20 2024-08-15 2024-10-15)
WINDOWS=(0 5 10 15 20)      # 万-unit window starts (0w_5w, 5w_10w, ...)
ROWS_PER_WINDOW=50000

if (($#)); then
  DATES=("$@")
fi

mkdir -p "$RAW_DIR" "$BLOCK_DIR"

for date in "${DATES[@]}"; do
  raw="$(mktemp /tmp/eth_raw_${date}_XXXXXX.csv)"
  echo "==> fetching $date (${#WINDOWS[@]} windows x ${ROWS_PER_WINDOW} rows)"
  "$PYBIN" "$DIR/tools/get_trace.py" --date "$date" --start-row 0 \
    --row-count $((${#WINDOWS[@]} * ROWS_PER_WINDOW)) --output "$raw"

  total_rows=$(($(wc -l < "$raw") - 1))
  for i in "${!WINDOWS[@]}"; do
    start_w=${WINDOWS[$i]}
    end_w=$((start_w + 5))
    from_line=$((i * ROWS_PER_WINDOW + 2))
    to_line=$(((i + 1) * ROWS_PER_WINDOW + 1))
    if (( from_line > total_rows + 1 )); then
      echo "    skip ${date}_${start_w}w_${end_w}w (no rows)"
      continue
    fi
    window_csv="$(mktemp /tmp/eth_win_${date}_${start_w}w_XXXXXX.csv)"
    { head -n 1 "$raw"; sed -n "${from_line},${to_line}p" "$raw"; } > "$window_csv"
    out="$BLOCK_DIR/lsbt_${date}_${start_w}w_${end_w}w.csv"
    "$PYBIN" "$DIR/tools/convert_to_lsbt.py" --input "$window_csv" --output "$out"
    rm -f "$window_csv"
  done
  mv "$raw" "$RAW_DIR/trace_${date}.csv"
done

echo "Ethereum block traces: $BLOCK_DIR ($(ls "$BLOCK_DIR"/*.csv | wc -l) slices)"
