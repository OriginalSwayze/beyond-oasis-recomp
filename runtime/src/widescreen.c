/* Widescreen: the game's tile-map streaming, rewritten in C to keep a wider band of
 * columns valid in VRAM.
 *
 * Each scrolling layer has a 0x28-byte struct ($FF1716 for plane A, $FF173E for plane B):
 *   +00 camera x (pixels)        +04 camera y (pixels)
 *   +08 left map column          +0A top map row
 *   +0C VRAM offset of the top-left cell (column * 2 + row * $80, 64x32 plane)
 *   +16 map pointer (long)       +1A VRAM nametable base
 *   +1C pending column steps     +1E pending row steps (sign = direction)
 *   +20 map width (cells)        +22 map height (cells)    +24 log2 of the map row stride
 *   +26 dirty flags (VBlank)     +27 bit 0 = layer has a map, bit 1 = map fits the whole plane
 *
 * The original routines keep 41 columns valid: the left map column and the 40 to its
 * right. These versions keep `pad` more columns on each side (up to 11, so the band
 * still fits the 64-column plane), and draw cells outside the map as the blank tile the
 * game itself uses for empty planes. Each one replaces a whole subroutine: it does the
 * same work as the original, then returns to the caller. Registers the original leaves
 * in a known state (the VDP port pointers) are set the same way; the others were
 * scratch for the original too. */
#include "genesis.h"
#include "recomp_rt.h"
#include "game.h"

#define VDP_DATA  0xC00000
#define VDP_CTRL  0xC00004
#define BLANK_CELL 0x0780               /* tile entry sub_00D962 fills empty planes with */

static int pad;                         /* extra columns kept valid on each side */
/* The last of the rows the game keeps valid, from the top one: $1C (29 rows) for the 224-line
 * picture, $1E (31 rows) in the PAL versions' 240-line one; it is the only change those
 * versions made to these routines. */
static int last_row(void) { return game_is_pal() ? 0x1E : 0x1C; }
uint32_t patch_cull_left, patch_cull_widen;
int patch_cam_margin;

static int16_t L16(uint32_t layer, int off) { return (int16_t)rd16(layer + off); }

static void vram_cell(uint32_t base, int vrow, int vcol, uint32_t v) {
    uint32_t a = (base + (uint32_t)(((vrow & 31) * 64 + (vcol & 63)) * 2)) & 0xFFFF;
    wr16(VDP_CTRL, 0x4000 | (a & 0x3FFF));
    wr16(VDP_CTRL, a >> 14);
    wr16(VDP_DATA, v);
}

/* Map cell (col, row): rows wrap like the original. The original wraps columns too, which
 * only shows on a map narrower than its 41 columns (the rushing wave in the water dungeon,
 * 32 wide, repeats across the corridor): such maps wrap here as well, over those 41
 * columns from the left one (+08), and are blank in the widescreen columns beside them,
 * which the original never draws. On wider maps the columns past an edge would be the far
 * side of the room, so they are blank. */
static uint32_t map_cell(uint32_t layer, int col, int row) {
    int w = L16(layer, 0x20), h = L16(layer, 0x22);
    if (w > 0 && w < 41) {
        int from_left = col - L16(layer, 0x08);
        if (from_left < 0 || from_left > 40) return BLANK_CELL;
        col %= w;
        if (col < 0) col += w;
    } else if (col < 0 || col >= w) return BLANK_CELL;
    row %= h;
    if (row < 0) row += h;
    return rd16(rd32(layer + 0x16) + ((uint32_t)row << L16(layer, 0x24)) + (uint32_t)col * 2);
}

static void draw_column(uint32_t layer, int col, int vcol) {
    int top = L16(layer, 0x0A), vtop = (L16(layer, 0x0C) >> 7) & 31;
    uint32_t base = (uint16_t)L16(layer, 0x1A);
    for (int i = 0; i <= last_row(); i++) vram_cell(base, vtop + i, vcol, map_cell(layer, col, top + i));
}

static void draw_row(uint32_t layer, int row, int vrow) {
    int left = L16(layer, 0x08), vleft = (L16(layer, 0x0C) >> 1) & 63;
    uint32_t base = (uint16_t)L16(layer, 0x1A);
    for (int k = -pad; k <= 40 + pad; k++) vram_cell(base, vrow, vleft + k, map_cell(layer, left + k, row));
}

static uint32_t return_to_caller(int cycles) {
    USE_CYCLES(cycles);
    REG_PC = pop32();
    return REG_PC;
}

