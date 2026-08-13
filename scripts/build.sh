#!/usr/bin/env bash
# Shell translation of scripts/build.ps1.
#
# Usage: ./scripts/build.sh [release|debug]
set -euo pipefail

configuration="${1:-release}"
case "$configuration" in
  release)
    flags=(-std=c++17 -Wall -Wextra -pedantic -O3 -DNDEBUG)
    ;;
  debug)
    flags=(-std=c++17 -Wall -Wextra -pedantic -O0 -g)
    ;;
  *)
    echo "build.sh: invalid configuration '$configuration' (expected release or debug)" >&2
    exit 1
    ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"

g++ "${flags[@]}" "$root/code/runner.cpp" -o "$root/runner"

echo "Built PRD runner ($root/runner)"
