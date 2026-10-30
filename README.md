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
| Launcher (`runtime/src/launcher.c`, `launcher_scene.c`) | done: settings, ROM picker, animated armlet and spirits, soundtrack player |
| In-game menu (`runtime/src/menu.c`) | done: save/load slots with thumbnails, options, reset, quit |
| Internal resolution 2-4x (`runtime/src/hd.c`) | done: per-layer Scale2x/Scale3x |
| Smooth motion, 60-144 Hz+ (`runtime/src/hd.c`, `motion.c`) | done: moves every sprite by its object's movement between the game's 30 Hz ticks |
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
* `--launcher-shot F.png WIDTH TAB TIME`: render the launcher to a PNG with tab TAB (0-3) open, TIME seconds into its animations
* `--no-launcher`: start the game directly this time
* `--launcher-music-check SECONDS`: run the soundtrack player without an audio device; prints the title's song, each song change and the level every 30 seconds
* `--ost-check SECONDS`: play every song for SECONDS and print its level per second (in tenths of its loudest second), then the menu sound effects' levels
* `--menu-shot F.png W H`: open the in-game menu over the last frame and render it at W x H
* `--controls-shot`: with `--menu-shot`, render the Controls page over the menu
* `--input-check FILE`: rebind a few keys and buttons (including clashes), save them to FILE, load them back and print every binding
* `--save-to N`, `--slot N`, `--states-base PREFIX`: save a slot at the end, pick the menu's slot,
  and keep test slots in `PREFIXstates/` instead of the real ones
* `--hd-shot F.png SCALE`: save the last frame at internal resolution SCALE (2-4), with the time it took
* `--sprite-check EVERY DIR SCALE`: every EVERY frames, compare the frame drawn layer by layer with the same frame drawn sprite by sprite (as smooth motion draws it); prints how many differ by more than 0.5% and saves the worst pair
* `--smooth-check DIR SCALE`: smooth motion over the last 8 frames at 120 Hz; prints how far each picture is between ticks and how many sprites follow objects, and saves the pictures
* `--motion-check`: from frame 300 on, check that every frame's sprites are found in a tick the game built; prints how many sprites follow an object and the largest move
* `--filter N`: use display filter N (0 off, 1 scanlines, 2 CRT, 3 LCD)
* `--sharpen N`: sharpening 0-3 for this run (shows in `--filter-shot`)
* `--lighting N` (0-2), `--ambience 0|1`, `--scaler 0|1` (edge, smooth), `--audio N` (0 original, 1 clean, 2 rich): the effects for this run
* `--outfit a,b,c,d`: Ali's outfit colours for this run (as `outfit_colors` in `settings.ini`)
* `--outfit-check F.png`: at the end, save Ali's sprites 6x in his current colours and list the colour indices he uses
* `--outfit-shot`: with `--menu-shot`, render the outfit page over the menu
* `--menu-tab TAB ROW`: with `--menu-shot`, open tab TAB (0 game, 1 display, 2 effects, 3 more) with row ROW selected (shows its preview card)
* `--save-preview`: at the end, save the frame for the launcher's previews (`preview_frame.cache`)
* `--filter-shot F.png W H MODE`: draw the last frame at W x H with filter MODE on the GPU (hidden window) and save it
* `--volume N`: volume 0-100 for this run
* `--overclock PERCENT`: run the 68000 at this speed (also below 100, to simulate a heavy scene); headless runs print the spare time, game ticks and late ticks
* `--load N`: load slot N (from the `--states-base` folder) before the first frame
* `--cheat health|spirit|weapons`: turn a built-in cheat on for this run
* `--poke FFxxxx:VV[VV],...`: hold RAM bytes or words every frame (applied after the built-in cheats)
* `--wav F.wav`, `--pc-profile N` (hottest code over the last N frames: cycles per native block, instructions for interpreted code)

## Launcher

Before the game starts, a launcher shows the settings over a desert at dusk: the sun sets
behind the dunes, stars twinkle, sand blows across and embers rise from the Gold Armlet,
which floats in the middle with a sheen sweeping over it. The four spirits circle the
armlet as glowing orbs with their emblems: Dytto (water), Efreet (fire), Bow (plant) and
Shade (shadow).

