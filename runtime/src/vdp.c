/* Sega 315-5313 VDP: ports, DMA, interrupts and a scanline renderer.
 * The renderer can draw extra columns on both sides of the 320-pixel screen
 * (vdp_opts.extra_width) for widescreen; the game still thinks it is 320 wide. */
#include "genesis.h"
#include "hd.h"
#include "motion.h"
#include "texpack.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "m68k.h"

/* Debugging (--trace-vram LO HI): log CPU writes and DMAs that touch VRAM [lo, hi). */
uint32_t vdp_trace_lo, vdp_trace_hi;
static void trace_vram(const char *kind, uint32_t dst, uint32_t len, uint32_t src) {
    if (vdp_trace_hi <= vdp_trace_lo || (vdp.code & 0x0F) != 1) return;
    uint32_t end = dst + len * (vdp.reg[15] ? vdp.reg[15] : 1);
    if (end <= vdp_trace_lo || dst >= vdp_trace_hi) return;
    fprintf(stderr, "%s dst=%04X len=%u inc=%u src=%06X pc=%06X\n", kind, dst, len, vdp.reg[15], src,
            m68k_get_reg(NULL, M68K_REG_PPC));
}

Vdp vdp;
VdpOptions vdp_opts;

/* Measured VDP DAC levels for 3-bit channels: normal, shadow and highlight. */
static const uint8_t LEVEL_NORMAL[8]    = { 0, 52, 87, 116, 144, 172, 206, 255 };
static const uint8_t LEVEL_SHADOW[8]    = { 0, 29, 52, 70, 87, 101, 116, 130 };
static const uint8_t LEVEL_HIGHLIGHT[8] = { 130, 144, 158, 172, 187, 206, 228, 255 };

static uint16_t vram16(uint32_t a) {
    a &= 0xFFFE;
    return (uint16_t)(vdp.vram[a] << 8 | vdp.vram[a + 1]);
}

static int player_w, player_h;

void vdp_reset(void) {
    memset(&vdp, 0, sizeof vdp);
    vdp_player_w = vdp_player_h = player_w = player_h = 0;   /* nothing of the last run in previews */
}

int vdp_screen_width(void)  { return (vdp.reg[12] & 0x81) ? 320 : 256; }
int vdp_screen_height(void) { return (vdp.reg[1] & 0x08) ? 240 : 224; }

int vdp_irq_level(void) {
    if (vdp.vint_pending && (vdp.reg[1] & 0x20)) return 6;
    if (vdp.hint_pending && (vdp.reg[0] & 0x10)) return 4;
    return 0;
}

/* ---- Port writes ------------------------------------------------------------ */
static void write_target(uint16_t v) {
    switch (vdp.code & 0x0F) {
    case 0x1: { /* VRAM: odd addresses write the bytes swapped, like the hardware */
        uint16_t a = vdp.addr & 0xFFFE;
        if (vdp.addr & 1) v = (uint16_t)(v << 8 | v >> 8);
        vdp.vram[a] = (uint8_t)(v >> 8);
        vdp.vram[a + 1] = (uint8_t)v;
        break;
    }
    case 0x3: vdp.cram[(vdp.addr >> 1) & 0x3F] = v & 0x0EEE; break;
    case 0x5: {
        int i = (vdp.addr >> 1) & 0x3F;
        if (i < 40) vdp.vsram[i] = v & 0x07FF;
        break;
    }
    }
    vdp.addr += vdp.reg[15];
}

static uint32_t dma_length(void) {
    uint32_t len = vdp.reg[19] | vdp.reg[20] << 8;
    return len ? len : 0x10000;
}

static void dma_68k(void) {
    uint32_t len = dma_length();
    uint32_t src = (uint32_t)(vdp.reg[23] & 0x7F) << 17 | vdp.reg[22] << 9 | vdp.reg[21] << 1;
    trace_vram("dma", vdp.addr, len, src);
    for (uint32_t i = 0; i < len; i++) {
        write_target(bus_read16(src));
        /* The source address wraps inside a 128 KB window. */
        src = (src & 0xFE0000) | ((src + 2) & 0x1FFFF);
    }
    vdp.reg[19] = vdp.reg[20] = 0;
    vdp.reg[21] = (uint8_t)(src >> 1);
    vdp.reg[22] = (uint8_t)(src >> 9);
    vdp.reg[23] = (vdp.reg[23] & 0x80) | ((src >> 17) & 0x7F);
}