static void port_registers(void) { A(2) = VDP_DATA; A(4) = VDP_CTRL; }

/* The original raises the interrupt mask to 7 and then clears two bits of it, so the
 * caller always gets mask 1 back. */
static void interrupt_mask_after_vdp_access(void) { m68ki_set_sr((m68ki_get_sr() | 0x0700) & 0xF9FF); }

/* sub_0023CC: scroll one column (VBlank; called while +1C != 0). */
static uint32_t scroll_column(uint32_t pc) {
    (void)pc;
    uint32_t layer = A(3);
    int w = L16(layer, 0x20), steps = L16(layer, 0x1C);
    int left = L16(layer, 0x08), c = L16(layer, 0x0C);
    int target, vtarget;
    if (steps >= 0) {
        wr16(layer + 0x1C, (uint32_t)(steps - 1));
        if (++left >= w) left -= w;
        c = (((c & 0x7E) + 2) & 0x7E) | (c & 0xF80);
        target = left + 40 + pad;
        vtarget = ((c & 0x7E) >> 1) + 40 + pad;
    } else {
        wr16(layer + 0x1C, (uint32_t)(steps + 1));
        if (--left < 0) left += w;
        c = (((c & 0x7E) - 2) & 0x7E) | (c & 0xF80);
        target = left - pad;
        vtarget = ((c & 0x7E) >> 1) - pad;
    }
    wr16(layer + 0x08, (uint32_t)left);
    wr16(layer + 0x0C, (uint32_t)c);
    draw_column(layer, target, vtarget);
    if (w < 41) {
        /* A narrow map shows only in the original's 41 columns (map_cell): the columns at
         * that band's edges change between the map and blank as it moves. */
        int v0 = (c & 0x7E) >> 1;
        draw_column(layer, left - 1, v0 - 1);
        draw_column(layer, left, v0);
        draw_column(layer, left + 40, v0 + 40);
        draw_column(layer, left + 41, v0 + 41);
    }
    port_registers();
    return return_to_caller(2700);
}

/* sub_002512: scroll one row (VBlank; called while +1E != 0). */
static uint32_t scroll_row(uint32_t pc) {
    (void)pc;
    uint32_t layer = A(3);
    int h = L16(layer, 0x22), steps = L16(layer, 0x1E);
    int top = L16(layer, 0x0A), c = L16(layer, 0x0C);
    int target, vtarget;
    if (steps >= 0) {
        wr16(layer + 0x1E, (uint32_t)(steps - 1));
        if (++top >= h) top -= h;
        c = (((c & 0xF80) + 0x80) & 0xF80) | (c & 0x7E);
        target = top + last_row();
        vtarget = (c >> 7) + last_row();
    } else {
        wr16(layer + 0x1E, (uint32_t)(steps + 1));
        if (--top < 0) top += h;
        c = (((c & 0xF80) - 0x80) & 0xF80) | (c & 0x7E);
        target = top;
        vtarget = c >> 7;
    }
    wr16(layer + 0x0A, (uint32_t)top);
    wr16(layer + 0x0C, (uint32_t)c);
    draw_row(layer, target, vtarget);
    port_registers();
    return return_to_caller(2200 + pad * 100);
}

/* Fills the columns visible with zero scroll (0..40+pad and the last `pad`) with the blank cell. */
static void clear_plane(uint32_t base) {
    for (int row = 0; row <= last_row(); row++)
        for (int k = -pad; k <= 40 + pad; k++) vram_cell(base, row, k, BLANK_CELL);
}

/* sub_00D7C0: set up a layer for a new area and draw it. */
static uint32_t init_layer(uint32_t pc) {
    (void)pc;
    uint32_t layer = A(3);
    static const int zeroed[] = { 0x0E, 0x10, 0x0C, 0x08, 0x0A, 0x1C, 0x1E };
    for (unsigned i = 0; i < sizeof zeroed / sizeof *zeroed; i++) wr16(layer + zeroed[i], 0);
    uint32_t base = (uint16_t)L16(layer, 0x1A);
    port_registers();
    if (!(rd8(layer + 0x27) & 1)) {     /* no map: clear the plane (loc_00D95E) */
        clear_plane(base);
        interrupt_mask_after_vdp_access();
        return return_to_caller(41 * 29 * 12 + 200);
    }
    int w = L16(layer, 0x20), h = L16(layer, 0x22);
    int left = L16(layer, 0x00) >> 3, top = L16(layer, 0x04) >> 3;
    left %= w; if (left < 0) left += w;
    top %= h;  if (top < 0) top += h;
    wr16(layer + 0x08, (uint32_t)left);
    wr16(layer + 0x0A, (uint32_t)top);
    bool whole_plane = rd8(layer + 0x27) & 2;
    if (!whole_plane && h == 32 && (uint16_t)w <= 64) { wr8(layer + 0x27, rd8(layer + 0x27) | 2); whole_plane = true; }
    if (whole_plane) {
        /* The map fits the plane: draw all of it. A 64-wide map wraps around the plane
         * like the original draws it; a narrower one is blank past its right edge. */
        for (int row = 0; row < 32; row++)
            for (int k = 0; k < 64; k++)
                vram_cell(base, row, k, map_cell(layer, w == 64 ? (left + k) % 64 : left + k, top + row));
    } else {
        for (int row = 0; row <= last_row(); row++)
            for (int k = -pad; k <= 40 + pad; k++) vram_cell(base, row, k, map_cell(layer, left + k, top + row));
    }
    interrupt_mask_after_vdp_access();
    return return_to_caller(41 * 29 * 40 + 400);
}

