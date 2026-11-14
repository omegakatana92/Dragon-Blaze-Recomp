# ROM setup

One MAME-format archive is needed. Nothing else (no BIOS, no CHD, no samples).

## `dragnblz.zip` — MAME 0.264 set `dragnblz`

| File | Size | CRC32 | Role |
|---|---|---|---|
| `2prog_h.u21` | 512 KiB | `fc5eade8` | SH-2 program (high words) |
| `1prog_l.u22` | 512 KiB | `95d6fd02` | SH-2 program (low words) |
| `1l.u4` … `10l.u58` | 10 x 2 MiB | see `tools/rom_set.py` | graphics (low words) |
| `1h.u12` … `10h.u59` | 10 x 2 MiB | see `tools/rom_set.py` | graphics (high words) |
| `snd0.u52` | 2 MiB | `7fd1b225` | YMF278B wave ROM |
| `eeprom-dragnblz.u44` | 256 bytes | `46e85da9` | default EEPROM contents |

All 24 files are required. The files may sit at the top of the zip or in a
`dragnblz/` sub-folder. Archives of the other PS5 games (Gunbird 2,
Strikers 1945 III, Gunbarich, Tetris The Grand Master 2) are recognised by
their program ROM names and refused with a message.

## How the files are found and checked

- **Frontend:** ROM Selection lists `.zip` files; pick `dragnblz.zip`. At
  start-up the frontend also looks for `dragnblz.zip` in the data folder,
  `~/Downloads` and `~/roms`.
- **Runtime:** every file's size and CRC32 (from the zip directory) is checked
  before anything is loaded; a missing, wrong or swapped file is named in the
  error message.
- **Tools / build:** `python3 tools/rom_set.py dragnblz.zip` checks sizes,
  CRC32 and SHA-1 of the extracted data.

## What the build does with your ROM

A *recompiled* build reads the two program ROMs at configure time and writes C
translations to `build/generated/`. That C is derived from your ROM: keep
`build/`, `dist/` and any recompiled package private, and never commit them.
A *generic* build (`-DDB_GENERIC_BUILD=ON`) does not read any ROM at build
time. Graphics, sound and the default EEPROM are always read from your archive
at run time; nothing is extracted to disk.

## The EEPROM

The board keeps its settings (difficulty, lives, coinage, …), bookkeeping and
high scores in a 256-byte serial EEPROM. On first use it holds the default
contents from `eeprom-dragnblz.u44` (as MAME does); afterwards the frontend
keeps it in `nvram/dragnblz-<fingerprint>.nv` in the data folder. The file is
the raw 256 bytes, the same layout as MAME's `nvram/dragnblz/eeprom`, so the
two can be exchanged.
