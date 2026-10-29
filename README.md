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
| Widescreen game patches, cheats, launcher, in-game menu | next |

## Testing

The native build must behave exactly like the interpreter. Two checks:

```
BeyondOasis.exe --headless 6000 --verify <rom>        # every block vs. the interpreter: registers, flags, RAM, VDP, cycles
BeyondOasis.exe --headless 6000 --hash-log a.log <rom>
BeyondOasis.exe --headless 6000 --hash-log b.log --interpret <rom>
                                                      # a.log and b.log must be identical
```

Other headless options: `--shot F.bmp`, `--wav F.wav`, `--pc-profile N` (hottest interpreted code).

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