/* sub_00D950: clear the plane at d0 with the blank cell (keeps registers). */
static uint32_t clear_plane_at(uint32_t pc) {
    (void)pc;
    clear_plane((uint16_t)D(0));
    interrupt_mask_after_vdp_access();
    return return_to_caller(41 * 29 * 12 + 300);
}

/* sub_00DC26: redraw a changed map rectangle (doors, switches...), clipped to the
 * columns kept in VRAM. In: d1/d2 map column/row, d3/d4 width-1/height-1, a1 source
 * cells (stride = map width). Maps that fill the plane take the original path. */
static uint32_t redraw_rect(uint32_t pc) {
    (void)pc;
    uint32_t layer = A(3);
    if (rd8(layer + 0x27) & 2) return game->redraw_rect + 0xEE;   /* past the replaced part ($DD14) */
    int col = (int16_t)D(1) - L16(layer, 0x08), row = (int16_t)D(2) - L16(layer, 0x0A);
    int cols = (int16_t)D(3) + 1, rows = (int16_t)D(4) + 1;
    uint32_t src = A(1), stride = (uint32_t)(uint16_t)L16(layer, 0x20) * 2;
    if (col < -pad) { cols += col + pad; src += (uint32_t)(-pad - col) * 2; col = -pad; }
    if (cols > 42 + pad - col) cols = 42 + pad - col;
    if (row < 0) { rows += row; src += (uint32_t)(-row) << L16(layer, 0x24); row = 0; }
    if (rows > last_row() + 2 - row) rows = last_row() + 2 - row;
    if (cols <= 0 || rows <= 0) return return_to_caller(100);
    int vleft = (L16(layer, 0x0C) >> 1) & 63, vtop = (L16(layer, 0x0C) >> 7) & 31;
    uint32_t base = (uint16_t)L16(layer, 0x1A);
    for (int r = 0; r < rows; r++)
        for (int k = 0; k < cols; k++) vram_cell(base, vtop + row + r, vleft + col + k, rd16(src + r * stride + (uint32_t)k * 2));
    port_registers();
    interrupt_mask_after_vdp_access();
    return return_to_caller(rows * cols * 30 + 300);
}

/* The extra columns only show real content while a map layer is active (gameplay and
 * in-engine scenes). Logos, the title, menus and story scenes are drawn for 320 pixels,
 * so their sides are masked to black. */
void widescreen_frame(void) {
    vdp_opts.mask_sides = !((rd8(0xFF1716 + 0x27) | rd8(0xFF173E + 0x27)) & 1);
}

void widescreen_patches(int extra_width) {
    pad = (extra_width + 7) / 8;
    if (pad > 11) pad = 11;
    bool on = extra_width > 0;
    /* Sprite culling: keep pieces in the side areas. The right side stops at hardware
     * X = $1FF (screen 383); past that the 9-bit X would wrap to the left edge. */
    int right = extra_width < 63 ? extra_width : 63;
    patch_cull_left = (uint32_t)extra_width;
    patch_cam_margin = extra_width;
    patch_cull_widen = (uint32_t)(extra_width + right);
    if (!game) return;                  /* no ROM loaded yet: hooked when one is */
    machine_override(game->scroll_column, on ? scroll_column : NULL);
    machine_override(game->scroll_row, on ? scroll_row : NULL);
    machine_override(game->init_layer, on ? init_layer : NULL);
    machine_override(game->clear_plane_at, on ? clear_plane_at : NULL);
    machine_override(game->redraw_rect, on ? redraw_rect : NULL);
}
