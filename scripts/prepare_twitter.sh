#!/usr/bin/env bash
# Decompress downloaded Twitter (OSDI'20) samples and convert them to the
# repository's block CSV format (version,depth,key).
#
# Usage: ./scripts/prepare_twitter.sh [--max-requests N] [CLUSTER ...]
#   --max-requests N   keep only the first N requests per cluster
#                      (default: 10000000)
#   CLUSTER ...        cluster ids to process (default: 12 24 47 48 52)
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
raw_dir="$root/workload/twitter_osdi20/raw"
csv_dir="$root/workload/twitter_osdi20/csv"

max_requests=10000000
clusters=(12 24 47 48 52)
while (($#)); do
  case "$1" in
    --max-requests) max_requests=$2; shift 2 ;;
    -h|--help) echo "Usage: prepare_twitter.sh [--max-requests N] [CLUSTER ...]"; exit 0 ;;
    *) clusters=("$@"); break ;;
  esac
done

mkdir -p "$csv_dir"

for c in "${clusters[@]}"; do
  zst="$raw_dir/cluster${c}.oracleGeneral.sample100.zst"
  bin="$raw_dir/cluster${c}.oracleGeneral.sample100.bin"
  if [[ ! -f "$zst" ]]; then
    echo "missing $zst (run ./scripts/fetch_twitter.sh first)" >&2
    exit 1
  fi
  if [[ ! -f "$bin" ]]; then
    echo "==> decompressing cluster${c}"
    zstd -d -f "$zst" -o "$bin"
  fi
  out="$csv_dir/twitter_cluster${c}.csv"
  echo "==> converting cluster${c} (max_requests=$max_requests)"
  "$root/tools/twitter_bin_to_block" "$bin" "$out" "$max_requests"
done

echo "Converted traces are in $csv_dir"