static void dma_copy(void) {
    uint32_t len = dma_length();
    uint16_t src = (uint16_t)(vdp.reg[21] | vdp.reg[22] << 8);
    for (uint32_t i = 0; i < len; i++) {
        vdp.vram[vdp.addr] = vdp.vram[src];
        src++;
        vdp.addr += vdp.reg[15];
    }
    vdp.reg[19] = vdp.reg[20] = 0;
    vdp.reg[21] = (uint8_t)src;
    vdp.reg[22] = (uint8_t)(src >> 8);
}

static void dma_fill(uint16_t v) {
    uint32_t len = dma_length();
    write_target(v);
    if ((vdp.code & 0x0F) == 0x1) {
        for (uint32_t i = 0; i < len; i++) {
            vdp.vram[vdp.addr ^ 1] = (uint8_t)(v >> 8);
            vdp.addr += vdp.reg[15];
        }
    } else {
        for (uint32_t i = 0; i < len; i++) write_target(v);
    }
    vdp.reg[19] = vdp.reg[20] = 0;
}

static void write_control(uint16_t v) {
    if (!vdp.pending) {
        if ((v & 0xC000) == 0x8000) {
            int r = (v >> 8) & 0x1F;
            if (r < 24) vdp.reg[r] = (uint8_t)v;
            vdp.code = 0; /* a register write also resets the access code */
            return;
        }
        vdp.code = (vdp.code & 0x3C) | (v >> 14);
        vdp.addr = (vdp.addr & 0xC000) | (v & 0x3FFF);
        vdp.pending = true;
        return;
    }
    vdp.pending = false;
    vdp.code = (vdp.code & 0x03) | ((v >> 2) & 0x3C);
    vdp.addr = (vdp.addr & 0x3FFF) | (uint16_t)((v & 3) << 14);
    if ((vdp.code & 0x20) && (vdp.reg[1] & 0x10)) {
        if (!(vdp.reg[23] & 0x80)) dma_68k();
        else if (vdp.reg[23] & 0x40) dma_copy();
        else vdp.fill_pending = true;
    }
}

static uint16_t read_data(void) {
    uint16_t v = 0;
    switch (vdp.code & 0x0F) {
    case 0x0: v = vram16(vdp.addr); break;
    case 0x4: { int i = (vdp.addr >> 1) & 0x3F; v = i < 40 ? vdp.vsram[i] : 0; break; }
    case 0x8: v = vdp.cram[(vdp.addr >> 1) & 0x3F]; break;
    }
    vdp.addr += vdp.reg[15];
    return v;
}

static uint16_t read_status(void) {
    int cyc = machine_cycles_in_line();
    uint16_t s = 0x3400 | 0x0200; /* FIFO empty */
    if (vdp.vint_pending) s |= 0x0080;
    if (vdp.line >= vdp_screen_height() || !(vdp.reg[1] & 0x40)) s |= 0x0008;
    if (cyc >= CYCLES_PER_LINE - 84) s |= 0x0004; /* inside horizontal blanking */
    s |= vdp.status & 0x0060;
    vdp.status &= ~0x0060; /* sprite overflow / collision clear on read */
    vdp.pending = false;
    return s;
}

static uint16_t read_hv(void) {
    int v = vdp.line;
    if (v > 0xEA) v -= 6; /* NTSC V counter jumps from $EA to $E5 */
    int h = machine_cycles_in_line() * 210 / CYCLES_PER_LINE; /* H40: ~210 visible counter steps */
    return (uint16_t)((v & 0xFF) << 8 | (h & 0xFF));
}

uint16_t vdp_read16(uint32_t a) {
    switch (a & 0x1C) {
    case 0x00: vdp.pending = false; return read_data();
    case 0x04: return read_status();
    case 0x08: case 0x0C: return read_hv();
    }
    return 0xFFFF;
}

void vdp_write16(uint32_t a, uint16_t v) {
    switch (a & 0x1C) {
    case 0x00:
        vdp.pending = false;
        if (vdp.fill_pending) { vdp.fill_pending = false; dma_fill(v); }
        else { trace_vram("cpu", vdp.addr, 1, 0); write_target(v); }
        break;
    case 0x04: write_control(v); break;
    case 0x10: case 0x14: psg_write((uint8_t)v); break;
    }
}

uint8_t vdp_read8(uint32_t a) {
    uint16_t v = vdp_read16(a & ~1u);
    return (a & 1) ? (uint8_t)v : (uint8_t)(v >> 8);
}

void vdp_write8(uint32_t a, uint8_t v) {
    if ((a & 0x1C) == 0x10) { if (a & 1) psg_write(v); return; }
    vdp_write16(a & ~1u, (uint16_t)(v << 8 | v));
}

/* ---- Renderer ----------------------------------------------------------------
 * Each pixel is built as: bits 0-5 colour index (palette*16+colour),
 * bit 6 = high priority. 0 in the low 4 bits means transparent. */
