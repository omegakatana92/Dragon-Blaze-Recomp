# Building on Linux

Requirements: CMake ≥ 3.20, Ninja (or make), C11 and C++17 compilers, Python ≥ 3.9,
SDL2 development files (`libsdl2-dev`) for the frontend.

```bash
# recompiled build: translates your ROM at configure time (keep private)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDRAGNBLZ_ROM=/path/to/dragnblz.zip
cmake --build build
cp -r profiles build/bin/
./build/bin/DragonBlaze

# generic build: no ROM needed at build time, redistributable
cmake -S . -B build-generic -G Ninja -DCMAKE_BUILD_TYPE=Release -DDB_GENERIC_BUILD=ON
cmake --build build-generic
```

`scripts/build-local.sh dragnblz.zip` checks the ROM, builds and runs the
smoke tests (`ctest`: boot, save-state round trip, 20,000-frame attract,
20,000-frame random play).

## Headless runner

`dragnblz_headless --rom dragnblz.zip --frames N` runs the game without a
window and prints progress. Options: `--png FILE`, `--png-every N`,
`--wav FILE` (mono, 37,286 Hz), `--dump-ram FILE`, `--dump-ram-at PREFIX:F1,F2`
(RAM + PNG after the listed frames), `--dump-video FILE`, `--hashes FILE`
(per-frame RAM hash), `--inputs none|auto|random:N|hold:MASK|demo|service:N`,
`--state-test FRAME --state-test-len N`, `--nvram-in/--nvram-out FILE`
(EEPROM), `--region 0x01000000`, `--debug-dip`, `--watch ADDR --watch-file F`
(work RAM words per frame), `--strict` (stop at untranslated code instead of
using the interpreter). At exit it reports how many instructions ran on the
fallback interpreter and where they started.

## Comparing with MAME

`tools/mame/demo_inputs.lua` plays the `--inputs demo` sequence into MAME 0.264
and dumps work RAM and a snapshot after frames 1800-4800:

```bash
mame dragnblz -nodrc -autoboot_script tools/mame/demo_inputs.lua -nvram_directory /tmp/empty-nv \
     -video none -sound none -nothrottle -wavwrite mame.wav -nocompressor
dragnblz_headless --rom dragnblz.zip --frames 4800 --inputs demo --dump-ram-at ours_:1800,2400,3000,3600,4200,4800
```

MAME's snapshots are rotated (ROT270); rotate ours 90° counter-clockwise to
compare.

## Translation coverage

`tools/recompilesh2.py` finds code from the vector table, the boot-time RAM
copy of the program, branch/call targets, code pointers in literal pools
(iterated to a fixed point) and pointer tables, with a plausibility filter.
Entry points that only appear at run time (computed jumps) are found with
`tools/iterate_seeds.py dragnblz.zip`, which runs the headless program with
several input scripts and appends every fallback entry address to
`tools/sh2_seeds.txt` (addresses only, no ROM data).

## Release build (portable binary for SteamOS and older distributions)

The release binary links SDL2 statically and targets glibc 2.31 (Steam Runtime
"sniper" / Debian 11 baseline). SDL2 still loads X11, Wayland, PulseAudio
(PipeWire's pulse server on the Deck) and ALSA from the system at run time.
The packages were built with zig as the C compiler:

```bash
pip install ziglang
mkdir -p ~/zigcc && for t in cc c++; do cat > ~/zigcc/$t <<EOS
#!/bin/sh
exec python3 -m ziglang $t -target x86_64-linux-gnu.2.31 -idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu "\$@"
EOS
done
printf '#!/bin/sh\nexec python3 -m ziglang ar "$@"\n' > ~/zigcc/ar
printf '#!/bin/sh\nexec python3 -m ziglang ranlib "$@"\n' > ~/zigcc/ranlib
chmod +x ~/zigcc/*

# static SDL2 2.30.x (backends loaded dynamically)
CC=~/zigcc/cc cmake -S SDL2-2.30.9 -B sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_AR=$HOME/zigcc/ar -DCMAKE_RANLIB=$HOME/zigcc/ranlib \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF -DCMAKE_INSTALL_PREFIX=$PWD/sdl-static
cmake --build sdl-build && cmake --install sdl-build

CC=~/zigcc/cc CXX=~/zigcc/c++ cmake -S . -B build/linux-generic -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_AR=$HOME/zigcc/ar -DCMAKE_RANLIB=$HOME/zigcc/ranlib \
  -DSDL2_DIR=$PWD/sdl-static/lib/cmake/SDL2 -DDB_GENERIC_BUILD=ON
cmake --build build/linux-generic
APPIMAGETOOL=/path/to/appimagetool KIND=generic ./scripts/build-appimage.sh
```

For a private recompiled AppImage use `-DDRAGNBLZ_ROM=...`
instead of `-DDB_GENERIC_BUILD=ON`, the build directory `build/linux-recompiled`
and `KIND=recompiled`.

Check: `ldd build/linux-generic/bin/DragonBlaze` lists only libc, libm,
libpthread, libdl; `objdump -T ... | grep GLIBC_` tops out at 2.29.

## Verified (2026-11-14, Ubuntu 24.04 container)

- Development build (GCC 13) and release builds (zig 0.13 `cc`/`c++`, glibc
  2.31 target, SDL2 2.30.9 static), recompiled and generic: per-frame RAM
  hashes identical over 20,000 frames of random play. The release binaries
  need GLIBC_2.29 at most and link only libc, libm, libpthread and libdl
  (libc++ is linked statically).
- Save-state round-trip test passes; MAME comparisons as in HARDWARE.md.
- The frontend and the AppImage run under Xvfb (Mesa llvmpipe): main menu
  without a ROM, ROM loading, game, quick menu, autosave and Continue. Under
  Xvfb's software renderer, with other jobs running, a few audio underruns were
  logged (13 in 1,544 frames); the frame rate held.
- Not run on physical Linux desktop hardware or a Steam Deck.
