# Beyond Oasis Recomp

A native PC port of **Beyond Oasis (USA, Sega Genesis)** built from your own ROM.
No game data is included in this repository: every tool reads your ROM and
checks its SHA-1 (`2944910c07c02eace98c17d78d07bef7859d386a`).

## Status

| Part | State |
|---|---|
| ROM verification (`tools/setup.mjs`) | done |
| 68000 decoder, code tracer, disassembly listing (`tools/disasm.mjs`) | done: ~97 KB of code in 8,365 blocks |
| Runtime: memory map, VDP with widescreen renderer, pads, SRAM, settings | done |
| Z80 + YM2612 + PSG sound | done |
| 68000 → C recompiler (`tools/recomp.mjs`, built automatically by CMake) | done: 99.98% of 68000 time runs as native C |
| Widescreen game patches (`runtime/src/widescreen.c`, `config/patches.json`) | done: gameplay at 16:9, other screens pillarboxed |
| Save states and rewind (`runtime/src/state.c`, `rewind.c`) | done |
| Launcher (`runtime/src/launcher.c`) | done: settings, ROM picker, foil steelbook |
| In-game menu (`runtime/src/menu.c`) | done: save/load slots with thumbnails, options, reset, quit |
| Internal resolution 2-4x (`runtime/src/hd.c`) | done: per-layer Scale2x/Scale3x |
| Smooth motion, 60-144 Hz+ (`runtime/src/hd.c`) | done: glides between the game's 30 Hz updates |
| Display filters (`runtime/src/filter.c`) | done: scanlines, CRT, LCD |
| Slowdown fix (`runtime/src/machine.c`) | done: 68000 at 150-300% |
| Cheats (`runtime/src/cheats.c`) | done: infinite health and spirit, unbreakable weapons, RAM codes |

## Testing

The native build must behave exactly like the interpreter. Two checks:

```
BeyondOasis.exe --headless 6000 --verify <rom>        # every block vs. the interpreter: registers, flags, RAM, VDP, cycles
BeyondOasis.exe --headless 6000 --hash-log a.log <rom>
BeyondOasis.exe --headless 6000 --hash-log b.log --interpret <rom>
                                                      # a.log and b.log must be identical
```

Run both checks with `--widescreen 0`: the widescreen patches change the game on purpose.

Headless runs start from blank save RAM and never write `save.srm`, so tests play the same
whatever you have saved, and never touch your save.
With widescreen off, every patch falls back to the original code, so the results must match exactly.

Other headless options:

* `--press "FRAME:BUTTONS[:FRAMES],..."`: scripted input (U D L R A B C S), e.g. `600:S,1500:R:300`
* `--shot F.png` / `--shots EVERY DIR`: screenshots (PNG or BMP)
* `--widescreen 0|1`: override the setting for this run
* `--watch ADDR,...`: print RAM bytes (e.g. `173D` for $FF173D) whenever they change
* `--trace-vram LO HI`: log CPU writes and DMAs into a VRAM range, with the 68000 PC
* `--dump F`: save RAM, VRAM, CRAM, VSRAM and VDP registers
* `--hide-layers MASK`: hide plane A (1), plane B (2) or sprites (4)
* `--state-check A B`: snapshot before frame A, run to B, reload and replay; both runs must end identical
* `--rewind-check K`: record every frame, rewind K frames; must equal the snapshot taken then
* `--launcher-shot F.png WIDTH LX LY`: render the launcher to a PNG with the light at (LX, LY), -1..1
* `--no-launcher`: start the game directly this time
* `--launcher-music-check SECONDS`: run the launcher music without an audio device and print its level per second and each loop's fade
* `--menu-shot F.png W H`: open the in-game menu over the last frame and render it at W x H
* `--save-to N`, `--slot N`, `--states-base PREFIX`: save a slot at the end, pick the menu's slot,
  and keep test slots in `PREFIXstates/` instead of the real ones
