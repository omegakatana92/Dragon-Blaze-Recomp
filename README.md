# Dragon Blaze Recomp

**Dragon Blaze Recomp** is a static recompilation project focused on bringing *Dragon Blaze*, the classic arcade shoot 'em up from Psikyo, to modern hardware.

The project aims to recreate the original game's functionality through native recompilation, making the arcade experience accessible on modern Windows computers and Linux-based gaming devices.

Dragon Blaze Recomp is part of an ongoing effort to bring classic arcade games to modern platforms through static recompilation. The goal is to make these games easier to access while preserving the original experiences for both longtime fans and new players.

Whether playing on a Windows desktop or enjoying arcade action on the Steam Deck, Dragon Blaze Recomp aims to give this Psikyo classic a new home on modern hardware.

**Take to the skies once again with Dragon Blaze Recomp, bringing classic arcade action to Windows and Steam Deck.**

## Technical overview

A native Windows / Linux / Steam Deck build of Psikyo's 2000 vertical
shoot 'em up *Dragon Blaze* (MAME set `dragnblz`), which runs on Psikyo's PS5
board (Hitachi SH-2, PS6406B video, Yamaha YMF278B). The SH-2 game program is
translated ahead of time into C, and the board is reimplemented around it.

You need your own `dragnblz.zip` (24 files, MAME 0.264 set; no BIOS). No ROM
data, extracted graphics or sound is in this repository or in any public
package.

## What this is (and is not)

| Technique | Used here? |
|---|---|
| **Static recompilation** — every SH-2 instruction the analysis reaches (vectors, branch/call targets, code pointers in literal pools and pointer tables, entry points found by running the game) is translated to a C function at build time (`tools/recompilesh2.py`) and linked into the executable. The boot code copies the main program into work RAM and runs it there; the translator translates that code at its RAM address. | **Yes**, in the *recompiled* build. |
| **Decompilation** — readable, hand-named C of the game logic. | No. The generated C is mechanical, one function per instruction. |
| **Dynamic recompilation (JIT)** | No. Nothing is translated at run time. |
| **Interpretation** | A fallback SH-2 interpreter (`runtime/src/sh2_interp.c`) generated from the *same* decoder and calling the *same* instruction functions with the same cycle accounting. In the recompiled build it only runs code the static analysis missed (and RAM code while it does not match what was translated); the **generic** build uses it for everything. |
| **Hardware reimplementation** | Yes: the PS6406B sprite/background chip (zoomed sprites, per-pen and fixed alpha, Z-buffered priorities, row-scroll/zoom line layers, line fill/blend), the palette, the 93C56 EEPROM, the SH7604 divider and free-running timer, the interrupt logic and MAME's timing. MAME 0.264 is the behavioural reference; no MAME code is linked. The YMF278B (OPL4) is the ymfm library — the same code MAME uses. |

### Two kinds of build

| Build | Contains | Redistributable? |
|---|---|---|
| **Recompiled** (`-DDRAGNBLZ_ROM=...`) | C translated from *your* ROM | **No — keep it private.** It is code derived from the copyrighted program ROM. |
| **Generic** (`-DDB_GENERIC_BUILD=ON`) | no ROM-derived code; the SH-2 runs on the interpreter | Yes. Public release packages are generic builds. |

Both load your ROM at run time and produce identical output (per-frame work
RAM hashes match over 20,000 frames of random play). Headless during gameplay
(including drawing every frame) the generic build runs at ~130 fps and the
recompiled build at ~160 fps on the build machine; the game needs 60.

## Status

