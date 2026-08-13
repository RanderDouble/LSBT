#!/usr/bin/env bash
# Shell translation of scripts/run_libcachesim_hierarchy.ps1.
#
# Usage: ./scripts/run_libcachesim_hierarchy.sh --trace PATH
#            [--format raw|lsbt] [--capacity-ratios R1,R2,...]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/../../.." && pwd)"

format="raw"
trace=""
ratios=(0.005 0.01 0.02)

usage() {
  cat <<'EOF'
Usage: run_libcachesim_hierarchy.sh [options]

  --trace PATH            input trace CSV (required)
  --format raw|lsbt       trace format (default: raw)
  --capacity-ratios LIST  comma-separated capacity ratios
                          (default: 0.005,0.01,0.02)
EOF
}

while (($#)); do
  case "$1" in
    --format) format=$2; shift 2 ;;
    --trace) trace=$2; shift 2 ;;
    --capacity-ratios) IFS=',' read -r -a ratios <<<"$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ -z $trace ]]; then
  usage >&2
  exit 1
fi

converter="$workspace/lrd_cache/LSBTreplacement/tools/hierarchy_trace_convert"
simulator="$workspace/libCacheSim/_build_prd/bin/cachesim"
converted="$workspace/external_traces/hierarchy/${format}_$(basename "$trace" .csv).csv"

if [[ ! -x "$converter" ]]; then
  echo "Build $converter first." >&2
  exit 1
fi
if [[ ! -x "$simulator" ]]; then
  echo "Build libCacheSim/_build_prd first." >&2
  exit 1
fi
mkdir -p "$(dirname "$converted")"

"$converter" "$format" "$trace" "$converted"

unique="$(awk -F, 'NR>1 && $2 != "" && !seen[$2]++ { c++ } END { print c+0 }' "$converted")"

for ratio in "${ratios[@]}"; do
  capacity="$(awk -v u="$unique" -v r="$ratio" 'BEGIN { c=int(u*r)*64; if (c<64) c=64; print c }')"
  "$simulator" "$converted" csv prd-epoch "$capacity" \
    -t 'time-col=1,obj-id-col=2,obj-size-col=3,feature-cols=4,delimiter=,,has-header=true,obj-id-is-num=1'
  "$simulator" "$converted" csv s3fifo "$capacity" \
    -t 'time-col=1,obj-id-col=2,obj-size-col=3,delimiter=,,has-header=true,obj-id-is-num=1'
  "$simulator" "$converted" csv lru "$capacity" \
    -t 'time-col=1,obj-id-col=2,obj-size-col=3,delimiter=,,has-header=true,obj-id-is-num=1'
done
