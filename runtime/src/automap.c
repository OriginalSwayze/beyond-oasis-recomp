/* The auto-map: see automap.h.
 *
 * World coordinates: the planes' pixel at picture x (widescreen sides included) shows the
 * world at x - extra + camera, as the camera is where the original 320-wide screen
 * starts. A map is (map width + 320) x (map height + 224) pixels: $FF177A/$FF177C hold
 * how far the camera can go. */
#include "automap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis.h"
#include "ui.h"

#define CELL 4                          /* world pixels per map pixel */
#define MAX_MAPS 160
#define MAX_SIDE 640                    /* map pixels; larger areas are cropped */

typedef struct {
    uint32_t key;
    int w, h;                           /* map pixels */
    uint16_t *px;                       /* RGB565 + 1 (0 = not seen) */
    uint32_t used;                      /* for dropping the oldest when full */
} Area;

static Area areas[MAX_MAPS];
static int count;
static uint32_t clock_;
static Area *here;
static float ali_wx = -1, ali_wy = -1;  /* world pixels */
static int frame_n;

static uint16_t peek16(uint32_t a) { a &= 0xFFFF; return (uint16_t)(bus.ram[a] << 8 | bus.ram[(a + 1) & 0xFFFF]); }

static uint32_t area_key(void) {
    uint32_t k = 2166136261u;
    for (uint32_t a = 0x16F6; a < 0x16FE; a++) k = (k ^ bus.ram[a]) * 16777619u;
    k = (k ^ peek16(0x177A)) * 16777619u;
    k = (k ^ peek16(0x177C)) * 16777619u;
    return k ? k : 1;
}

static Area *find(uint32_t key, int w, int h, bool make) {
    for (int i = 0; i < count; i++)
        if (areas[i].key == key) { areas[i].used = ++clock_; return &areas[i]; }
    if (!make) return NULL;
    Area *a;
    if (count < MAX_MAPS) a = &areas[count++];
    else {                              /* full: the area not seen for longest goes */
        a = &areas[0];
        for (int i = 1; i < count; i++) if (areas[i].used < a->used) a = &areas[i];
        free(a->px);
    }
    a->key = key; a->w = w; a->h = h; a->used = ++clock_;
    a->px = calloc((size_t)w * h, sizeof *a->px);
    if (!a->px) { a->key = 0; return NULL; }
    return a;
}

static uint16_t to565(uint32_t c) {
    uint16_t v = (uint16_t)(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31));
    return v ? v : 1;                   /* 0 is "not seen" */
}

/* The scenery's colour at a picture pixel: the top plane where it is drawn. */
static uint32_t ground_at(const HdFrame *f, int x, int y) {
    uint32_t a = f->a[y][x], b = f->b[y][x];
    bool ao = (a >> 16) & 0x0F, ah = (a >> 16) & 0x40, bo = (b >> 16) & 0x0F, bh = (b >> 16) & 0x40;
    uint32_t top = ao && (ah || !(bo && bh)) ? a : bo ? b : 0;
    return top ? vdp_color_rgb((uint16_t)top) : 0;
}

void automap_frame(const HdFrame *f, bool world, float cam_x, float cam_y) {
    if (!world || f->h <= 0) { return; }
    int mw = (peek16(0x177A) + 320) / CELL, mh = (peek16(0x177C) + 224) / CELL;
    if (mw <= 0 || mh <= 0) return;
    if (mw > MAX_SIDE) mw = MAX_SIDE;
    if (mh > MAX_SIDE) mh = MAX_SIDE;
    here = find(area_key(), mw, mh, true);
    if (!here) return;
    if (vdp_player_w) { ali_wx = vdp_player_x + vdp_player_w / 2.f + cam_x; ali_wy = vdp_player_y + vdp_player_h - 4.f + cam_y; }
    if (++frame_n % 4) return;          /* the scenery barely moves in four frames */
    for (int y = CELL / 2; y < f->h; y += CELL) {
        int my = (int)((y + cam_y) / CELL);
        if (my < 0 || my >= here->h) continue;
        for (int x = CELL / 2; x < f->w; x += CELL) {
            int mx = (int)((x - f->extra + cam_x) / CELL);
            if (mx < 0 || mx >= here->w) continue;
            uint32_t c = ground_at(f, x, y);
            if (c) here->px[(size_t)my * here->w + mx] = to565(c);
        }
    }
}

