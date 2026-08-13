#!/usr/bin/env bash
# Shell translation of scripts/build_ablation.ps1.
#
# Usage: ./scripts/build_ablation.sh [release|debug]
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
    echo "build_ablation.sh: invalid configuration '$configuration' (expected release or debug)" >&2
    exit 1
    ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"

g++ "${flags[@]}" "$root/code/ablation_runner.cpp" -o "$root/ablation_runner"
g++ "${flags[@]}" "$root/code/ablation_path_runner.cpp" -o "$root/ablation_path_runner"

"$root/ablation_runner" --self-test
"$root/ablation_path_runner" --self-test

echo "Built ablation runners and passed self-tests"