* `--hd-shot F.png SCALE`: save the last frame at internal resolution SCALE (2-4), with the time it took
* `--smooth-check DIR SCALE`: smooth motion over the last 8 frames at 120 Hz; prints how far each picture is between motion updates and the largest movement difference between touching, neighbouring sprites, and saves the pictures
* `--filter N`: use display filter N (0 off, 1 scanlines, 2 CRT, 3 LCD)
* `--filter-shot F.png W H MODE`: draw the last frame at W x H with filter MODE on the GPU (hidden window) and save it
* `--volume N`: volume 0-100 for this run
* `--overclock PERCENT`: run the 68000 at this speed (also below 100, to simulate a heavy scene); headless runs print the spare time, game ticks and late ticks
* `--load N`: load slot N (from the `--states-base` folder) before the first frame
* `--cheat health|spirit|weapons`: turn a built-in cheat on for this run
* `--poke FFxxxx:VV[VV],...`: hold RAM bytes or words every frame (applied after the built-in cheats)
* `--wav F.wav`, `--pc-profile N` (hottest code over the last N frames: cycles per native block, instructions for interpreted code)

## Launcher

Before the game starts, a launcher shows the settings (display, enhancements, ROM) next
to the game presented as a collector's steelbook: brushed steel with a holographic foil
frame, the title logo in gold foil and the Gold Armlet embossed over a sunburst. The
light follows the mouse like tilting a real case, and sways by itself when the mouse rests.

The artwork comes from your ROM: on first start the launcher plays the intro silently
(about two seconds), captures the armlet scene and the title screen, and caches them in
`launcher_art.cache` next to the executable. It is redrawn in software every frame,
split across CPU cores (about 8 ms per frame at 1080p).

The launcher plays the game's title theme, made live by your ROM's own sound driver on
the emulated Z80, YM2612 and PSG; no audio is stored anywhere. A background thread runs
the ROM from a reset (with blank save RAM, like the art capture) to the title screen
without sound, measures how long the title stays before the attract demo (740 frames,
12.3 s), then plays it in real time, fading in, and loops it with a short fade just
before the demo would start. M turns the music on and off (`launcher_music` in
`settings.ini`); pressing Play fades it out. The game then starts from a reset with
your real save RAM.

Keys: arrows select and change, Enter activates, M music, Esc quits; mouse and controller work
too. "Show launcher at startup" turns it off; `show_launcher=1` in `settings.ini` brings it back.

## Volume

"Volume" (launcher and pause menu, `volume` in `settings.ini`) is a slider from 0 to 100%
for the game's sound and the launcher music. Left/right move it in steps of 10%, and a
click on the track sets it there (in steps of 5%).

## In-game menu

Esc pauses the game over a blurred copy of the last frame. The menu saves and loads
the nine slots (each with a thumbnail and the time it was saved), changes the display
options and rewind on the spot, and resets or quits (both ask for a second press).

## Internal resolution

"Internal resolution" (launcher and pause menu, `internal_scale` in `settings.ini`,
default 3x) redraws the game at 2x, 3x or 4x of its 320x224 picture. The VDP keeps each
line's three layers (plane B, plane A with the window, sprites) with their colours and
priorities. `hd.c` scales each layer on its own with Scale2x or Scale3x (4x is 2x twice),
then composites them at that size with the VDP's own rules, priorities and
shadow/highlight included.

These scalers only copy whole source pixels, so outlines and diagonals get smoothed while
colours stay exact and layers never bleed into each other. Mid-frame palette changes and
line scrolling still work, because every line was captured with its own colours. The art
gains no new detail: it is the original art, drawn smoother at the window's size.

At 3x a frame takes about 4 ms (6-8 ms at 4x with widescreen) on one core. "Native" turns
it off and shows the original pixels. The native picture is unchanged, as are save-state
thumbnails and screenshots.