The settings sit on a stone tablet with gold trim and one tab per spirit: DISPLAY,
ENHANCE, GAME and MUSIC. Opening a tab turns its spirit to the front of the circle and
slides its rows in; the highlight glides between rows, a changed value pulses, and the
selected option's description shows under the rows.

The artwork comes from your ROM: on first start the launcher plays the intro silently
(about two seconds), captures the armlet scene and the title screen, and caches them in
`launcher_art.cache` next to the executable. The spirits' orbs and emblems are drawn from
shapes. Everything is drawn in software every frame (about 5 ms at 720p, 15 ms at 1080p;
larger windows are drawn at 1080p and scaled up).

### Soundtrack player and sounds

The launcher plays the game's soundtrack, made live by your ROM's own sound driver on the
emulated Z80, YM2612 and PSG; no audio is stored anywhere (`runtime/src/ost.c`):

* A background thread runs the ROM from a reset (with blank save RAM, like the art capture)
  to the title screen without sound, noting the song the title asks for (song $04).
* It then parks the game (the 68000 loops on a `bra.s *` in work RAM with interrupts
  masked) and calls the sound driver itself, as the game does: `jsr $060000` once a frame
  to play the music and `jsr $060004` with a command to start a song ($01nn) or a sound
  effect. The calls return to a hand-written routine at an address in the cartridge header.
* The ROM's song table at $0638D4 has 34 songs ($00-$21). Looping songs play for three
  minutes and fade out; jingles end when they go quiet (measured without the FM DAC's DC
  offset). The player plays them all in order, repeats one, or shuffles (`ost_mode`), and
  remembers the last song (`ost_track`).
* The MUSIC tab shows the song, its time, a level meter and previous / play-stop / next
  buttons; the song playing also shows under the armlet on every tab.
* Every choice in the launcher makes the game's own menu sound: the cursor sound ($0256)
  when moving, the confirm sound ($0347) when changing or opening something, the cancel
  sound ($03AA) when closing the controls page, and the error sound ($0253) when Play is
  pressed without a ROM. They play even with the music off.

Keys: arrows select and change, Enter activates, Q / E (or Page Up / Down, LB / RB) switch
tabs, [ and ] change the song, M turns the music on and off, Esc quits; the mouse and a
controller work too. Pressing Play fades the music out; the game then starts from a reset
with your real save RAM. "Show launcher at startup" turns the launcher off;
`show_launcher=1` in `settings.ini` brings it back.

## Volume

"Volume" (launcher and pause menu, `volume` in `settings.ini`) is a slider from 0 to 100%
for the game's sound and the launcher music. Left/right move it in steps of 10%, and a
click on the track sets it there (in steps of 5%).

## In-game menu

Esc pauses the game over a blurred, warmed copy of the last frame with embers drifting
over it. The menu sits on the launcher's stone tablet, with one tab per spirit: GAME
(resume, save and load the nine slots, cheats, rewind, reset, quit), DISPLAY, EFFECTS and
MORE (volume, audio, Controls, Ali's outfit). Q / E, Page Up / Down or LB / RB switch tabs;
the rows slide in, the highlight glides, and a changed value pulses.

The card on the right follows the selected row: the save slot's thumbnail and when it was
saved; for a picture option, a live before/after on the paused frame (`runtime/src/preview.c`):
the renderer keeps every frame's layers, so the frame is drawn again without the option and
with it (its value, or a typical one when it is off), joined by a dividing line that sweeps
slowly across. Detail options (internal resolution, scaler, sharpening) show 1:1 pixels
around the middle; scene-wide ones (lighting, ambience) show the whole frame. Every other
row explains what it changes. The launcher shows the same previews over its scene, on the
frame the game saved last (`preview_frame.cache`, written when the menu opens and on quit).

Every choice makes the game's own menu sound (cursor, confirm, cancel, error). The game
cannot be asked to play them while it is paused without changing it, so they are rendered
once from the ROM, the way the launcher's player does (`menu_sounds.cache`, about half a
second on the first start), and played straight into the audio queue.

All the enhancements are off by default, so the game looks and sounds as it did on the
console until you turn something on.

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

The game moves its camera and characters once per tick, every other frame: its logic
runs at 30 Hz, in 2-pixel steps. "Smooth motion" (on by default, `smooth_motion` in
`settings.ini`) draws the moments between two ticks from the game's own data instead of
guessing from the picture:

