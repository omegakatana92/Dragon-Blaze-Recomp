# Known issues

- **Not tested on real hardware or by a person:** Windows 10/11 PCs, the
  Steam Deck, physical controllers and arcade sticks, and actual
  play-throughs. Gameplay was exercised with scripted and random inputs
  (headless) and compared with MAME; the frontend was run under Xvfb and Wine.
- **Audio is not sample-compared with MAME:** MAME resamples its output to
  48 kHz, so only levels (within ~1 % per second) and the waveform after
  resampling (correlation 0.996) were compared. The sound chip code itself is
  the same ymfm library MAME uses.
- **Video follows MAME's emulation, not the real chip:** where MAME's
  `psikyosh_v.cpp` is known to be approximate (its own TODO list: sprite zoom
  rounding, background line zoom, sprite-sprite priority groups, flip screen,
  the 240-line mode), this port reproduces MAME's output, not the hardware's.
  Background zoom on non-line layers is not implemented (as in MAME; the game
  does not appear to use it).
- **Region:** the default is International Ver. B (MAME's default). Changing
  the region jumper takes effect after a reset; the other regions were not
  checked beyond starting.
- **Button 3** is mapped (the board has it) but its in-game use was not checked.
- **Gadgets:** only the two score values are offered; lives, bombs and other
  values are not verified and therefore not shown.
- **CRT curvature** is not offered (SDL_Renderer has no shaders).