#define PRI 0x40

static int plane_size(int bits) { return bits == 0 ? 32 : bits == 1 ? 64 : bits == 3 ? 128 : 32; }

/* Draws a scrolled plane (A or B) for line `line` into out[0..w) where out[0] is screen x = -extra. */
/* Each pixel's tile reference on this line, for texture packs (vdp_opts.tile_refs). */
static uint32_t ref_a[320 + MAX_EXTRA_WIDTH * 2], ref_b[320 + MAX_EXTRA_WIDTH * 2], ref_s[320 + MAX_EXTRA_WIDTH * 2];

static void draw_plane(uint8_t *out, int w, int extra, int line, uint32_t nt, int which) {
    uint32_t *refs = which ? ref_b : ref_a;
    bool keep_refs = vdp_opts.tile_refs;
    uint16_t last_e = 0, last_id = 0;
    bool have_e = false;
    int pw = plane_size(vdp.reg[16] & 3), ph = plane_size((vdp.reg[16] >> 4) & 3);
    if (pw == 128 && ph > 32) ph = 32; /* 128x64 / 128x128 are not valid: hardware limits to 4096 cells */
    uint32_t hs_base = (vdp.reg[13] & 0x3F) << 10;
    int hs_line;
    switch (vdp.reg[11] & 3) {
    case 2: hs_line = line & ~7; break;
    case 3: hs_line = line; break;
    default: hs_line = 0; break;
    }
    int hscroll = vram16(hs_base + hs_line * 4 + which * 2) & 0x3FF;
    bool col_vscroll = vdp.reg[11] & 0x04;
    int screen_w = vdp_screen_width();

    for (int i = 0; i < w; i++) {
        int x = i - extra;
        int vs;
        if (col_vscroll) {
            int col = x < 0 ? 0 : x >= screen_w ? screen_w / 16 - 1 : x >> 4;
            vs = vdp.vsram[(col * 2 + which) % 40];
        } else {
            vs = vdp.vsram[which];
        }
        int px = (x - hscroll) & (pw * 8 - 1);
        int py = (line + vs) & (ph * 8 - 1);
        uint16_t e = vram16(nt + (((py >> 3) * pw + (px >> 3)) * 2));
        int row = (e & 0x1000) ? 7 - (py & 7) : (py & 7);
        int col = (e & 0x0800) ? 7 - (px & 7) : (px & 7);
        uint8_t b = vdp.vram[((e & 0x7FF) * 32 + row * 4 + (col >> 1)) & 0xFFFF];
        int c = (col & 1) ? (b & 0xF) : (b >> 4);
        out[i] = (uint8_t)((c ? ((e >> 9) & 0x30) | c : 0) | ((e & 0x8000) ? PRI : 0));
        if (keep_refs) {
            if (!have_e || e != last_e) { last_e = e; have_e = true; last_id = texpack_tile(e & 0x7FF, (e >> 13) & 3); }
            refs[i] = texpack_ref(last_id, e & 0x0800, e & 0x1000, col, row);   /* also where transparent: a replacement may fill it */
        }
    }
}

/* Window plane: unscrolled, replaces plane A where the window is active. */
static bool window_covers(int x, int line) {
    int wv = (vdp.reg[18] & 0x1F) * 8;
    bool down = vdp.reg[18] & 0x80;
    if (down ? line >= wv : line < wv) return true;
    int wh = (vdp.reg[17] & 0x1F) * 16;
    bool right = vdp.reg[17] & 0x80;
    /* A zero-width left window is no window: without the wh > 0 check, the widescreen
     * columns left of x = 0 would count as inside it. */
    return right ? x >= wh : (wh > 0 && x < wh);
}

static void draw_window(uint8_t *out, int w, int extra, int line) {
    bool h40 = vdp_screen_width() == 320;
    uint32_t nt = (vdp.reg[3] & (h40 ? 0x3C : 0x3E)) << 10;
    int cells = h40 ? 64 : 32;
    for (int i = 0; i < w; i++) {
        int x = i - extra;
        if (!window_covers(x, line)) continue;
        int px = x & (cells * 8 - 1);
        uint16_t e = vram16(nt + ((line >> 3) * cells + (px >> 3)) * 2);
        int row = (e & 0x1000) ? 7 - (line & 7) : (line & 7);
        int col = (e & 0x0800) ? 7 - (px & 7) : (px & 7);
        uint8_t b = vdp.vram[((e & 0x7FF) * 32 + row * 4 + (col >> 1)) & 0xFFFF];
        int c = (col & 1) ? (b & 0xF) : (b >> 4);
        if (vdp_opts.tile_refs) ref_a[i] = texpack_ref(texpack_tile(e & 0x7FF, (e >> 13) & 3), e & 0x0800, e & 0x1000, col, row);
        out[i] = (uint8_t)((c ? ((e >> 9) & 0x30) | c : 0) | ((e & 0x8000) ? PRI : 0) |
                           (vdp_opts.hd_capture ? HD_WINDOW : 0));
    }
}