The last step to the window is "sharp bilinear" (`filter.c`): the picture is enlarged by
a whole number with hard pixel edges to just above the window's size, and only that last
small step is smoothed. A plain smooth stretch from 1272x672 to 1920x1080 softens every
edge; this keeps about 20% more edge contrast. On a 1080p or larger screen, 4x is the
sharpest choice, since it needs the smallest final step.

## Smooth motion

The game moves its camera and characters every other frame: its logic runs at 30 Hz,
in 2-pixel steps. "Smooth motion" (on by default, `smooth_motion` in `settings.ini`)
glides from one motion update to the next instead of jumping:

* The VDP also records each line's scroll values and every sprite: its position and all
  of its pixels, including those hidden behind other sprites.
* A frame where the scroll or a sprite moved is a motion update. Each picture is drawn
  a fraction `t` of the way from the previous update to the latest: plane A and B are
  shifted back by `(1 - t)` of their scroll change, and every sprite paired with the
  same sprite at the previous update (same tiles, size, attributes, near) is placed
  between its two positions. The window, the HUD and anything that did not move stay put.
* A character is several sprites. When its animation frame changes, the pieces with new
  tiles cannot be paired; they move with a touching paired piece next to them in the
  sprite list (or the nearest one), so the character glides as one instead of coming
  apart for a frame. Every picture is drawn this way, also when nothing moves, so
  overlapping sprites never switch between two ways of drawing.
* At 60 Hz this turns the 30 Hz steps into 60 Hz motion. On a faster display (120, 144 Hz
  and up) the window switches to vsync and draws a picture at every refresh.
* Scene cuts (scroll jumps over 24 pixels), column scrolling and sprite masking are
  drawn as they are, without gliding.

Motion is shown one update (about 33 ms) late. Smooth motion works at any internal
resolution; at Native it moves whole pixels. A picture takes about 3 ms at 3x (16:9) on
several cores, 5 ms on frames that also scale new layers.

## Display filters

"Filter" (launcher and pause menu, `filter=off|scanlines|crt|lcd` in `settings.ini`)
draws the picture with a screen look, on the GPU through SDL's renderer:

* **Scanlines**: a dark gap between the game's lines, lined up with its 224 lines at
  any window size (they fade out when a line is under three screen pixels tall).
* **CRT**: scanlines, red/green/blue aperture-grille stripes, a soft glow, a vignette
  and a gently curved picture.
* **LCD**: a thin grid between the game's pixels, like a handheld screen.

The scanlines, stripes, grid and vignette are one overlay texture, built once per
window size and multiplied over the picture. The glow is the picture drawn at 1/8 size
and added back stretched, which blurs it. The curve draws the picture and the overlay
as a bent 40x30 triangle mesh (`SDL_RenderGeometry`). Filters work with every internal
resolution and with smooth motion; screenshots and save thumbnails stay unfiltered.

## Slowdown fix

The game runs its logic every other frame (30 Hz). Each tick runs the game logic, waits
for the first V blank (`$8B72`), draws, and waits for the tick countdown at `$FF1654`
to run out (`$8ACA`). When a busy scene needs more than two frames of 68000 time, the
countdown has already run out at the end of the tick and the game slows down.

"Slowdown fix" (Off, 150%, 200%, 300% CPU; `overclock` in `settings.ini`) gives the
68000 that many more cycles per scanline. The Z80, the sound chips, the video timing and
the in-line position the VDP reports keep their real timing, so music and the frame rate
are unchanged; the game just finishes each tick sooner and waits longer.

Headless runs report it (`--overclock`): in the village, normal speed leaves 52% of
the time spare, 200% leaves 76%. Run at 40% to imitate a scene too heavy for the real
hardware and 230 of 300 ticks happen, all late: that is the slowdown the fix removes.
At 100% the machine is unchanged (`--verify` and native/interpreter logs match).

Moments that were CPU-bound, like area loading, also get shorter. Save states work at
any speed, but replays are only identical at the speed they were recorded at.

## Cheats

