#!/usr/bin/env bash
# Setup for the deploy package: install Python dependencies and build the
# native runner + trace converter.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command -v python3 >/dev/null || { echo "ERROR: python3 required" >&2; exit 1; }
command -v g++ >/dev/null || { echo "ERROR: g++ (C++17) required" >&2; exit 1; }
command -v zstd >/dev/null || echo "WARN: zstd not found; twitter download/convert will fail"

# Python deps: prefer an isolated venv; on PEP 668 externally-managed systems
# fall back to --break-system-packages. pip runs exactly once.
PYBIN="$DIR/.venv/bin/python"
if [[ -x "$PYBIN" ]]; then
  echo "using existing virtualenv: $DIR/.venv"
  "$PYBIN" -m pip install -r "$DIR/requirements.txt"
elif python3 -m venv "$DIR/.venv" 2>/dev/null && [[ -x "$PYBIN" ]]; then
  echo "created virtualenv: $DIR/.venv"
  "$PYBIN" -m pip install -r "$DIR/requirements.txt"
else
  echo "WARN: python3 -m venv failed (recommend: apt install python3-venv python3-full)" >&2
  echo "WARN: falling back to pip --break-system-packages" >&2
  python3 -m pip install --break-system-packages -r "$DIR/requirements.txt"
fi

g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic \
  "$DIR/code/ablation_runner.cpp" -o "$DIR/ablation_runner"
g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic \
  "$DIR/tools/twitter_bin_to_block.cpp" -o "$DIR/tools/twitter_bin_to_block"

echo "setup done: $DIR/ablation_runner"
