# Controls

All game inputs can be remapped in **Controller Settings** (two keyboard keys
and one controller button/axis per input), with presets, dead zone and
"restore defaults". Controllers are hot-plugged. Menu input is never passed
to the game: while a menu is open the game is paused, and buttons held when it
closes are ignored until released.

## Default keyboard

| Input | Player 1 | Player 2 |
|---|---|---|
| Up / Down / Left / Right | Arrows (or W/S/A/D) | I / , / U / O |
| Shot (Button 1) | Z (or J) | Keypad 1 |
| Bomb (Button 2) | X (or K) | Keypad 2 |
| Button 3 | C (or L) | Keypad 3 |
| Start | Enter (or 1) | 2 |
| Coin | 5 | 6 |

Cabinet: Service coin = `9`, Service Mode (test switch) = `F2`.

Frontend hotkeys (remappable under Controller Settings > Hotkeys): `Esc` quick
menu, `F5` quick save, `F9` quick load, `F6`/`F7` previous/next slot,
`F12` screenshot, `F11` fullscreen, `Backspace` (hold) rewind, `Pause` or `P`
pause, `F10` FPS counter, `F3` reset.

## Default controller (Xbox / PlayStation / Steam Deck / arcade stick)

Each player uses the controller assigned under Controller Settings > Player
Controllers (automatic: 1st controller = P1, 2nd = P2).

| Input | Button |
|---|---|
| Move | D-pad or left stick |
| Shot / Bomb / Button 3 | A / B / X (Cross / Circle / Square) |
| Start | Start / Menu (Options) |
| Coin | Back / View (Share) |
| Quick menu | Back + Start held together (configurable) |

The board has three buttons per player (MAME's `dragnblz` input definition).
Shot (button 1) and bomb (button 2) were exercised in scripted runs that match
MAME; what button 3 does in the game was not checked.

## Service menu

Press `F2` (the board's test switch) to enter the game's Test Mode (screen,
input and sound tests, Game Setting, Maintenance Code, Exit). Difficulty,
lives, coinage and similar settings are changed under Game Setting and kept in
the EEPROM, which the frontend stores between sessions (`nvram/` in the data
folder). Move with P1 up/down, select with button 1.

## Board switches (Gameplay Settings)

The PS5 board has no DIP switches for game settings. The frontend offers what
MAME documents:

| Switch | Values | Default | Notes |
|---|---|---|---|
| Region (JP4 jumpers) | Japan / International Ver. A / International Ver. B | International Ver. B | read by the game at power-on: applies after a reset |
| Debug switch | Off / On | Off | MAME labels it "Tilt (Enables Debug Mode)"; MAME's notes say it enables the ROM test and skips the EEPROM security check. Leave it off for normal play. |