The pause menu has a toggle for each cheat; the launcher's "Cheats" row steps through
Off, Health, Health + spirit and All.

* **Infinite health.** The player's hit handler subtracts no damage (a recompiler patch
  at `$005884`, `playerDamage` in `config/patches.json`), and the health is held full.
* **Infinite spirit.** Spirit power is held full once Ali has a spirit.
* **Unbreakable weapons.** Breakable weapons keep their uses (a weapon on its last use
  gets two). Turning the cheat off leaves them with the count they had.

Extra RAM codes go in `settings.ini` as `cheat_codes=FFxxxx:VV,FFxxxx:VVVV,...`: two hex
digits write a byte, four write a word, every frame. Useful addresses:

| Address | Meaning |
|---|---|
| `$FF1A70` / `$FF1A72` | max / current health (word) |
| `$FF1A74` / `$FF1A76` | max / current spirit (word; max is 0 before the first spirit) |
| `$FF0D80` + 2k | weapon slot k (0-7): id byte, uses byte (`$FF` = never breaks) |
| `$FF0D90` + 2k | item slot k (0-15): id byte (0 = empty), then a second byte |

## Save states and rewind

A snapshot holds the whole machine between frames: both CPUs, all RAM, the VDP, I/O,
the sound chips and the scheduler (about 157 KB). Save states write one to
`states/slotN.state`; they load only into a build with the same state layout.

Rewind records a snapshot every frame as the change from the previous one (about 1 KB
per frame in play), up to `rewind_mb` in `settings.ini` (default 64). Holding the rewind
key steps back one frame per frame, redrawing each with the input it was played with.

## Widescreen

The game draws a 320-pixel screen; widescreen adds up to 80 pixels on each side
(16:9 adds 52). To fill them with real content during gameplay:

* **Map streaming.** The game keeps 41 columns of each 64-column scroll plane valid.
  `widescreen.c` replaces the column/row streamers, the area draw, the plane clear and the
  tile-change redraw with C versions that keep up to 11 more columns on each side.
* **Sprite culling.** Ten compares drop sprite pieces outside x = -32..320. The recompiler
  widens them through runtime variables (`config/patches.json`).
* **Camera limits.** The camera stops the extra width away from each map edge, so the
  wider view never shows past the map.
* **Other screens.** Logos, the title, menus and story scenes are made for 320 pixels and
  are pillarboxed in black. The extra width is shown while a map layer is loaded.

## How it fits together

1. `tools/disasm.mjs` traces code from the vector table, jump tables and code
   pointers, and writes `build/disasm/beyond_oasis.asm` and `build/analysis.json`.
2. The runtime runs the game. Code that has been recompiled (or rewritten by
   hand) runs as native C; anything else falls back to the Musashi interpreter.
   That keeps the game fully playable while more of it becomes readable C.
3. While you play, the runtime records which code ran (`coverage.bin`); the
   tracer uses it to find code it could not discover statically.

## Usage

```
node tools/setup.mjs "<path to Beyond Oasis (USA) ROM>"
node tools/disasm.mjs
cmake -S . -B build/cmake -G Ninja
cmake --build build/cmake
build/cmake/BeyondOasis.exe "<path to ROM>"
```

Keys: arrows, Z/X/C = A/B/C, Enter = Start, F5 = widescreen on/off, F11 = fullscreen.
Menu: Esc (or Back/Guide on a controller) pauses the game and opens the in-game menu.
Save states: F2 save, F4 load, F6/F7 previous/next slot (1-9, files in `states/`).
Rewind: hold Backspace (or the left shoulder button on a controller).
Settings live in `settings.ini` next to the executable.

## Third-party code

* [Musashi](https://github.com/kstenerud/Musashi) (MIT): 68000 interpreter fallback
* [SDL2](https://github.com/libsdl-org/SDL) (zlib): window, input, audio
* [z80](https://github.com/superzazu/z80) (MIT): Z80 sound CPU
* [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause): YM2612 FM synthesis
