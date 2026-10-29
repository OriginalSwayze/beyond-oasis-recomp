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
| Cheats, launcher, in-game menu | next |

## Testing

The native build must behave exactly like the interpreter. Two checks:

```
BeyondOasis.exe --headless 6000 --verify <rom>        # every block vs. the interpreter: registers, flags, RAM, VDP, cycles
BeyondOasis.exe --headless 6000 --hash-log a.log <rom>
BeyondOasis.exe --headless 6000 --hash-log b.log --interpret <rom>
                                                      # a.log and b.log must be identical
```

Run both checks with `--widescreen 0`: the widescreen patches change the game on purpose.
With widescreen off, every patch falls back to the original code, so the results must match exactly.

Other headless options:

* `--press "FRAME:BUTTONS[:FRAMES],..."`: scripted input (U D L R A B C S), e.g. `600:S,1500:R:300`
* `--shot F.png` / `--shots EVERY DIR`: screenshots (PNG or BMP)
* `--widescreen 0|1`: override the setting for this run
* `--watch ADDR,...`: print RAM bytes (e.g. `173D` for $FF173D) whenever they change
* `--trace-vram LO HI`: log CPU writes and DMAs into a VRAM range, with the 68000 PC
* `--dump F`: save RAM, VRAM, CRAM, VSRAM and VDP registers
* `--hide-layers MASK`: hide plane A (1), plane B (2) or sprites (4)
* `--wav F.wav`, `--pc-profile N` (hottest interpreted code)

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
Settings live in `settings.ini` next to the executable.

## Third-party code

* [Musashi](https://github.com/kstenerud/Musashi) (MIT): 68000 interpreter fallback
* [SDL2](https://github.com/libsdl-org/SDL) (zlib): window, input, audio
* [z80](https://github.com/superzazu/z80) (MIT): Z80 sound CPU
* [ymfm](https://github.com/aaronsgiles/ymfm) (BSD-3-Clause): YM2612 FM synthesis
