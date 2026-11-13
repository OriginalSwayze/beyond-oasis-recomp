/* Shared state and interfaces for the Genesis hardware runtime. */
#ifndef GENESIS_H
#define GENESIS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ROM_SIZE        0x300000
#define SRAM_SIZE       0x4000          /* bytes on the odd addresses $200001-$203FFF */
#define LINES_NTSC      262
#define LINES_PAL       313
/* Seconds per frame of the console the loaded cartridge expects (PAL: 50 Hz). */
double machine_frame_s(void);
#define CYCLES_PER_LINE 488             /* 68000 clocks per scanline (53.693175 MHz / 7 / 15.7 kHz) */
#define MAX_EXTRA_WIDTH 80              /* widescreen pixels per side, at most: the game streams 41 of the
                                           plane's 64 columns, leaving room for 11 more on each side */

/* ---- Bus (bus.c) ---------------------------------------------------------- */
typedef struct {
    uint8_t rom[ROM_SIZE];
    uint8_t ram[0x10000];
    uint8_t sram[SRAM_SIZE];
    bool sram_mapped;                   /* $A130F1 bit 0 */
    bool sram_dirty;
    uint8_t zram[0x2000];               /* Z80 RAM, visible to the 68000 at $A00000 */
    bool z80_busreq, z80_reset;
} Bus;

extern Bus bus;

/* Returns NULL on success, else a message describing why the ROM was rejected. Any version
 * this build has loads, and becomes `game` (game.h). */
const char *bus_load_rom(const char *path);
struct GameVersion;
const struct GameVersion *bus_rom_version(const char *path);   /* which version a file is, or NULL */
uint8_t  bus_read8(uint32_t a);
uint16_t bus_read16(uint32_t a);
void     bus_write8(uint32_t a, uint8_t v);
void     bus_write16(uint32_t a, uint16_t v);

/* ---- VDP (vdp.c) ---------------------------------------------------------- */
typedef struct {
    uint8_t  vram[0x10000];             /* byte-addressed, big-endian like the real VRAM */
    uint16_t cram[64];
    uint16_t vsram[40];
    uint8_t  reg[32];
    uint16_t addr;
    uint8_t  code;
    bool     pending;                   /* first half of a control word received */
    bool     fill_pending;
    uint16_t status;
    int      line;                      /* current scanline */
    int      hint_counter;
    bool     vint_pending, hint_pending;
    int      line_cycle_start;          /* CPU cycle count at the start of the line, for HV/HBlank */
} Vdp;

/* Display / enhancement options the renderer reads every line. */
typedef struct {
    int  extra_width;                   /* widescreen pixels on each side (0 = original 320) */
    bool no_sprite_limit;               /* draw past the 20-per-line hardware limit (no flicker) */
    bool mask_sides;                    /* draw the widescreen columns black (screens made for 320) */
    int  hide_layers;                   /* debugging: bit 0 plane A, 1 plane B, 2 sprites, 3 window */
    bool hd_capture;                    /* keep each line's layers for hd_compose() (hd.h) */
    bool outfit;                        /* draw the player's sprites with vdp_outfit's colours */
    bool tile_refs;                     /* keep each pixel's tile reference for texture packs (texpack.h) */
    int  tone;                          /* VDP_TONE_*: the colours' tone (vdp.c) */
    int  color_blind;                   /* VDP_BLIND_*: colours moved where that eye can tell them apart */
} VdpOptions;
enum { VDP_TONE_ORIGINAL, VDP_TONE_WARM, VDP_TONE_COOL, VDP_TONE_VIVID, VDP_TONES };
enum { VDP_BLIND_OFF, VDP_BLIND_PROTAN, VDP_BLIND_DEUTAN, VDP_BLIND_TRITAN, VDP_BLINDS };

