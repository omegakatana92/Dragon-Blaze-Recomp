# Save states

Save states capture the complete running board and restore it exactly. They are
separate from the game's own non-volatile memory: the PS5 board keeps settings,
bookkeeping and high scores in a 256-byte serial EEPROM, which the frontend
stores as `nvram/dragnblz-<ROM fingerprint>.nv` (see FRONTEND.md). Loading a
save state restores the EEPROM contents it was saved with; the `.nv` file is
then updated with them.

## Using them

- **Slots 1-10** plus an **Autosave** slot. Save/Load State in either menu shows
  each slot's thumbnail, date and time, and play length.
- **Quick save / quick load** (F5 / F9 by default) use the current slot, shown
  with `*` in the slot list; F6 / F7 change it.
- Saving over an existing slot asks for confirmation in the menus. The quick-save
  hotkey overwrites the current slot without asking (that is what it is for).
- Loading while a game is running asks first, because unsaved progress is lost.
- **Autosave** is written when you return to the main menu or exit (both can be
  turned off in Extras). **Continue** on the main menu resumes it.
- Delete a slot with Delete / X in the slot list.

## Where they are stored

`<data folder>/states/dragnblz-<ROM fingerprint>/slotNN.fest` and
`autosave.fest`. The data folder is:

- Windows: `%APPDATA%\OmegaKatana\Dragon Blaze\`
- Linux / Steam Deck: `~/.local/share/OmegaKatana/Dragon Blaze/`
- Portable mode (a `portable.txt` beside the executable, or `--portable`):
  the executable's folder. Ignored inside an AppImage, which is read-only.

Because the folder name contains the ROM fingerprint, states from a different ROM
revision are never listed together with yours.

## What a state contains

The file is a container written atomically (write to `.tmp`, then rename):

| Field | Purpose |
| --- | --- |
| `FESTATE1`, container version | identifies the file format |
| game id `dragnblz`, runtime version | refuses states from other games |
| ROM fingerprint (CRC32 of the program image) | refuses other revisions or a modified set |
| timestamp, frame count | shown in the slot list |
| 160x120 thumbnail (unrotated; shown rotated) | preview |
| core state | the board, see below |
| CRC32 of the whole file | detects damage |

The core state (`runtime/src/state.c`, magic `DBLZCORE`) is a versioned, tagged,
little-endian blob with its own CRC32 and fingerprint. One routine walks the board
for both saving and loading, so the two layouts cannot drift apart. Sections:

- `SH2 ` - R0-R15, PC, PR, SR, GBR, VBR, MACH/MACL, interrupt lines (and which
  are level-held) and the internal (on-chip) interrupt request, SLEEP state,
  instruction and cycle counters.
- `ONCH` - the SH7604 on-chip register file, the DIVU operands/flags and the
  free-running timer (TIER, FTCSR, FRC, OCRA/B, TCR, TOCR, next event).
- `RAM ` - 1 MiB work RAM (including the RAM copy of the program).
- `VRAM` - sprite/background RAM, palette RAM, zoom table, video registers.
- `VID ` - the sprite list latched at the last VBLANK, the sprite assembly
  buffer and the last composed picture.
- `EEPR` - EEPROM contents and the serial interface state (command, address,
  shift register, write/erase completion time).
- `YMF ` - the YMF278B in ymfm's own save format (FM and wavetable registers,
  all channels and operators, envelope and LFO state), its two timers, the busy
  flag and the stream position.
- `MISC` - region jumpers, debug switch, frame counter.

No ROM data is stored in a state.

## Restoring safely

Loading first validates the whole file (magic, version, CRC, game id,
fingerprint, every section's tag and length). Only then is the running board
replaced. A damaged or incompatible state is rejected with a message and the
current game continues untouched. After loading, queued audio is flushed, held
inputs are cleared and must be released before they count again, and the
picture is re-rendered from the restored video state.

Because code the build did not translate runs on the fallback interpreter, a
state can be resumed anywhere: states move freely between the recompiled and
the generic build of the same ROM set, and between versions with different
translation coverage.

## Verification (2026-11-13)

- `dragnblz_headless --inputs random:1 --state-test 2400 --state-test-len 600`
  saves at frame 2400 (in a stage, with music and effects playing), runs 600
  frames recording work RAM, sprite RAM, the composed picture, the EEPROM and
  the audio output hash per frame, checks that a corrupted copy and a
  truncated copy are rejected and leave the board untouched, re-saves right
  after loading (byte-identical to the original state), then replays the same
  600 frames. Result: **PASS, identical**. Registered as the
  `save_state_round_trip` CTest. (This test found that ymfm's own save/restore
  omits the wavetable registers; see MODIFIED_FILES.md.)
- In the SDL frontend (Xvfb): autosave written when leaving the game, and
  Continue resumes it.

## Rewind

Extras > Rewind keeps a ring of compressed core states (every 4th frame) within a
memory budget (Rewind Memory Limit, default 64 MB; oldest history is dropped).
Hold the Rewind hotkey to step back. It uses the same serializer as save states.

## Input movies

Extras > Record Input Movie stores a full state plus one input word per frame
(`movies/movie-*.femv`); Play Latest Movie loads the state and replays the
inputs. Playback is exact because the runtime is deterministic (the development build,
the Linux release builds and the Windows builds under Wine, recompiled and
generic, produce identical per-frame RAM hashes over the same random-input
runs).
