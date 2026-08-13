#!/usr/bin/env bash
# Shell translation of scripts/run_twitter_libcachesim_prd.ps1.
#
# Usage: ./scripts/run_twitter_libcachesim_prd.sh [--trace PATH]
#            [--trace-type TYPE] [--trace-params PARAMS] [--capacities C1,C2,...]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/../../.." && pwd)"

trace="$workspace/libCacheSim/data/twitter_cluster52.csv"
trace_type="csv"
trace_params='time-col=1,obj-id-col=2,obj-size-col=3,delimiter=,,has-header=true,obj-id-is-num=1'
capacities=(1MB 5MB 10MB)

usage() {
  cat <<'EOF'
Usage: run_twitter_libcachesim_prd.sh [options]

  --trace PATH         input trace (default: libCacheSim/data/twitter_cluster52.csv)
  --trace-type TYPE    trace type (default: csv)
  --trace-params PARAMS
                       cachesim -t parameter string
  --capacities LIST    comma-separated capacities (default: 1MB,5MB,10MB)
EOF
}

while (($#)); do
  case "$1" in
    --trace) trace=$2; shift 2 ;;
    --trace-type) trace_type=$2; shift 2 ;;
    --trace-params) trace_params=$2; shift 2 ;;
    --capacities) IFS=',' read -r -a capacities <<<"$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

sim="$workspace/libCacheSim/_build_prd/bin/cachesim"
if [[ ! -x "$sim" ]]; then
  echo "Missing $sim. Build libCacheSim first; see results/libcachesim_twitter_prd.md." >&2
  exit 1
fi

for capacity in "${capacities[@]}"; do
  "$sim" "$trace" "$trace_type" prd "$capacity" -t "$trace_params" \
    -e "cold-size-ratio=0.2,ghost-size-ratio=8,lease-turnovers=8"
  "$sim" "$trace" "$trace_type" prd "$capacity" -t "$trace_params" \
    -e "cold-size-ratio=0.2,ghost-size-ratio=8,lease-turnovers=1,adaptive-lease=true"
  "$sim" "$trace" "$trace_type" prd "$capacity" -t "$trace_params" \
    -e "cold-size-ratio=0.2,ghost-size-ratio=8,lease-turnovers=8,adaptive-lease=true"
  "$sim" "$trace" "$trace_type" s3fifo "$capacity" -t "$trace_params"
  "$sim" "$trace" "$trace_type" lru "$capacity" -t "$trace_params"
done
