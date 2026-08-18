#!/usr/bin/env bash
# Setup for the deploy package: install Python dependencies and build the
# native runner + trace converter.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command -v python3 >/dev/null || { echo "ERROR: python3 required" >&2; exit 1; }
command -v g++ >/dev/null || { echo "ERROR: g++ (C++17) required" >&2; exit 1; }
command -v zstd >/dev/null || echo "WARN: zstd not found; twitter download/convert will fail"

python3 -m pip install -r "$DIR/requirements.txt"

g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic \
  "$DIR/code/ablation_runner.cpp" -o "$DIR/ablation_runner"
g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic \
  "$DIR/tools/twitter_bin_to_block.cpp" -o "$DIR/tools/twitter_bin_to_block"

echo "setup done: $DIR/ablation_runner"
