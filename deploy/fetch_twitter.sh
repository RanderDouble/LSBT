#!/usr/bin/env bash
# Download Twitter (OSDI'20) sample100 clusters used by the S3FIFO paper,
# decompress, and convert to the block CSV format (depth=1).
#
# Usage: ./fetch_twitter.sh [--max-requests N] [CLUSTER ...]
#   --max-requests N   keep the first N requests per cluster (default 10M)
#   CLUSTER ...        cluster ids to fetch (default: 12 24 47 48 52)
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="$DIR/workloads/twitter/raw"
CSV_DIR="$DIR/workloads/twitter/csv"
PYBIN="$DIR/.venv/bin/python"
[[ -x "$PYBIN" ]] || PYBIN="$(command -v python3)"

# --- config ---
CLUSTERS=(12 24 47 48 52)
MAX_REQUESTS=10000000
CONNECTIONS=24
BASE_URL="https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/twitter/sample100"

while (($#)); do
  case "$1" in
    --max-requests) MAX_REQUESTS=$2; shift 2 ;;
    -h|--help) echo "Usage: fetch_twitter.sh [--max-requests N] [CLUSTER ...]"; exit 0 ;;
    *) CLUSTERS=("$@"); break ;;
  esac
done

mkdir -p "$RAW_DIR" "$CSV_DIR"

for c in "${CLUSTERS[@]}"; do
  file="cluster${c}.oracleGeneral.sample100.zst"
  zst="$RAW_DIR/$file"
  bin="$RAW_DIR/cluster${c}.oracleGeneral.sample100.bin"
  out="$CSV_DIR/twitter_cluster${c}.csv"

  if [[ ! -s "$zst" ]]; then
    echo "==> downloading $file"
    size="$(curl -L -sSI "$BASE_URL/$file" | awk 'tolower($1)=="content-length:" {print $2}' | tr -d '\r')"
    attempt=0
    while ! zstd -t "$zst" >/dev/null 2>&1; do
      attempt=$((attempt + 1))
      if (( attempt > 25 )); then echo "download failed: $file" >&2; exit 1; fi
      "$PYBIN" "$DIR/tools/download_range.py" \
        --url "$BASE_URL/$file" --output "$zst.part" \
        --size "$size" --connections "$CONNECTIONS" || true
      if zstd -t "$zst.part" >/dev/null 2>&1; then mv "$zst.part" "$zst"; fi
    done
  else
    echo "already present: $file"
  fi

  if [[ ! -f "$bin" ]]; then
    echo "==> decompressing cluster${c}"
    zstd -d -f "$zst" -o "$bin"
  fi
  echo "==> converting cluster${c} (max_requests=$MAX_REQUESTS)"
  "$DIR/tools/twitter_bin_to_block" "$bin" "$out" "$MAX_REQUESTS"
done

echo "Twitter block traces: $CSV_DIR ($(ls "$CSV_DIR"/*.csv | wc -l) clusters)"