* While the game builds its sprite list (`runtime/src/motion.c`), the runtime notes for
  every sprite which object drew it and where the game placed that object on screen.
  Objects draw through three routines that first work out the object's screen position
  ($AB4A, $AECC, $B0C4) and small effects through one ($A98E); the V blank handler sends
  the finished list to the VDP ($1FBE), which ends a tick. The HUD, menus and text are
  drawn other ways, belong to no object and never move between ticks.
* The VDP also records each line's scroll values and every sprite: its position and all
  of its pixels, including those hidden behind other sprites. The sprites shown are found
  in the tick they came from by their entries in the sprite list.
* Each picture is drawn a fraction `t` of the way from the previous tick to the latest:
  plane A and B are shifted back by `(1 - t)` of their scroll change, and every sprite by
  `(1 - t)` of its own object's movement. All the pieces of a character belong to the same
  object, so they always move together, through animation changes too, and a character
  can never disappear, double or come apart. An object that is new, whose slot now holds
  a different kind of object, or that moved more than 48 pixels in one tick (a warp) is
  drawn where it is.
* Sprites are stacked front to back by their position in the sprite list, exactly as
  the hardware does (`--sprite-check` compares this per-sprite drawing with the layer
  drawing; they match to within 0.07% of pixels, at overlapping edges). Every picture is
  drawn this way, also when nothing moves, so overlaps never switch between two ways of
  drawing.
* The main loop runs at the display's refresh with vsync: every refresh runs a game frame
  if one is due (60 a second on any display) and draws the moment it is now. So 30 Hz
  steps become 60 Hz motion on a 60 Hz display and 144 Hz motion on a 144 Hz one.
* Scene cuts (scroll jumps over 24 pixels), column scrolling and sprite masking are
  drawn as they are, without gliding.

Motion is shown one tick (about 33 ms) late. Smooth motion works at any internal
resolution; at Native it moves whole pixels. The game's own logic stays at 30 Hz: running
it every frame would make the whole game run twice as fast, and halving every speed,
timer and animation in the game to make up for it is a far bigger change.

A picture takes 1.5-3 ms at 4x (16:9), split across cores by a pool of worker threads
started once. Measured live on a 144 Hz display: 144 pictures a second, 90% of them
6.7-7.2 ms apart (6.94 is perfect), the slowest 10.4 ms.

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

## Scaler, lighting and ambience

"Scaler" (Edge, Smooth; `scaler=edge|smooth` in `settings.ini`) picks how the layers are
scaled for the internal resolution. Edge is Scale2x / Scale3x. Smooth uses Hyllian's xBR
rules: it looks at a 5x5 neighbourhood to tell along which diagonal an edge runs and
rounds each corner on the right side of it, so curves and shallow slopes come out cleaner.
The usual xBR blends colours at edges; this one takes a whole neighbouring source pixel
instead, so every output pixel is still a real layer pixel and the layers, priorities and
transparency composite exactly as before. Corners whose two candidate neighbours equal the
pixel itself are skipped, and the layers are split into bands over all cores: a 4x frame
takes about 4 ms, like Edge.