static inline uint32_t layer_px(uint8_t v);
static uint32_t rgb(uint16_t c, int mode);
static uint32_t rgb_raw(uint16_t c, int mode);

/* ---- The player's outfit ----------------------------------------------------- *
 * Sprite pixels set bit 7 of their byte when they are the player's and their colour
 * index has an outfit colour; sprite_px() then takes vdp_outfit's colour instead of CRAM's. */
#define OUTFIT 0x80
uint16_t vdp_outfit_pick[OUTFIT_BANDS][64];
uint8_t  vdp_outfit_anim[OUTFIT_BANDS][64];
uint8_t  vdp_outfit_ref[64];
static uint16_t vdp_outfit[64];         /* this line's colour for each index */
static uint8_t outfit_used[64];         /* the indices that have a pick in some band */
static int outfit_nused, player_bottom, outfit_line = -1;
static unsigned outfit_frame;
uint8_t  vdp_player[PLAYER_IMG * PLAYER_IMG];
uint16_t vdp_player_cram[64];
int      vdp_player_w, vdp_player_h, vdp_player_x, vdp_player_y;
static uint8_t player_now[PLAYER_IMG * PLAYER_IMG];     /* the frame being drawn */
static uint16_t player_cram[64];
static bool player_entry[80];
static int player_x0, player_y0;

uint16_t vdp_outfit_shade(uint16_t pick, uint16_t now, int ref) {
    int lum = ((now >> 1) & 7) + ((now >> 5) & 7) + ((now >> 9) & 7);
    int ch[3] = { (pick >> 1) & 7, (pick >> 5) & 7, (pick >> 9) & 7 };
    for (int k = 0; k < 3; k++) {
        int v = ref > 0 ? (ch[k] * lum * 2 + ref) / (ref * 2) : ch[k];
        ch[k] = v > 7 ? 7 : v;
    }
    return (uint16_t)(ch[2] << 9 | ch[1] << 5 | ch[0] << 1);
}

/* The animations change the pick, in the Genesis' 3-bit channels, before it is shaded. */
uint16_t vdp_outfit_color(uint16_t pick, int anim, uint16_t now, int ref, int above_feet, unsigned frame) {
    int ch[3] = { (pick >> 1) & 7, (pick >> 5) & 7, (pick >> 9) & 7 };
    switch (anim) {
    case OUTFIT_ANIM_SHIMMER: {         /* a bright band sweeping up him every two seconds */
        int d = (int)(frame % 120) - 20 - above_feet;
        d = d < 0 ? -d : d;
        if (d < 6) for (int k = 0; k < 3; k++) ch[k] += (6 - d + 1) / 2;
        break;
    }
    case OUTFIT_ANIM_PULSE: {           /* breathes between full and about half, every 1.6 s */
        static const uint8_t wave[16] = { 8, 8, 7, 7, 6, 5, 5, 4, 4, 4, 5, 5, 6, 7, 7, 8 };
        int f = wave[(frame / 6) % 16];
        for (int k = 0; k < 3; k++) ch[k] = (ch[k] * f + 4) / 8;
        break;
    }
    case OUTFIT_ANIM_RAINBOW: {         /* the hues run up him, at the pick's brightness */
        static const uint8_t hues[12][3] = { { 7, 0, 0 }, { 7, 3, 0 }, { 7, 6, 0 }, { 5, 7, 0 }, { 1, 7, 0 }, { 0, 7, 3 },
                                             { 0, 7, 7 }, { 0, 3, 7 }, { 1, 0, 7 }, { 4, 0, 7 }, { 7, 0, 6 }, { 7, 0, 3 } };
        int m = ch[0] > ch[1] ? ch[0] : ch[1];
        m = m > ch[2] ? m : ch[2];
        if (m < 3) m = 3;
        const uint8_t *h = hues[(frame / 5 + (unsigned)above_feet / 3) % 12];
        for (int k = 0; k < 3; k++) ch[k] = (h[k] * m + 3) / 7;
        break;
    }
    case OUTFIT_ANIM_FLAME: {           /* flickering fire: reds at his feet, yellows higher up, licking upwards */
        static const uint8_t fire[5][3] = { { 5, 0, 0 }, { 7, 1, 0 }, { 7, 3, 0 }, { 7, 5, 1 }, { 7, 7, 3 } };
        unsigned n = (unsigned)above_feet * 2654435761u ^ (frame / 4) * 40503u;
        n ^= n >> 13;
        int at = (int)((unsigned)(above_feet + frame / 2) % 7 + (n & 3)) / 2;
        const uint8_t *h = fire[at > 4 ? 4 : at];
        for (int k = 0; k < 3; k++) ch[k] = (ch[k] + h[k] * 3 + 2) / 4;
        break;
    }
    }
    for (int k = 0; k < 3; k++) ch[k] = ch[k] > 7 ? 7 : ch[k];
    return vdp_outfit_shade((uint16_t)(ch[2] << 9 | ch[1] << 5 | ch[0] << 1), now, ref);
}

