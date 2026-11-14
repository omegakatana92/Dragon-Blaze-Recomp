# Building for Windows 10/11 (x86-64)

Two routes produce the same program: a native MinGW build on Windows, or a
cross-compile from Linux.

## A. Native build on Windows (MSYS2)

1. Install MSYS2, open the "MSYS2 MINGW64" shell and run
   `pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-python git`
2. Build:

```bash
git clone <this repo> && cd Dragon-Blaze-Recomp
# recompiled (private) build:
cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDRAGNBLZ_ROM=C:/path/to/dragnblz.zip
# or the generic build: -DDB_GENERIC_BUILD=ON instead of the ROM path
cmake --build build/cmake
cp /mingw64/bin/SDL2.dll build/cmake/bin/
cp -r profiles build/cmake/bin/
./build/cmake/bin/DragonBlaze-Windows-x86_64.exe
```

The SDL2 "mingw" development package also works; pass
`-DSDL2_DIR=<SDL2>\x86_64-w64-mingw32\lib\cmake\SDL2`.

## B. Cross-compile from Linux

```bash
sudo apt install gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64-posix cmake ninja-build python3
# SDL2 2.30.x mingw development package from https://github.com/libsdl-org/SDL/releases
tar xzf SDL2-devel-2.30.9-mingw.tar.gz
cmake -S . -B build/win64-generic -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DSDL2_DIR=$PWD/SDL2-2.30.9/x86_64-w64-mingw32/lib/cmake/SDL2 \
  -DDB_GENERIC_BUILD=ON
cmake --build build/win64-generic
SDL2_DLL=$PWD/SDL2-2.30.9/x86_64-w64-mingw32/bin/SDL2.dll KIND=generic ./scripts/package-windows.sh
```

The package (`dist/DragonBlaze-Windows-x86_64.zip`) contains the executable,
`SDL2.dll`, `profiles/dragnblz.json`, licences and docs. The executable is
statically linked against libgcc/libstdc++; it needs only system DLLs and `SDL2.dll`.
For a recompiled package use `-DDRAGNBLZ_ROM=...`, the build
directory `build/win64-recompiled` and `KIND=recompiled` (the zip is then named
`...-PRIVATE.zip`: do not share it).

Settings, saves and the EEPROM file go to `%APPDATA%\OmegaKatana\Dragon Blaze\`
(or beside the executable with `--portable` / a `portable.txt` file).

## Verified (2026-11-14)

- Cross-compiled with MinGW-w64 GCC 13 (posix threads; libgcc, libstdc++ and
  winpthread linked statically: the executable imports only KERNEL32, msvcrt,
  SHELL32 and SDL2.dll). Under Wine 9: the headless runner (recompiled and
  generic) gives per-frame RAM hashes identical to the Linux builds over
  5,000 frames of random play; the packaged frontend starts (Direct3D
  renderer), loads the ROM, runs the game and opens the quick menu.
- **Not run on a Windows 10/11 PC.**