"Lighting" (Off, Soft, Bright; `lighting`) makes light glow: near-white and vivid, bright
pixels (fire, magic, gems, the HUD's gold) are gathered into a picture an eighth the size,
blurred at two sizes and laid over the picture with a screen blend. Sunlit ground is
bright but neither, so daylight scenes do not wash out.

"Ambience" (`ambience`) adds a soft vignette, and in the game world dust motes drifting
in the air; they are placed in the world (the camera position is read from the plane A
layer struct at $FF1716), so they stay put as the camera moves. Menus and story scenes get
the vignette only.

These, and sharpening, run on the picture before it goes to the GPU (`hd_post`), so the
display filters still apply on top. None of them change the game.

## Audio

"Audio" (Original, Clean, Rich; `audio` in `settings.ini`, Original by default) changes only
the sound that comes out (`runtime/src/sound.cpp`), never the machine's state, so save
states and replays are the same in every mode:

* Clean: the YM2612's 53 kHz output is resampled to the device rate with a 16-tap
  windowed-sinc filter instead of straight lines, so nothing above the output's Nyquist
  frequency folds back as noise; each PSG square wave is averaged over every sample
  instead of read at one instant, which keeps its high notes from aliasing; a DC blocker
  removes the offset the FM DAC holds (about 2500 of 32767), and the mix keeps headroom
  and ends in a soft limiter instead of clipping (the original clips about 1400 times a
  second in the village at full volume).
* Rich: Clean, with the stereo widened a little and a light room reverb (four comb and
  two all-pass filters per side, after Freeverb) mixed in quietly.

## Sharpening

"Sharpening" (Off, Light, Medium, Strong; `sharpen` in `settings.ini`) sharpens the
picture before it goes to the GPU, at the internal resolution (or the native one), split
across CPU cores (`hd_sharpen` in `runtime/src/hd.c`). It is contrast-adaptive, after
AMD's CAS: each pixel is pushed away from its four neighbours, by less where the contrast
is already high, so the soft edges of the scaled layers and of the final stretch to the
window get crisper while hard pixel edges get no halos or ringing. On Strong, about one
pixel in eight of a village frame at 3x changes.

## Ali's outfit

"Ali's outfit" (in the pause menu and on the launcher's GAME tab) recolours the clothes
of the player character (`runtime/src/outfit.c`):

* Every sprite in the game uses palette line 0, so changing a CRAM colour would recolour
  the townspeople with him. Instead the renderer recolours only the sprites his object
  drew: motion.c already records which object each sprite-table entry belongs to, and the
  VDP marks his pixels whose colour index has an outfit colour (bit 7 of the sprite byte,
  unused by the hardware) and takes that colour instead of CRAM's.
* His clothes are ramps of colour indices, brightest first: trousers 15, 13; sash 2, 1;
  boots and wrist bands 7, 6, 5; hair 4, 3 (the Gold Armlet on his arm shares the hair's
  colours). One colour is picked per part; each index gets it scaled by how bright the
  game makes that index right now, so folds and shading stay and the game's fades still
  darken him.
* The page shows him large in the colours being picked, next to how he looks now, and
  changes nothing until Apply (Reset goes back to his own colours, Cancel leaves). Each
  part steps through named colours with left and right; Enter mixes your own from red,
  green and blue in the Genesis's eight steps each. The launcher shows the picture of
  him the game saved last time (`player_preview.cache` next to the executable).
* `outfit` and `outfit_colors` (four CRAM colours in hex, `-` for his own) in
  `settings.ini` hold the choice.

## Cheats

The "Cheats" row (pause menu and launcher) steps through Off, Health, Health + spirit
and All; `cheat_health`, `cheat_spirit` and `cheat_weapons` in `settings.ini` set each one.

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

Default controls (all remappable, see Controls below):

| Action | Keyboard | Controller (Xbox layout) |
|---|---|---|
| Move | Arrows | D-pad (the left stick always works too) |
| A / B / C | Z / X / C | X / A / B |
| Start | Enter | Start |
| Rewind (hold) | Backspace | LB |
| Pause menu | Esc | Back (Guide and Esc always work too) |

Fixed keys: F2 save state, F4 load, F6/F7 previous/next slot (1-9, files in `states/`),
F5 widescreen on/off, F11 fullscreen. Settings live in `settings.ini` next to the executable.

## Controls

"Controls" in the pause menu and in the launcher opens a page with every action's key and
controller button. Pick a row, press Enter (or A, or click) and then the new key or button:
a key binds the keyboard column, a button the controller column. A key or button another
action had is swapped onto that action's old one, so no two actions ever share. Esc
cancels, and "Reset to defaults" puts everything back. The menus keep their own fixed
keys (arrows / D-pad, Enter / A, Esc / B), and Esc and the Guide button always open the
pause menu, so a binding can never lock you out. Bindings are saved in `settings.ini` as
`key_<action>` and `pad_<action>` (SDL key and button names). Any controller SDL knows
works: Xbox, PlayStation, Switch Pro and most others; the first one connected is used.

## Third-party code

* [Musashi](https://github.com/kstenerud/Musashi) (MIT): 68000 interpreter fallback
* [SDL2](https://github.com/libsdl-org/SDL) (zlib): window, input, audio
* [z80](https://github.com/superzazu/z80) (MIT): Z80 sound CPU
* [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause): YM2612 FM synthesis
