# Changelog

## 0.1.0 — first release

- Static recompilation of the Dragon Blaze SH-2 program (including the main
  program the boot code copies to work RAM), with a fallback interpreter built
  from the same instruction semantics; a generic (interpreter-only,
  redistributable) build.
- Psikyo PS5 board: PS6406B video ported from MAME (zoomed/alpha sprites with
  Z-buffer, line layers, line fill/blend), palette, 93C56 EEPROM, SH7604
  divider and free-running timer, interrupts, MAME's attosecond timing.
- YMF278B through ymfm (the library MAME uses) with MAME-equivalent glue.
- Verified against MAME 0.264: work RAM and screen identical at 17 reference
  frames of attract mode and scripted gameplay.
- Shared SDL2 frontend: ROM picker with validation, menus, display (vertical
  monitor), audio, controller mapping, save states, rewind, input movies,
  EEPROM persistence, score gadgets.
- Windows x64 zip and Linux / Steam Deck AppImage (generic builds).
