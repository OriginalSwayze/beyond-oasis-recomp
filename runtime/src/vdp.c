/* Sega 315-5313 VDP: ports, DMA, interrupts and a scanline renderer.
 * The renderer can draw extra columns on both sides of the 320-pixel screen
 * (vdp_opts.extra_width) for widescreen; the game still thinks it is 320 wide. */
#include "genesis.h"
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

void vdp_reset(void) {
    memset(&vdp, 0, sizeof vdp);
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
static void draw_plane(uint8_t *out, int w, int extra, int line, uint32_t nt, int which) {
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
        out[i] = (uint8_t)((c ? ((e >> 9) & 0x30) | c : 0) | ((e & 0x8000) ? PRI : 0));
    }
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
            if (x == -128 && any_nonzero_x) break;
            if (x != -128) any_nonzero_x = true;
            int sy = line - y;
            if (attr & 0x1000) sy = vcells * 8 - 1 - sy;
            for (int sx = 0; sx < hcells * 8; sx++) {
                if (px_used++ >= max_px) break;
                int i = x + sx + extra;
                if (i < 0 || i >= w) continue;
                int cx = (attr & 0x0800) ? hcells * 8 - 1 - sx : sx;
                int tile = ((attr & 0x7FF) + (cx >> 3) * vcells + (sy >> 3)) & 0x7FF;
                uint8_t b = vdp.vram[(tile * 32 + (sy & 7) * 4 + ((cx & 7) >> 1)) & 0xFFFF];
                int c = (cx & 1) ? (b & 0xF) : (b >> 4);
                if (!c) continue;
                if (out[i] & 0x0F) { vdp.status |= 0x20; continue; } /* earlier sprite wins */
                out[i] = (uint8_t)(((attr >> 9) & 0x30) | c | ((attr & 0x8000) ? PRI : 0));
            }
        }
        if (link == 0 || link >= max_total) break;
    }
}

static uint32_t rgb(uint16_t c, int mode) {
    const uint8_t *t = mode == 0 ? LEVEL_SHADOW : mode == 2 ? LEVEL_HIGHLIGHT : LEVEL_NORMAL;
    return (uint32_t)t[(c >> 1) & 7] << 16 | (uint32_t)t[(c >> 5) & 7] << 8 | t[(c >> 9) & 7];
}

void vdp_render_line(int line, uint32_t *dst) {
    int extra = vdp_opts.extra_width;
    int w = vdp_screen_width() + extra * 2;
    uint16_t backdrop = vdp.cram[vdp.reg[7] & 0x3F];
    if (!(vdp.reg[1] & 0x40)) {
        uint32_t c = rgb(backdrop, 1);
        for (int i = 0; i < w; i++) dst[i] = c;
        return;
    }

    static uint8_t a[320 + MAX_EXTRA_WIDTH * 2], b[320 + MAX_EXTRA_WIDTH * 2], s[320 + MAX_EXTRA_WIDTH * 2];
    memset(s, 0, (size_t)w);
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
    for (int i = 0; i < w; i++) {
        /* Top-most opaque pixel: S-high, A-high, B-high, S-low, A-low, B-low, backdrop. */
        uint8_t layers[6] = {
            (s[i] & PRI) ? s[i] : 0, (a[i] & PRI) ? a[i] : 0, (b[i] & PRI) ? b[i] : 0,
            (s[i] & PRI) ? 0 : s[i], (a[i] & PRI) ? 0 : a[i], (b[i] & PRI) ? 0 : b[i],
        };
        int idx = -1, which = -1;
        for (int k = 0; k < 6; k++) {
            if (layers[k] & 0x0F) { idx = layers[k] & 0x3F; which = k; break; }
        }
        uint16_t color = idx < 0 ? backdrop : vdp.cram[idx];
        int mode = 1;
        if (sh) {
            /* Planes are shadowed unless one of them is high priority. */
            mode = ((a[i] | b[i]) & PRI) ? 1 : 0;
            if (which == 0 || which == 3) {
                int sc = s[i] & 0x3F;
                if (sc == 0x3E || sc == 0x3F) {
                    /* Palette 3 colours 14/15 are operators: tint whatever is below. */
                    int below = -1;
                    for (int k = 0; k < 6; k++) {
                        if (k == 0 || k == 3) continue;
                        if (layers[k] & 0x0F) { below = layers[k] & 0x3F; break; }
                    }
                    color = below < 0 ? backdrop : vdp.cram[below];
                    mode = sc == 0x3E ? (mode == 0 ? 1 : 2) : 0;
                } else if (s[i] & PRI) {
                    mode = 1;
                }
            }
        }
        dst[i] = rgb(color, mode);
    }
    if (vdp_opts.mask_sides && extra) {
        memset(dst, 0, (size_t)extra * sizeof *dst);
        memset(dst + w - extra, 0, (size_t)extra * sizeof *dst);
    }
}