bool automap_draw(float x, float y, float w, float h, float t, float alpha) {
    if (!here) return false;
    /* The part seen so far, fitted and centred. */
    int x0 = here->w, y0 = here->h, x1 = -1, y1 = -1;
    for (int my = 0; my < here->h; my++)
        for (int mx = 0; mx < here->w; mx++)
            if (here->px[(size_t)my * here->w + mx]) {
                if (mx < x0) x0 = mx;
                if (mx > x1) x1 = mx;
                if (my < y0) y0 = my;
                if (my > y1) y1 = my;
            }
    if (x1 < 0) return false;
    int sw = x1 - x0 + 1, sh = y1 - y0 + 1;
    float k = w / sw < h / sh ? w / sw : h / sh;    /* layout units per map pixel */
    if (k > 6) k = 6;
    float ox = x + (w - sw * k) / 2, oy = y + (h - sh * k) / 2;
    int px0 = (int)ui_x(ox), py0 = (int)ui_y(oy), px1 = (int)ui_x(ox + sw * k), py1 = (int)ui_y(oy + sh * k);
    for (int py = py0; py < py1; py++) {
        int my = y0 + (int)((float)(py - py0) / (py1 - py0) * sh);
        for (int pxx = px0; pxx < px1; pxx++) {
            int mx = x0 + (int)((float)(pxx - px0) / (px1 - px0) * sw);
            uint16_t v = here->px[(size_t)my * here->w + mx];
            if (!v) continue;
            Rgb c = rgb(((v >> 11) & 31) / 31.f, ((v >> 5) & 63) / 63.f, (v & 31) / 31.f);
            ui_blend_px(pxx, py, c, alpha);
        }
    }
    /* Ali: a blinking gold dot with a dark ring. */
    if (ali_wx >= 0) {
        float ax = ox + (ali_wx / CELL - x0) * k, ay = oy + (ali_wy / CELL - y0) * k;
        float pulse = .6f + .4f * sinf(t * 6);
        float r = ui_x(ax + 7) - ui_x(ax), cx = ui_x(ax), cy = ui_y(ay);
        for (int py = (int)(cy - r - 2); py <= (int)(cy + r + 2); py++)
            for (int pxx = (int)(cx - r - 2); pxx <= (int)(cx + r + 2); pxx++) {
                float dx = pxx + .5f - cx, dy = py + .5f - cy, d = sqrtf(dx * dx + dy * dy);
                if (d < r * .6f) ui_blend_px(pxx, py, rgb_hex(0xFFD24A), alpha * pulse);
                else if (d < r) ui_blend_px(pxx, py, rgb_hex(0x140C08), alpha * .8f);
            }
    }
    return true;
}

/* ---- automap.bin: "BOAM", version, count, then per area key, w, h and its pixels ---------- */
void automap_save(const char *base_dir) {
    char path[1100];
    snprintf(path, sizeof path, "%sautomap.bin", base_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t head[3] = { 0x4D414F42u, 1, (uint32_t)count };
    fwrite(head, sizeof head, 1, f);
    for (int i = 0; i < count; i++) {
        uint32_t a[3] = { areas[i].key, (uint32_t)areas[i].w, (uint32_t)areas[i].h };
        fwrite(a, sizeof a, 1, f);
        fwrite(areas[i].px, sizeof *areas[i].px, (size_t)areas[i].w * areas[i].h, f);
    }
    fclose(f);
}

void automap_load(const char *base_dir) {
    char path[1100];
    snprintf(path, sizeof path, "%sautomap.bin", base_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint32_t head[3];
    if (fread(head, sizeof head, 1, f) == 1 && head[0] == 0x4D414F42u && head[1] == 1)
        for (uint32_t i = 0; i < head[2] && i < MAX_MAPS; i++) {
            uint32_t a[3];
            if (fread(a, sizeof a, 1, f) != 1 || !a[1] || !a[2] || a[1] > MAX_SIDE || a[2] > MAX_SIDE) break;
            Area *ar = find(a[0], (int)a[1], (int)a[2], true);
            if (!ar || ar->w != (int)a[1] || ar->h != (int)a[2]) break;
            if (fread(ar->px, sizeof *ar->px, (size_t)ar->w * ar->h, f) != (size_t)ar->w * ar->h) break;
        }
    fclose(f);
    here = NULL;
}
