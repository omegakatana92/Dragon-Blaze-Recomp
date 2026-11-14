#!/usr/bin/env bash
# Development build + headless smoke tests.
# usage: scripts/build-local.sh /path/to/dragnblz.zip [build-dir]
set -euo pipefail
if [[ $# -lt 1 ]]; then echo "usage: $0 /path/to/dragnblz.zip [build-dir]" >&2; exit 2; fi
ROM=$1
BUILD_DIR=${2:-build/cmake}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
python3 tools/rom_set.py "$ROM"
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDRAGNBLZ_ROM="$ROM"
cmake --build "$BUILD_DIR"
(cd "$BUILD_DIR" && ctest --output-on-failure -R "headless_boot|save_state_round_trip")
echo "Build and headless smoke tests passed."