uint32_t vdp_color_rgb(uint16_t cram) { return rgb(cram, 1); }
uint32_t vdp_color_raw(uint16_t cram) { return rgb_raw(cram, 1); }

/* This line's outfit colours, from the band it is in on him. */
static void outfit_for_line(int line) {
    if (line == outfit_line) return;
    outfit_line = line;
    int above = player_bottom - 1 - line, band = outfit_band(above);
    for (int u = 0; u < outfit_nused; u++) {
        int k = outfit_used[u];
        uint16_t pick = vdp_outfit_pick[band][k];
        vdp_outfit[k] = pick == OUTFIT_KEEP ? OUTFIT_KEEP
                        : vdp_outfit_color(pick, vdp_outfit_anim[band][k], vdp.cram[k], vdp_outfit_ref[k], above, outfit_frame);
    }
}

/* At the first line: the last frame's picture of the player if he was on screen, his
 * entries and the box around them in this one, and this frame's outfit colours. */
static void player_frame_start(void) {
    if (player_w && player_h) {
        memcpy(vdp_player, player_now, sizeof vdp_player);
        memcpy(vdp_player_cram, player_cram, sizeof vdp_player_cram);
        vdp_player_w = player_w; vdp_player_h = player_h;
        vdp_player_x = player_x0; vdp_player_y = player_y0;
    }
    memset(player_now, 0, sizeof player_now);
    memcpy(player_cram, vdp.cram, sizeof player_cram);
    outfit_frame++;
    outfit_line = -1;
    outfit_nused = 0;
    for (int k = 0; k < 64; k++) {
        vdp_outfit[k] = OUTFIT_KEEP;
        if (vdp_opts.outfit && (vdp_outfit_pick[0][k] != OUTFIT_KEEP || vdp_outfit_pick[1][k] != OUTFIT_KEEP || vdp_outfit_pick[2][k] != OUTFIT_KEEP))
            outfit_used[outfit_nused++] = (uint8_t)k;
    }
    bool h40 = vdp_screen_width() == 320;
    uint32_t sat = (vdp.reg[5] & (h40 ? 0x7E : 0x7F)) << 9;
    motion_player_entries(vdp.vram + sat, h40 ? 80 : 64, player_entry);
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    for (int k = 0; k < 80; k++) {
        if (!player_entry[k]) continue;
        uint32_t s = sat + k * 8;
        int y = (vram16(s) & 0x3FF) - 128, size = vram16(s + 2), x = (vram16(s + 6) & 0x1FF) - 128;
        int w = (((size >> 10) & 3) + 1) * 8, h = (((size >> 8) & 3) + 1) * 8;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + w > x1) x1 = x + w;
        if (y + h > y1) y1 = y + h;
    }
    player_x0 = x0; player_y0 = y0; player_bottom = y1;
    player_w = x1 > x0 ? (x1 - x0 > PLAYER_IMG ? PLAYER_IMG : x1 - x0) : 0;
    player_h = y1 > y0 ? (y1 - y0 > PLAYER_IMG ? PLAYER_IMG : y1 - y0) : 0;
}

static inline uint32_t sprite_px(uint8_t v) {
    if ((v & OUTFIT) && (v & 0x0F)) return (uint32_t)vdp_outfit[v & 0x3F] | (uint32_t)(v & 0x7F) << 16;
    return layer_px((uint8_t)(v & 0x7F));
}

