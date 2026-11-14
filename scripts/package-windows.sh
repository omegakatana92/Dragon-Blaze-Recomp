#!/usr/bin/env bash
# Zip a Windows build with SDL2.dll, the game profile, licences and docs.
#   BUILD_DIR  build directory (default build/win64-generic)
#   KIND       generic (default) or recompiled
#   SDL2_DLL   path to the x86_64 SDL2.dll (required)
# A generic build contains no ROM-derived code and may be redistributed.  A
# recompiled build contains code translated from YOUR ROM: keep it private.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
KIND=${KIND:-generic}
BUILD_DIR=${BUILD_DIR:-build/win64-$KIND}
DIST=${DIST:-dist}
: "${SDL2_DLL:?set SDL2_DLL to the x86_64 SDL2.dll}"
if [[ $KIND == recompiled ]]; then NAME=DragonBlaze-Recompiled-Windows-x86_64-PRIVATE; else NAME=DragonBlaze-Windows-x86_64; fi
STAGE="$DIST/$NAME"
rm -rf "$STAGE" && mkdir -p "$STAGE/docs" "$STAGE/profiles"
cp "$BUILD_DIR/bin/DragonBlaze-Windows-x86_64.exe" "$STAGE/"
x86_64-w64-mingw32-strip "$STAGE/DragonBlaze-Windows-x86_64.exe" 2>/dev/null || true
cp "$SDL2_DLL" "$STAGE/"
cp profiles/dragnblz.json "$STAGE/profiles/"
cp -r LICENSES LICENSE "$STAGE/"
cp README.md CHANGELOG.md "$STAGE/"
cp docs/CONTROLS.md docs/SAVE_STATES.md docs/FRONTEND.md docs/BUILD_WINDOWS.md docs/ROM_SETUP.md docs/KNOWN_ISSUES.md "$STAGE/docs/"
(cd "$DIST" && rm -f "$NAME.zip" && python3 -m zipfile -c "$NAME.zip" "$NAME")
echo "wrote $DIST/$NAME.zip (no ROM included)"
