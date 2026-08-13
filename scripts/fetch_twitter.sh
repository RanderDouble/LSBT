#!/usr/bin/env bash
# Download the Twitter (OSDI'20) sample100 clusters used by the S3FIFO paper
# from the CMU PDL cache datasets archive.
#
# Usage: ./scripts/fetch_twitter.sh [--connections N] [CLUSTER ...]
# Default clusters: 12 24 47 48 52. Uses a parallel range downloader because
# the CMU mirror is slow for single connections.
# Files are stored under workload/twitter_osdi20/raw/ and are gitignored.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
data_dir="$root/workload/twitter_osdi20/raw"

connections=32
clusters=()
while (($#)); do
  case "$1" in
    --connections) connections=$2; shift 2 ;;
    -h|--help) echo "Usage: fetch_twitter.sh [--connections N] [CLUSTER ...]"; exit 0 ;;
    *) clusters+=("$1"); shift ;;
  esac
done
if (( ${#clusters[@]} == 0 )); then
  clusters=(12 24 47 48 52)
fi

base_url="https://ftp.pdl.cmu.edu/pub/datasets/twemcacheWorkload/cacheDatasets/twitter/sample100"
mkdir -p "$data_dir"

for c in "${clusters[@]}"; do
  file="cluster${c}.oracleGeneral.sample100.zst"
  dest="$data_dir/$file"
  if [[ -s "$dest" ]]; then
    echo "already present: $file"
  else
    echo "==> downloading $file"
    size="$(curl -L -sSI "$base_url/$file" | awk 'tolower($1)=="content-length:" {print $2}' | tr -d '\r')"
    if [[ -z $size ]]; then
      curl -L --fail --retry 3 -o "$dest" "$base_url/$file"
    else
      attempt=0
      while ! zstd -t "$dest.part" >/dev/null 2>&1; do
        attempt=$((attempt + 1))
        if (( attempt > 25 )); then
          echo "still corrupted after 25 rounds: $file" >&2
          exit 1
        fi
        echo "==> download round $attempt for $file"
        python3 "$root/tools/download_range.py" \
          --url "$base_url/$file" --output "$dest.part" \
          --size "$size" --connections "$connections" || true
        if zstd -t "$dest.part" >/dev/null 2>&1; then
          break
        fi
      done
      mv "$dest.part" "$dest"
    fi
  fi
done

echo "Downloaded to $data_dir"