| Area | Status |
|---|---|
| ROM loading and validation (size + CRC32 in the runtime, SHA-1 in `tools/rom_set.py`; files may sit in a sub-folder of the zip; other PS5 games are recognised and refused) | implemented + tested |
| SH-2 translation + fallback interpreter | implemented + tested (recompiled vs generic identical; runs reach no instruction the runtime cannot execute) |
| Boot, warning/monitor screens, title, attract demo, service menu | implemented + tested |
| Accuracy vs MAME 0.264 | **work RAM byte-identical and screen pixel-identical at all 17 reference frames**: 11 frames of attract mode (60–3,600) and 6 frames of a scripted one-player game (1,800–4,800) |
| Graphics (zoomed/alpha sprites, line effects, priorities) | implemented + tested against MAME snapshots |
| Audio (YMF278B via ymfm) | implemented + tested: level within ~1 % of MAME's uncompressed recording in every second of play, waveform correlation 0.996 after resampling to MAME's 48 kHz |
| Gameplay | tested with scripted/random inputs (headless); **not played by a person** |
| EEPROM (settings, bookkeeping, high scores) | implemented (MAME `eepromser` behaviour, default contents from the set); persisted by the frontend |
| Save states, autosave, rewind, input movies | implemented + tested (600-frame headless round trip, including the sound chip) |
| Gadgets | Score P1 / Score P2 (addresses verified against MAME runs), plus FPS, frame time, input display |
| Frontend | implemented; tested under Xvfb (Linux) and Wine; **controllers, Steam Deck and a Windows PC not tested** |
| Windows x64 packages, Linux / Steam Deck AppImage | built; **not run on Windows 10/11 hardware or a Steam Deck** |

## Quick start

1. Get the package for your platform (or build it, see below).
2. Start it, choose **ROM Selection** and pick your `dragnblz.zip` (keyboard,
   mouse or controller). Every file is checked.
3. **Start Game**. Coin = `5`, P1 Start = `Enter`, shot / bomb / button 3 =
   `Z X C`. `Esc` (or Back + Start on a controller) opens the quick menu.
   See [docs/CONTROLS.md](docs/CONTROLS.md).

The first boot shows the game's own monitor-adjustment screen (as on a fresh
board / in MAME); it continues by itself.

## Building

```bash
git clone <this repo> && cd Dragon-Blaze-Recomp
# recompiled build (translates your ROM at configure time; keep the result private)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDRAGNBLZ_ROM=/path/to/dragnblz.zip
cmake --build build
./build/bin/DragonBlaze
./build/bin/dragnblz_headless --rom /path/to/dragnblz.zip --frames 3000 --png shot.png

# generic build (no ROM needed to build)
cmake -S . -B build-generic -G Ninja -DCMAKE_BUILD_TYPE=Release -DDB_GENERIC_BUILD=ON
cmake --build build-generic
```

The CMake files ship in the source package attached to each release (they are
not kept in the git repository). A C11 and a C++17 compiler are needed (the
sound chip library is C++).
Details: [docs/BUILD_LINUX.md](docs/BUILD_LINUX.md),
[docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md), [docs/STEAM_DECK.md](docs/STEAM_DECK.md).

## Documentation

- [docs/ROM_SETUP.md](docs/ROM_SETUP.md) — required files and checks
- [docs/CONTROLS.md](docs/CONTROLS.md) — mappings, service menu, region and debug switches
- [docs/FRONTEND.md](docs/FRONTEND.md) — menus, display/audio options, profiles, gadgets
- [docs/SAVE_STATES.md](docs/SAVE_STATES.md) — format and guarantees
- [docs/HARDWARE.md](docs/HARDWARE.md) — the board, the translator, the interpreter and how they were verified
- [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)
- [docs/MODIFIED_FILES.md](docs/MODIFIED_FILES.md) — what is shared with the other projects

## Licences

This project's own code is GPL-2.0 (`LICENSE`). Board behaviour follows MAME
(BSD-3-Clause); the YMF278B is ymfm by Aaron Giles (BSD-3-Clause, with two
small local changes to its save/restore, see MODIFIED_FILES.md); see
`LICENSES/`. SDL2 (zlib), miniz (MIT), Inter font (OFL). *Dragon Blaze* is ©
Psikyo; this project is not affiliated with or endorsed by its rights holders.
