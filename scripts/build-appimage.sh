#!/usr/bin/env bash
# Build the Steam Deck / Linux AppImage from a release build.
#
#   KIND          generic (default) or recompiled
#   BUILD_DIR     build directory holding bin/DragonBlaze (default build/linux-$KIND)
#   APPIMAGETOOL  path to appimagetool (default: on PATH)
#
# The release build should link SDL2 statically against an old glibc baseline;
# see docs/BUILD_LINUX.md ("Release build").  No ROM data is packaged.
# A recompiled build contains code translated from YOUR ROM: keep it private.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

KIND=${KIND:-generic}
BUILD_DIR=${BUILD_DIR:-build/linux-$KIND}
APPDIR=${APPDIR:-build/AppDir-$KIND}
DIST=${DIST:-dist}
TOOL=${APPIMAGETOOL:-appimagetool}
if [[ $KIND == recompiled ]]; then OUT=DragonBlaze-Recompiled-SteamDeck-x86_64-PRIVATE.AppImage; else OUT=DragonBlaze-SteamDeck-x86_64.AppImage; fi

if [[ ! -x "$BUILD_DIR/bin/DragonBlaze" ]]; then
  echo "error: $BUILD_DIR/bin/DragonBlaze not found; build first" >&2
  exit 1
fi

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/dragnblz/profiles" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps" "$APPDIR/usr/share/doc/dragnblz" "$DIST"
cp "$BUILD_DIR/bin/DragonBlaze" "$APPDIR/usr/bin/dragnblz"
strip "$APPDIR/usr/bin/dragnblz" 2>/dev/null || true
cp profiles/dragnblz.json "$APPDIR/usr/share/dragnblz/profiles/"
cp packaging/appimage/dragnblz.desktop "$APPDIR/dragnblz.desktop"
cp packaging/appimage/dragnblz.png "$APPDIR/dragnblz.png"
cp packaging/appimage/AppRun "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun" "$APPDIR/usr/bin/dragnblz"
cp packaging/appimage/dragnblz.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/dragnblz.png"
cp packaging/appimage/dragnblz.desktop "$APPDIR/usr/share/applications/dragnblz.desktop"
cp -r LICENSES LICENSE README.md docs/CONTROLS.md "$APPDIR/usr/share/doc/dragnblz/"

if ldd "$APPDIR/usr/bin/dragnblz" | grep -q SDL2; then
  echo "warning: the binary links SDL2 dynamically; the host must provide libSDL2-2.0.so.0" >&2
fi

ARCH=x86_64 $TOOL --no-appstream "$APPDIR" "$DIST/$OUT"
echo "wrote $DIST/$OUT (no ROM included)"