/* The player character's outfit colours (vdp.c). Some colours are shared by two parts of
 * him (his hair and the Gold Armlet, his headband and boots), so his body is split by
 * height, counted up from his feet (the bottom of his sprites' box, which his sword does
 * not move): the head, the body and the feet (outfit_band). For each band and colour index
 * (palette * 16 + colour), vdp_outfit_pick is the colour picked for it or OUTFIT_KEEP and
 * vdp_outfit_anim its animation (OUTFIT_ANIM_*); vdp_outfit_ref is the brightness
 * (r + g + b, 0-21) of the colour the index stands for: each line the index gets the pick
 * scaled by how bright the game's CRAM colour is now, so shades keep their depth and fades
 * still darken him. Which sprites are his comes from motion.c. The last frame he was on
 * screen is kept in vdp_player (colour indices, 0 = none) with that frame's CRAM, for
 * previews. */
#define OUTFIT_KEEP 0xFFFF
#define PLAYER_IMG 128
enum { OUTFIT_HEAD, OUTFIT_BODY, OUTFIT_FEET, OUTFIT_BANDS };
enum { OUTFIT_ANIM_NONE, OUTFIT_ANIM_SHIMMER, OUTFIT_ANIM_PULSE, OUTFIT_ANIM_RAINBOW, OUTFIT_ANIM_FLAME, OUTFIT_ANIMS };
extern uint16_t vdp_outfit_pick[OUTFIT_BANDS][64];
extern uint8_t  vdp_outfit_anim[OUTFIT_BANDS][64];
extern uint8_t  vdp_outfit_ref[64];
/* The band of a row of his, by how far above his feet it is (0 = the lowest row). */
static inline int outfit_band(int above_feet) { return above_feet >= 33 ? OUTFIT_HEAD : above_feet < 12 ? OUTFIT_FEET : OUTFIT_BODY; }
/* The colour an index gets: the pick, animated at this row and moment (frames at 60 Hz),
 * at the brightness of the game's colour now. */
uint16_t vdp_outfit_color(uint16_t pick, int anim, uint16_t now, int ref, int above_feet, unsigned frame);
extern uint8_t  vdp_player[PLAYER_IMG * PLAYER_IMG];
extern uint16_t vdp_player_cram[64];
extern int      vdp_player_w, vdp_player_h;   /* 0 until he has been on screen */
extern int      vdp_player_x, vdp_player_y;   /* where that picture was on screen (x from the left of the 320 area) */
uint16_t vdp_outfit_shade(uint16_t pick, uint16_t now, int ref);   /* the colour an index gets */
uint32_t vdp_color_rgb(uint16_t cram);                             /* 0x00RRGGBB, as shown (tone and colour-blind help) */
uint32_t vdp_color_raw(uint16_t cram);                             /* 0x00RRGGBB, as the console makes it */

extern Vdp vdp;
extern VdpOptions vdp_opts;

void     vdp_reset(void);
uint16_t vdp_read16(uint32_t a);
void     vdp_write16(uint32_t a, uint16_t v);
uint8_t  vdp_read8(uint32_t a);
void     vdp_write8(uint32_t a, uint8_t v);
int      vdp_screen_width(void);        /* 256 or 320, before widescreen */
int      vdp_screen_height(void);       /* 224 or 240 */
/* Renders one active line into dst (0x00RRGGBB), width = screen width + 2*extra_width. */
void     vdp_render_line(int line, uint32_t *dst);
/* Final colour of one pixel from its plane A, plane B and sprite layer pixels (hd.h). */
uint32_t vdp_compose(uint32_t a, uint32_t b, uint32_t s, uint16_t backdrop, bool sh);
int      vdp_irq_level(void);

/* ---- Controllers (io.c) --------------------------------------------------- */
enum {
    PAD_UP = 1 << 0, PAD_DOWN = 1 << 1, PAD_LEFT = 1 << 2, PAD_RIGHT = 1 << 3,
    PAD_B = 1 << 4, PAD_C = 1 << 5, PAD_A = 1 << 6, PAD_START = 1 << 7,
    PAD_Z = 1 << 8, PAD_Y = 1 << 9, PAD_X = 1 << 10, PAD_MODE = 1 << 11,
};