static void draw_sprites(uint8_t *out, int w, int extra, int line) {
    bool h40 = vdp_screen_width() == 320;
    uint32_t sat = (vdp.reg[5] & (h40 ? 0x7E : 0x7F)) << 9;
    int max_total = h40 ? 80 : 64;
    int max_line = vdp_opts.no_sprite_limit ? max_total : (h40 ? 20 : 16);
    int max_px = vdp_opts.no_sprite_limit ? 0x7FFFFFFF : (h40 ? 320 : 256);
    int on_line = 0, px_used = 0, link = 0;
    bool any_nonzero_x = false;

    for (int n = 0; n < max_total; n++) {
        int link_now = link;
        uint32_t s = sat + link * 8;
        int y = (vram16(s) & 0x3FF) - 128;
        int size = vram16(s + 2);
        int hcells = ((size >> 10) & 3) + 1, vcells = ((size >> 8) & 3) + 1;
        link = size & 0x7F;
        if (line >= y && line < y + vcells * 8) {
            uint16_t attr = vram16(s + 4);
            int x = (vram16(s + 6) & 0x1FF) - 128;
            if (++on_line > max_line) { vdp.status |= 0x40; break; }
            /* A sprite at X=0 hides every lower-priority sprite on this line. */
            if (x == -128 && any_nonzero_x) { if (vdp_opts.hd_capture) hd_frame.sprite_mask = true; break; }
            if (x != -128) any_nonzero_x = true;
            int row = line - y;
            int sy = row;
            if (attr & 0x1000) sy = vcells * 8 - 1 - sy;
            HdSprite *cap = NULL;
            if (vdp_opts.hd_capture && line < HD_MAX_H) {
                cap = &hd_frame.sprites[link_now];
                if (!cap->drawn) {
                    cap->drawn = true;
                    /* Front-to-back order is the position in the sprite list walk (the
                     * same on every line), not the line where the sprite first shows. */
                    cap->rank = n;
                    hd_frame.sprite_count++;
                    cap->x = x; cap->y = y; cap->w = hcells * 8; cap->h = vcells * 8; cap->attr = attr;
                    cap->player = player_entry[link_now];
                    cap->item = false;
                    memset(cap->px, 0, sizeof cap->px);
                }
            }
            for (int sx = 0; sx < hcells * 8; sx++) {
                if (px_used++ >= max_px) break;
                int i = x + sx + extra;
                int cx = (attr & 0x0800) ? hcells * 8 - 1 - sx : sx;
                int tile = ((attr & 0x7FF) + (cx >> 3) * vcells + (sy >> 3)) & 0x7FF;
                uint8_t b = vdp.vram[(tile * 32 + (sy & 7) * 4 + ((cx & 7) >> 1)) & 0xFFFF];
                int c = (cx & 1) ? (b & 0xF) : (b >> 4);
                if (!c) continue;
                uint8_t v = (uint8_t)(((attr >> 9) & 0x30) | c | ((attr & 0x8000) ? PRI : 0));
                if (player_entry[link_now]) {
                    int px = x + sx - player_x0, py = line - player_y0;
                    if (px >= 0 && py >= 0 && px < player_w && py < player_h && !player_now[py * PLAYER_IMG + px])
                        player_now[py * PLAYER_IMG + px] = v & 0x3F;
                    if (vdp_opts.outfit) {
                        outfit_for_line(line);
                        if (vdp_outfit[v & 0x3F] != OUTFIT_KEEP) v |= OUTFIT;
                    }
                }
                uint32_t ref = vdp_opts.tile_refs ? texpack_ref(texpack_tile(tile, (attr >> 13) & 3), attr & 0x0800, attr & 0x1000, cx & 7, sy & 7) : 0;
                if (cap) { cap->px[row * 32 + sx] = sprite_px(v); cap->ref[row * 32 + sx] = ref; }
                if (i < 0 || i >= w) continue;
                if (out[i] & 0x0F) { vdp.status |= 0x20; continue; } /* earlier sprite wins */
                out[i] = v;
                ref_s[i] = ref;
            }
        }
        if (link == 0 || link >= max_total) break;
    }
}

static uint32_t rgb_raw(uint16_t c, int mode) {
    const uint8_t *t = mode == 0 ? LEVEL_SHADOW : mode == 2 ? LEVEL_HIGHLIGHT : LEVEL_NORMAL;
    return (uint32_t)t[(c >> 1) & 7] << 16 | (uint32_t)t[(c >> 5) & 7] << 8 | t[(c >> 9) & 7];
}

/* ---- Colour tone and colour-blind help ------------------------------------------------ *
 * The VDP has 512 colours in each of its three modes, so the tone and the colour-blind
 * correction are worked out once for each (in linear light) into a table that rgb() reads:
 * every picture, scaled or not, and every effect made from it gets them for nothing. The
 * correction is daltonization: the colour as that eye sees it (Machado, Oliveira and
 * Fernandes' matrices, full severity) is taken from the colour, and what that eye misses is
 * moved into channels it can tell apart, at two thirds of the full shift so the scene keeps its
 * colours. */
