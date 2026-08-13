#!/usr/bin/env bash
# Shell translation of scripts/summarize.ps1.
#
# Usage: ./scripts/summarize.sh [--input FILE] [--output FILE]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"

input="$root/results/raw.csv"
output="$root/results/summary.csv"

usage() {
  cat <<'EOF'
Usage: summarize.sh [options]

  --input FILE   raw results CSV (default: results/raw.csv)
  --output FILE  destination summary CSV (default: results/summary.csv)
EOF
}

while (($#)); do
  case "$1" in
    --input) input=$2; shift 2 ;;
    --output) output=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ ! -f "$input" ]]; then
  echo "input CSV not found: $input" >&2
  exit 1
fi

mkdir -p "$(dirname "$output")"

awk -F, '
NR == 1 { next }
$3 == "LSBTdiscrete"    { n[$4]++; req[$4]+=$6; hits[$4]+=$7; ms[$4]+=$10 }
$3 == "LSBTconsecutive" { ereq[$4]+=$6; ehits[$4]+=$7; ems[$4]+=$10 }
END {
  for (r in n) {
    dhr = req[r] ? hits[r]/req[r] : 0
    ehr = ereq[r] ? ehits[r]/ereq[r] : 0
    slow = ms[r] ? ems[r]/ms[r] : 0
    printf "%s,%d,%.0f,%.0f,%.0f,%.10g,%.10g,%.10g,%.10g,%.10g,%.10g\n", \
      r, n[r], req[r], hits[r], ehits[r], dhr, ehr, 100*(ehr-dhr), ms[r], ems[r], slow
  }
}
' "$input" | sort -n -t, -k1,1 > "$output"

echo "Summary: $output"