extern uint16_t pad_state[2];           /* PAD_* bits, 1 = pressed */

typedef struct { uint8_t data[3], ctrl[3]; } IoState;   /* I/O port data and direction registers */
extern IoState io;

uint8_t io_read8(uint32_t a);
void    io_write8(uint32_t a, uint8_t v);

/* ---- Z80 (z80bus.c) and sound chips (sound.cpp) ------------------------------ */
#define SOUND_OUT_MAX 4096              /* stereo frames buffered per emulated frame */

void z80_reset_cpu(void);
void z80_run(int m68k_cycles);
void z80_set_irq(bool on);

extern int16_t sound_out[SOUND_OUT_MAX * 2];
extern int     sound_out_count;         /* stereo frames ready in sound_out */
extern int     sound_out_rate;
extern bool    sound_fm_enabled, sound_psg_enabled;
/* Output quality: 0 as before (linear resampling); 1 clean: band-limited resampling,
 * anti-aliased PSG, no DC offset; 2 rich: clean with wider stereo and a light room reverb.
 * Only the sound that comes out changes, never the machine's state. */
extern int     sound_quality;
extern bool    sound_pal;              /* PAL console clocks (set before sound_reset) */
void    sound_end_frame(void);          /* machine.c, after each frame: the output effects */
void    sound_reset(void);
void    sound_advance(int m68k_cycles);
void    ym_write(int port, uint8_t v);
uint8_t ym_read(void);
void    psg_write(uint8_t v);

/* ---- CPU / machine (machine.c) -------------------------------------------- */
typedef struct {
    bool recomp;                        /* run recompiled blocks (otherwise interpret everything) */
    bool verify;                        /* check each recompiled block against the interpreter */
    int  overclock;                     /* 68000 speed in percent of the real clock (100 = original);
                                         * the Z80, sound and video keep their own timing */
} MachineOptions;
extern MachineOptions machine_opts;
extern uint32_t verify_checked, verify_failed;
/* 68000 cycles spent in the game's main loop waits (machine.c is_wait): the
 * time to spare. When it reaches 0 the game slows down. */
extern uint64_t stat_wait_cycles, stat_total_cycles;
extern uint32_t stat_ticks, stat_late_ticks;    /* game ticks, and ticks that ran late (slowdown) */
extern uint64_t stat_native_cycles, stat_interp_cycles;

void machine_reset(void);
void machine_run_frame(uint32_t *framebuffer, int pitch_pixels);
int  machine_cycles_in_line(void);      /* cycles executed since the current line started */

/* ---- Snapshots (state.c) ------------------------------------------------------
 * Taken between frames, for save states and rewind. Each module with private state
 * reads or writes it through state_io(). */
#define STATE_MAX (512 * 1024)          /* bytes; a snapshot is about 160 KB */
typedef struct { uint8_t *buf; size_t pos, cap; bool saving, ok; } StateIO;
void   state_io(StateIO *s, void *p, size_t n);
size_t state_save(uint8_t *buf, size_t cap);        /* returns the size, 0 on failure */
bool   state_load(const uint8_t *buf, size_t len);  /* false (and no change) if it does not fit this build */
void   z80_state(StateIO *s);                       /* z80bus.c */
void   sound_state(StateIO *s);                     /* sound.cpp */
void   machine_state(StateIO *s);                   /* machine.c */

/* Rewind (rewind.c): rewind_push() before each frame; rewind_step() goes back one. */
void rewind_reset(size_t budget_bytes);             /* 0 turns rewind off */
void rewind_push(uint16_t pad);
bool rewind_step(uint16_t *pad);
void rewind_restore_head(void);
int  rewind_frames(void);
size_t rewind_bytes(void);

/* ---- Game patches ------------------------------------------------------------ */
void widescreen_patches(int extra_width);   /* widescreen.c: 0 restores the original code */
void widescreen_frame(void);                /* widescreen.c: per-frame decisions (side masking) */

#endif