static uint32_t rgb_lut[3][512];
static int lut_tone = -1, lut_blind = -1;

static float to_linear(float v) { return v <= .04045f ? v / 12.92f : powf((v + .055f) / 1.055f, 2.4f); }
static uint8_t to_srgb(float v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    v = v <= .0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - .055f;
    return (uint8_t)(v * 255 + .5f);
}

static void grade(float c[3], int tone, int blind) {
    static const float gains[VDP_TONES][3] = { { 1, 1, 1 }, { 1, .95f, .82f }, { .86f, .95f, 1 }, { 1, 1, 1 } };
    for (int k = 0; k < 3; k++) c[k] *= gains[tone][k];
    if (tone == VDP_TONE_VIVID) {
        float y = .2126f * c[0] + .7152f * c[1] + .0722f * c[2];
        for (int k = 0; k < 3; k++) c[k] = y + (c[k] - y) * 1.15f;
    }
    if (blind) {
        static const float sim[4][3][3] = {
            { { 0 } },
            { { .152286f, 1.052583f, -.204868f }, { .114503f, .786281f, .099216f }, { -.003882f, -.048116f, 1.051998f } },
            { { .367322f, .860646f, -.227968f }, { .280085f, .672501f, .047413f }, { -.011820f, .042940f, .968881f } },
            { { 1.255528f, -.076749f, -.178779f }, { -.078411f, .930809f, .147602f }, { .004733f, .691367f, .303900f } },
        };
        float e[3];
        for (int k = 0; k < 3; k++) e[k] = .65f * (c[k] - (sim[blind][k][0] * c[0] + sim[blind][k][1] * c[1] + sim[blind][k][2] * c[2]));
        if (blind == VDP_BLIND_TRITAN) { c[0] += e[0] + .7f * e[2]; c[1] += e[1] + .7f * e[2]; }
        else { c[1] += .7f * e[0] + e[1]; c[2] += .7f * e[0] + e[2]; }
    }
}

static void make_lut(void) {
    int tone = vdp_opts.tone, blind = vdp_opts.color_blind;
    for (int mode = 0; mode < 3; mode++)
        for (int i = 0; i < 512; i++) {
            uint16_t cram = (uint16_t)((i & 7) << 1 | (i >> 3 & 7) << 5 | (i >> 6 & 7) << 9);
            uint32_t v = rgb_raw(cram, mode);
            if (tone || blind) {
                float c[3] = { to_linear((v >> 16 & 255) / 255.f), to_linear((v >> 8 & 255) / 255.f), to_linear((v & 255) / 255.f) };
                grade(c, tone, blind);
                v = (uint32_t)to_srgb(c[0]) << 16 | (uint32_t)to_srgb(c[1]) << 8 | to_srgb(c[2]);
            }
            rgb_lut[mode][i] = v;
        }
    lut_tone = tone;
    lut_blind = blind;
}

static uint32_t rgb(uint16_t c, int mode) {
    if (vdp_opts.tone != lut_tone || vdp_opts.color_blind != lut_blind) make_lut();
    return rgb_lut[mode][(c >> 1 & 7) | (c >> 5 & 7) << 3 | (c >> 9 & 7) << 6];
}

/* A layer pixel: the CRAM colour in bits 0-15 and the layer byte in bits 16-23 (the
 * priority bit is kept on transparent pixels too: shadow/highlight looks at it). */
static inline uint32_t layer_px(uint8_t v) {
    return (v & 0x0F) ? (uint32_t)vdp.cram[v & 0x3F] | (uint32_t)v << 16 : (uint32_t)(v & (PRI | HD_WINDOW)) << 16;
}

uint32_t vdp_compose(uint32_t a, uint32_t b, uint32_t s, uint16_t backdrop, bool sh) {
    uint8_t ab = (uint8_t)(a >> 16), bb = (uint8_t)(b >> 16), sb = (uint8_t)(s >> 16);
    /* Top-most opaque pixel: S-high, A-high, B-high, S-low, A-low, B-low, backdrop. */
    uint32_t layers[6] = {
        (sb & PRI) ? s : 0, (ab & PRI) ? a : 0, (bb & PRI) ? b : 0,
        (sb & PRI) ? 0 : s, (ab & PRI) ? 0 : a, (bb & PRI) ? 0 : b,
    };
    int which = -1;
    for (int k = 0; k < 6; k++) {
        if ((layers[k] >> 16) & 0x0F) { which = k; break; }
    }
    uint16_t color = which < 0 ? backdrop : (uint16_t)layers[which];
    int mode = 1;
    if (sh) {
        /* Planes are shadowed unless one of them is high priority. */
        mode = ((ab | bb) & PRI) ? 1 : 0;
        if (which == 0 || which == 3) {
            int sc = sb & 0x3F;
            if (sc == 0x3E || sc == 0x3F) {
                /* Palette 3 colours 14/15 are operators: tint whatever is below. */
                int below = -1;
                for (int k = 0; k < 6; k++) {
                    if (k == 0 || k == 3) continue;
                    if ((layers[k] >> 16) & 0x0F) { below = k; break; }
                }
                color = below < 0 ? backdrop : (uint16_t)layers[below];
                mode = sc == 0x3E ? (mode == 0 ? 1 : 2) : 0;
            } else if (sb & PRI) {
                mode = 1;
            }
        }
    }
    return rgb(color, mode);
}

