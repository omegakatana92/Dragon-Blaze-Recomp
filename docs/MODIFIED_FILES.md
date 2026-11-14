# Shared code and what changed

The frontend, packaging and conventions come from the earlier arcade
recompilation projects; the SH-2 translator and interpreter come from the
Cyvern project (Super Kaneko Nova System).

| Path | Origin | Changes for Dragon Blaze |
|---|---|---|
| `frontend/` | Cyvern `frontend/` | unchanged |
| `frontend/assets/fe_font.h`, `tools/make_font.py` | unchanged | — |
| `runtime/third_party/miniz` | unchanged | — |
| `runtime/third_party/ymfm` | ymfm as shipped with MAME 0.264 (`3rdparty/ymfm`) | **two local changes, save/restore only:** `pcm_engine::save_restore` also saves the PCM registers (upstream omits them) and the prepare bookkeeping; `fm_engine_base::save_restore` saves the prepare bookkeeping and rebuilds the caches on restore instead of invalidating them. Sound generation is unchanged. Without this a restored state could produce slightly different audio. |
| `tools/sh2dec.py` | Cyvern | unchanged |
| `tools/recompilesh2.py` | Cyvern | Dragon Blaze ROM layout; translates the boot-time RAM copy of the program at its RAM address; adds the base cycle after an instruction's body (MAME's order) so device accesses see MAME's time; emits `sh2_program_ram_code()` |
| `tools/make_sh2_interp.py`, `runtime/src/sh2_interp.c` | Cyvern | same cycle-order change |
| `runtime/src/sh2_ops.c` | Cyvern (converted from MAME `sh.cpp`) | unchanged |
| `runtime/src/sh2_runtime.c`, `runtime/include/sh2/sh2.h` | Cyvern | level-held interrupt lines (`held_lines`), RAM-code validity check (`sh2_ram_code_*`), development trace hook |
| `runtime/src/sh2_program_none.c` | Cyvern | `sh2_program_ram_code()` stub |
| `tools/iterate_seeds.py` | Cyvern | Dragon Blaze runner and arguments |
| `cmake/mingw-w64-x86_64.cmake` | Cyvern | C++ compiler; libstdc++ linked statically |
| `scripts/*.sh`, `packaging/appimage/*` | Cyvern | renamed; no BIOS |
| `ports/headless/main.c` | Cyvern headless runner | PS5 board API; `--region`, `--debug-dip`, `--watch`/`--watch-file`, `--dump-ram-at`, `demo` and `service:N` input scripts |
| `ports/sdl2/main.c` | Cyvern adapter (structure) | new adapter: 2 players, 3 buttons, region/debug switches, EEPROM, score gadgets, ROT270 |
| `runtime/src/state.c` | Cyvern serializer (stream, sections, CRC, scratch-board validation) | new sections for the PS5 board; the sound chip blob is applied only after the whole state validated |

New for this project: `tools/rom_set.py` (Dragon Blaze set),
`tools/mame/demo_inputs.lua` (MAME reference capture), `runtime/src/board.c`,
`video.c` (port of MAME `psikyosh_v.cpp`), `eeprom.c` (93C56),
`ymf278b.cpp` (ymfm glue), `runtime/include/db/*.h`.
