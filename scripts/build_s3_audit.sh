#!/usr/bin/env bash
# Shell translation of scripts/build_s3_audit.ps1.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"

g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic \
  "$root/code/s3_audit_runner.cpp" -o "$root/s3_audit_runner"

echo "Built s3_audit_runner ($root/s3_audit_runner)"