void vdp_render_line(int line, uint32_t *dst) {
    int extra = vdp_opts.extra_width;
    int w = vdp_screen_width() + extra * 2;
    uint16_t backdrop = vdp.cram[vdp.reg[7] & 0x3F];
    bool capture = vdp_opts.hd_capture && line < HD_MAX_H;
    if (line == 0) {
        player_frame_start();
        if (vdp_opts.tile_refs) texpack_frame();
    }
    if (capture) {
        if (line == 0) {
            hd_frame.w = w; hd_frame.extra = extra; hd_frame.mask_sides = vdp_opts.mask_sides;
            hd_serial++;
            for (int k = 0; k < 80; k++) hd_frame.sprites[k].drawn = false;
            hd_frame.sprite_count = 0;
            hd_frame.sprite_mask = false;
        }
        hd_frame.h = line + 1;
        hd_frame.backdrop[line] = backdrop;
        hd_frame.flags[line] = 0;
        uint32_t hs_base = (vdp.reg[13] & 0x3F) << 10;
        int hs_line = (vdp.reg[11] & 3) == 2 ? line & ~7 : (vdp.reg[11] & 3) == 3 ? line : 0;
        for (int p = 0; p < 2; p++) {
            hd_frame.hscroll[p][line] = (int16_t)(vram16(hs_base + hs_line * 4 + p * 2) & 0x3FF);
            hd_frame.vscroll[p][line] = (int16_t)(vdp.vsram[p] & 0x3FF);
        }
    }
    if (!(vdp.reg[1] & 0x40)) {
        uint32_t c = rgb(backdrop, 1);
        for (int i = 0; i < w; i++) dst[i] = c;
        return;
    }

    static uint8_t a[320 + MAX_EXTRA_WIDTH * 2], b[320 + MAX_EXTRA_WIDTH * 2], s[320 + MAX_EXTRA_WIDTH * 2];
    memset(s, 0, (size_t)w);
    if (vdp_opts.tile_refs) memset(ref_s, 0, (size_t)w * sizeof *ref_s);
    draw_plane(b, w, extra, line, (vdp.reg[4] & 7) << 13, 1);
    draw_plane(a, w, extra, line, (vdp.reg[2] & 0x38) << 10, 0);
    draw_window(a, w, extra, line);
    draw_sprites(s, w, extra, line);
    if (vdp_opts.hide_layers) {
        if (vdp_opts.hide_layers & 1) memset(a, 0, (size_t)w);
        if (vdp_opts.hide_layers & 2) memset(b, 0, (size_t)w);
        if (vdp_opts.hide_layers & 4) memset(s, 0, (size_t)w);
    }

    bool sh = vdp.reg[12] & 0x08;
    if (capture) {
        hd_frame.flags[line] = HD_LINE_DISPLAY | (sh ? HD_LINE_SH : 0) | ((vdp.reg[11] & 4) ? HD_LINE_COLUMN_VSCROLL : 0);
        for (int i = 0; i < w; i++) {
            uint32_t pa = layer_px(a[i]), pb = layer_px(b[i]), ps = sprite_px(s[i]);
            hd_frame.a[line][i] = pa; hd_frame.b[line][i] = pb; hd_frame.s[line][i] = ps;
            if (vdp_opts.tile_refs) { hd_frame.ta[line][i] = ref_a[i]; hd_frame.tb[line][i] = ref_b[i]; hd_frame.ts[line][i] = ref_s[i]; }
            dst[i] = vdp_compose(pa, pb, ps, backdrop, sh);
        }
    } else {
        for (int i = 0; i < w; i++) dst[i] = vdp_compose(layer_px(a[i]), layer_px(b[i]), sprite_px(s[i]), backdrop, sh);
    }
    if (vdp_opts.mask_sides && extra) {
        memset(dst, 0, (size_t)extra * sizeof *dst);
        memset(dst + w - extra, 0, (size_t)extra * sizeof *dst);
    }
}
