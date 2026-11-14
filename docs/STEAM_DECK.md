# Steam Deck

Artifact: `DragonBlaze-SteamDeck-x86_64.AppImage` (native Linux, no Proton; generic
build, redistributable). A recompiled AppImage built from your own ROMs is
named `...-PRIVATE.AppImage` and must not be shared.

**Status: built and run under Xvfb on a Linux PC; not yet run on a Steam Deck.**

## Install

1. Desktop Mode: copy the AppImage and `dragnblz.zip` into one folder, e.g.
   `~/Games/DragonBlaze/`. Right-click the AppImage > Properties >
   Permissions > "Is executable" (or `chmod +x`).
2. Run it once from Desktop Mode to check it starts. Use ROM Selection to pick
   `dragnblz.zip` (touchscreen, trackpad or controller).
3. Steam > Add a Non-Steam Game > browse to the AppImage. Optional launch
   options: `--rom "/home/deck/Games/DragonBlaze/dragnblz.zip"`.
4. Launch from Game Mode.

## Behaviour on the Deck

- Starts in borderless fullscreen when the `SteamDeck` environment variable is
  present (Steam sets it); menus are laid out for 1280x800.
- The game is vertical: on the Deck's landscape screen it is shown upright
  with side borders (Display > Background: gradient or ambient glow fills
  them). Rotation 0 suits playing with the Deck held sideways ("tate").
- Game Mode: gamescope provides XWayland, so `AppRun` selects SDL's X11 driver
  there; Desktop Mode leaves SDL's default (X11 or Wayland).
- Controls arrive as a standard controller through Steam Input. View = coin,
  Menu = start, View + Menu = quick menu, A/B/X = shot/bomb/button 3. Player 2 uses the
  next controller. The Steam button stays with Steam.
- Settings, saves, NVRAM, screenshots and movies:
  `~/.local/share/OmegaKatana/Dragon Blaze/` (outside the
  read-only AppImage, kept across updates).
- Audio goes through PulseAudio/PipeWire; default latency target 64 ms, adjustable.
- The game runs at its own 60 Hz, which matches the Deck's panel; the default
  (exact game rate) and Display sync behave the same there.

## Packaging details

The binary links SDL2 2.30.9 statically and needs glibc >= 2.29 only; SDL loads
X11/Wayland/PulseAudio/ALSA from SteamOS at run time. See `docs/BUILD_LINUX.md`
("Release build") and `scripts/build-appimage.sh`. The AppImage contains no ROM
data.

## Things to check on real hardware

- Game Mode launch, fullscreen and focus when the Steam overlay opens/closes
  (focus loss pauses the game by default).
- That the game profile is found (About screen shows the profile path).
- Built-in controls and an external pad as Player 2.
- Suspend/resume, then open the quick menu.
