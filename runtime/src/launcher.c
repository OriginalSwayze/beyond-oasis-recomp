/* Launcher: a settings screen shown before the game, with the game presented as a
 * collector's steelbook: brushed steel, the Gold Armlet and title logo embossed into it,
 * and a holographic foil finish whose light follows the mouse like tilting a real case.
 *
 * The artwork comes from the user's own ROM: the launcher runs the game's intro
 * silently and captures the armlet scene and the title screen, then caches them in
 * launcher_art.cache next to the executable. Everything is drawn in software into one
 * ui_px (also used for headless screenshots), at the window's pixel scale. */
#include "launcher.h"
#include "genesis.h"
#include "settings.h"
#include "ui.h"
#include "png.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

#define BASE_W UI_BASE_W
#define BASE_H UI_BASE_H

static uint32_t *background;            /* static parts of the frame, drawn once per size */

/* ---- Artwork captured from the ROM -------------------------------------------- */
typedef struct { int w, h; uint32_t *rgb; uint8_t *mask; } Art;
static Art armlet, logo;

#define ART_MAGIC    0x34524142u       /* cache file tag (bump when the capture changes) */

static char cache_path[1100];

static void art_free(Art *a) { free(a->rgb); free(a->mask); memset(a, 0, sizeof *a); }

/* Crops the pixels of a 320x224 frame that `keep` selects into an Art. */
static void art_crop(Art *a, const uint32_t *fb, int pitch, int (*keep)(uint32_t), int y_limit) {
    int x0 = 320, y0 = 224, x1 = -1, y1 = -1;
    for (int y = 0; y < y_limit; y++)
        for (int x = 0; x < 320; x++)
            if (keep(fb[y * pitch + x])) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    art_free(a);
    if (x1 < 0) return;
    a->w = x1 - x0 + 1; a->h = y1 - y0 + 1;
    a->rgb = malloc((size_t)a->w * a->h * 4);
    a->mask = malloc((size_t)a->w * a->h);
    for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w; x++) {
            uint32_t p = fb[(y0 + y) * pitch + x0 + x];
            a->rgb[y * a->w + x] = p;
            a->mask[y * a->w + x] = keep(p) ? 255 : 0;
        }
}

static int is_lit(uint32_t p) { int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255; return r > 28 || g > 28 || b > 28; }
static int is_gold(uint32_t p) { int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255; return r > 100 && r >= g && r > b + 50; }
/* Title lettering: gold faces and their dark brown outlines (the stone behind is blue-green). */
static int is_letter(uint32_t p) { int r = (p >> 16) & 255, b = p & 255; return r >= b + 6; }
static int is_teal(uint32_t p) { int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255; return g > r + 12 && b > r + 12; }

static int count(const uint32_t *fb, int (*pred)(uint32_t), int y_limit) {
    int n = 0;
    for (int i = 0; i < 320 * y_limit; i++) n += pred(fb[i]);
    return n;
}

/* The intro's Gold Armlet scene: a large gold object on black, fully on screen and still. */
static bool armlet_shown(const uint32_t *fb, int *last) {
    int gold = count(fb, is_gold, 224), lit = count(fb, is_lit, 224);
    bool edge = false;
    for (int y = 0; y < 224; y++) edge |= is_lit(fb[y * 320 + 318]) || is_lit(fb[y * 320 + 1]);
    bool still = abs(gold - *last) < 40;
    *last = gold;
    return gold > 4000 && lit < 320 * 224 / 2 && !edge && still;
}

/* The title screen: gold lettering over the teal stone background, finished fading in. */
static bool title_shown(const uint32_t *fb, int *last) {
    int gold = count(fb, is_gold, 140), teal = count(fb, is_teal, 224);
    bool still = abs(gold - *last) < 20;
    *last = gold;
    return gold > 900 && teal > 25000 && still;
}

/* Runs the game until `shown` accepts a frame (checked every few frames), pressing Start
 * every `press_every` frames if nonzero. Returns false if it never appears. */
static bool run_until(uint32_t *fb, bool (*shown)(const uint32_t *, int *), int press_every, int limit) {
    int last = -1000;
    for (int f = 0; f < limit; f++) {
        pad_state[0] = press_every && f >= 500 && f % press_every < 4 ? PAD_START : 0;
        bool check = f >= 300 && f % 4 == 0;
        machine_run_frame(check ? fb : NULL, 320);
        if (check && shown(fb, &last)) { pad_state[0] = 0; return true; }
    }
    pad_state[0] = 0;
    return false;
}

static bool art_save_cache(void) {
    FILE *f = fopen(cache_path, "wb");
    if (!f) return false;
    uint32_t magic = ART_MAGIC;
    fwrite(&magic, 4, 1, f);
    Art *arts[2] = { &armlet, &logo };
    for (int i = 0; i < 2; i++) {
        fwrite(&arts[i]->w, 4, 1, f); fwrite(&arts[i]->h, 4, 1, f);
        fwrite(arts[i]->rgb, 4, (size_t)arts[i]->w * arts[i]->h, f);
        fwrite(arts[i]->mask, 1, (size_t)arts[i]->w * arts[i]->h, f);
    }
    fclose(f);
    return true;
}

static bool art_load_cache(void) {
    FILE *f = fopen(cache_path, "rb");
    if (!f) return false;
    uint32_t magic = 0;
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == ART_MAGIC;
    Art *arts[2] = { &armlet, &logo };
    for (int i = 0; ok && i < 2; i++) {
        Art *a = arts[i];
        art_free(a);
        ok = fread(&a->w, 4, 1, f) == 1 && fread(&a->h, 4, 1, f) == 1 && a->w > 0 && a->h > 0 && a->w <= 320 && a->h <= 224;
        if (!ok) break;
        a->rgb = malloc((size_t)a->w * a->h * 4);
        a->mask = malloc((size_t)a->w * a->h);
        ok = fread(a->rgb, 4, (size_t)a->w * a->h, f) == (size_t)a->w * a->h &&
             fread(a->mask, 1, (size_t)a->w * a->h, f) == (size_t)a->w * a->h;
    }
    fclose(f);
    if (!ok) { art_free(&armlet); art_free(&logo); }
    return ok;
}

/* Runs the intro twice (about 1.5 s) to capture the armlet and the title logo. */
static void art_capture(void) {
    static uint32_t fb[320 * 240];
    int extra = vdp_opts.extra_width;
    bool mask = vdp_opts.mask_sides;
    vdp_opts.extra_width = 0;
    widescreen_patches(0);
    /* Start from blank save RAM so the intro plays the same for everyone. */
    uint8_t *sram = malloc(SRAM_SIZE);
    memcpy(sram, bus.sram, SRAM_SIZE);
    memset(bus.sram, 0xFF, SRAM_SIZE);
    machine_reset();
    if (run_until(fb, armlet_shown, 0, 2400)) art_crop(&armlet, fb, 320, is_lit, 224);
    machine_reset();
    if (run_until(fb, title_shown, 100, 3000)) {
        /* Find the lettering by its gold, then take whole letterforms inside that box. */
        Art gold = { 0 };
        art_crop(&gold, fb, 320, is_gold, 140);
        art_crop(&logo, fb, 320, is_letter, 140);
        art_free(&gold);
        /* Close one-pixel gaps in the strokes, taking the color of a neighboring letter pixel. */
        uint8_t *m = malloc((size_t)logo.w * logo.h);
        memcpy(m, logo.mask, (size_t)logo.w * logo.h);
        for (int y = 1; y < logo.h - 1; y++)
            for (int x = 1; x < logo.w - 1; x++) {
                if (logo.mask[y * logo.w + x]) continue;
                int n = 0, from = -1;
                for (int k = 0; k < 9; k++) {
                    int i = (y - 1 + k / 3) * logo.w + x - 1 + k % 3;
                    if (logo.mask[i]) { n++; from = i; }
                }
                if (n >= 5) { m[y * logo.w + x] = 255; logo.rgb[y * logo.w + x] = logo.rgb[from]; }
            }
        memcpy(logo.mask, m, (size_t)logo.w * logo.h);
        free(m);
    }
    memcpy(bus.sram, sram, SRAM_SIZE);
    free(sram);
    vdp_opts.extra_width = extra;
    vdp_opts.mask_sides = mask;
    widescreen_patches(extra);
    pad_state[0] = 0;
    if (armlet.w && logo.w) art_save_cache();
}

static bool rom_ok;

static void art_prepare(void) {
    art_free(&armlet); art_free(&logo);
    rom_ok = settings.rom_path[0] && !bus_load_rom(settings.rom_path);
    if (!rom_ok) return;
    if (!art_load_cache()) art_capture();
}

/* ---- The steelbook -------------------------------------------------------------- */
#define CASE_X 84
#define CASE_Y 54
#define CASE_W 408
#define CASE_H 612
#define CASE_R 16

enum { MAT_STEEL, MAT_FOIL, MAT_ART, MAT_GOLD };

static int sw, sh;                      /* case size in pixels */
static float *height;                   /* emboss height field */
static uint8_t *mat, *coverage, *brush;
static float *nrm_x, *nrm_y, *nrm_z;    /* surface normals of the height field */
static uint32_t *art_color;

static void blur(float *f, int w, int h, int r) {
    if (r < 1) return;
    float *tmp = malloc(sizeof(float) * (size_t)w * h);
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float s = 0; int n = 0;
                for (int k = -r; k <= r; k++) { int xx = x + k; if (xx >= 0 && xx < w) { s += f[y * w + xx]; n++; } }
                tmp[y * w + x] = s / n;
            }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float s = 0; int n = 0;
                for (int k = -r; k <= r; k++) { int yy = y + k; if (yy >= 0 && yy < h) { s += tmp[yy * w + x]; n++; } }
                f[y * w + x] = s / n;
            }
    }
    free(tmp);
}

/* Places `a` (scaled to `width` layout units, centered at cx, top at top) into the case
 * layers. Returns the bottom edge in layout units. */
static float place_art(const Art *a, float cx, float top, float width, int material, float lift, float detail) {
    if (!a->w) return top;
    float k = width / a->w, h = a->h * k;
    float *m = calloc((size_t)sw * sh, sizeof(float));
    int x0 = (int)((cx - width / 2) * ui_s), y0 = (int)(top * ui_s), pw = (int)(width * ui_s), ph = (int)(h * ui_s);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int X = x0 + x, Y = y0 + y;
            if (X < 0 || Y < 0 || X >= sw || Y >= sh) continue;
            /* Bilinear sample, weighting colors by the mask so edges don't pick up the background. */
            float fx = (x + .5f) * a->w / pw - .5f, fy = (y + .5f) * a->h / ph - .5f;
            int ix = (int)floorf(fx), iy = (int)floorf(fy);
            float tx = fx - ix, ty = fy - iy, cov = 0;
            Rgb cc = rgb(0, 0, 0);
            for (int k = 0; k < 4; k++) {
                int sx = ix + (k & 1), sy = iy + (k >> 1);
                if (sx < 0 || sy < 0 || sx >= a->w || sy >= a->h) continue;
                float wgt = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty) * a->mask[sy * a->w + sx] / 255.f;
                cc = add(cc, unpack(a->rgb[sy * a->w + sx]), wgt);
                cov += wgt;
            }
            if (cov < .5f) continue;
            cc = scale(cc, 1 / cov);
            m[Y * sw + X] = 1;
            mat[Y * sw + X] = (uint8_t)material;
            art_color[Y * sw + X] = pack(cc);
            height[Y * sw + X] += (cc.r * .3f + cc.g * .5f + cc.b * .2f) * detail;
        }
    blur(m, sw, sh, (int)(1.5f * ui_s + .5f));
    for (int i = 0; i < sw * sh; i++) height[i] += m[i] * lift;
    free(m);
    return top + h;
}

static void place_text(const char *s, const char *face, float px, int weight, float tracking, float cy, float depth) {
    const TextMask *t = ui_text_get(s, face, px, weight, tracking);
    if (!t->a) return;
    int x0 = (sw - t->w) / 2, y0 = (int)(cy * ui_s) - t->h / 2;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            int X = x0 + x, Y = y0 + y;
            if (X < 0 || Y < 0 || X >= sw || Y >= sh) continue;
            float a = t->a[y * t->w + x] / 255.f;
            height[Y * sw + X] -= a * depth;
            if (a > .5f) mat[Y * sw + X] = MAT_FOIL;
        }
}

static void build_case(void) {
    sw = (int)(CASE_W * ui_s); sh = (int)(CASE_H * ui_s);
    size_t n = (size_t)sw * sh;
    free(height); free(mat); free(coverage); free(brush); free(art_color);
    free(nrm_x); free(nrm_y); free(nrm_z);
    height = calloc(n, sizeof(float));
    mat = calloc(n, 1); coverage = calloc(n, 1); brush = malloc(n);
    art_color = calloc(n, 4);

    /* Brushed steel: horizontal streaks that drift slowly along each row. */
    for (int y = 0; y < sh; y++) {
        float row = hashf((uint32_t)y * 7919u);
        for (int x = 0; x < sw; x++) {
            float drift = hashf((uint32_t)(y * 31 + x / (int)(37 * ui_s + 1)) * 104729u);
            float fine = hashf((uint32_t)(y * 8191 + x) * 2654435761u);
            brush[y * sw + x] = (uint8_t)((row * .55f + drift * .25f + fine * .2f) * 255);
        }
    }

    float frame_in = 18, cxu = CASE_W / 2.f;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            float ux = x / ui_s, uy = y / ui_s;
            float d = ui_rrect_dist(ux, uy, 0, 0, CASE_W, CASE_H, CASE_R);
            coverage[y * sw + x] = (uint8_t)(clampf(.5f - d * ui_s, 0, 1) * 255);
            float h = smooth(0, 7, -d) * .9f;                              /* rounded bevel at the edge */
            float d1 = ui_rrect_dist(ux, uy, frame_in, frame_in, CASE_W - 2 * frame_in, CASE_H - 2 * frame_in, 8);
            float d2 = ui_rrect_dist(ux, uy, frame_in + 7, frame_in + 7, CASE_W - 2 * frame_in - 14, CASE_H - 2 * frame_in - 14, 5);
            float line1 = 1 - smooth(.6f, 1.8f, fabsf(d1)), line2 = 1 - smooth(.3f, 1.1f, fabsf(d2));
            h += line1 * .55f + line2 * .3f;                                 /* raised double frame */
            if (line1 > .4f || line2 > .5f) mat[y * sw + x] = MAT_FOIL;
            /* Engraved sunburst behind the art, fading toward the frame. */
            float dx = ux - cxu, dy = uy - 360;
            float ang = atan2f(dy, dx), r = sqrtf(dx * dx + dy * dy);
            float ray = .5f + .5f * cosf(ang * 72);
            h += ray * .05f * smooth(40, 120, r) * (1 - smooth(250, 340, r)) * (d1 < -2);
            if (ux < 26) h -= (1 - smooth(1, 3.5f, fabsf(ux - 22))) * .6f;   /* hinge groove */
            height[y * sw + x] = h;
        }

    float logo_bottom = place_art(&logo, cxu, 46, 330, MAT_GOLD, 1.0f, .7f);
    float art_top = logo_bottom + 34;
    place_art(&armlet, cxu, art_top, 346, MAT_ART, .9f, .6f);
    place_text("NATIVE PC EDITION", "Segoe UI Semibold", 13, 600, 5, CASE_H - 52, .7f);
    if (!logo.w) place_text("BEYOND OASIS", "Palatino Linotype", 46, 700, 6, 120, .9f);

    nrm_x = malloc(n * sizeof(float)); nrm_y = malloc(n * sizeof(float)); nrm_z = malloc(n * sizeof(float));
    float bump = 2.2f * ui_s;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int i = y * sw + x;
            float hl = height[i - (x > 0)], hr = height[i + (x < sw - 1)];
            float hu = height[i - (y > 0) * sw], hd = height[i + (y < sh - 1) * sw];
            float nx = (hl - hr) * bump, ny = (hu - hd) * bump, nn = sqrtf(nx * nx + ny * ny + 1);
            nrm_x[i] = nx / nn; nrm_y[i] = ny / nn; nrm_z[i] = 1 / nn;
        }
}

/* Shades the case for light direction (lx, ly) and time t, onto the ui_px. The work is
 * split by rows across threads; normals were precomputed with the height field. */
typedef struct { float lx, ly, t; int y0, y1; } ShadeJob;

static inline float gauss(float x2k) {  /* ~exp(-x2k), cheap */
    float q = 1 - x2k * .25f;
    if (q <= 0) return 0;
    q *= q;
    return q * q;
}

static int shade_rows(void *arg) {
    const ShadeJob *j = arg;
    float lx = j->lx, ly = j->ly, t = j->t;
    int ox = (int)(CASE_X * ui_s), oy = (int)(CASE_Y * ui_s);
    float ln = sqrtf(lx * lx + ly * ly + 1);
    float Lx = lx / ln, Ly = ly / ln, Lz = 1 / ln;
    float hx = Lx, hy = Ly, hz = Lz + 1, hn = sqrtf(hx * hx + hy * hy + hz * hz);
    hx /= hn; hy /= hn; hz /= hn;
    float cell = 5 * ui_s;
    Rgb steel_tint = rgb(.90f, .94f, 1.0f);
    float glint_at = .7f + .6f * lx + .3f * ly;
    for (int y = j->y0; y < j->y1; y++) {
        float v = (float)y / sh;
        float bd = (v - .5f - ly * .42f) * 3.6f;
        float band = gauss(bd * bd);
        uint32_t *row = ui_px + (size_t)(oy + y) * ui_w + ox;
        int gy = (int)(y / cell);
        for (int x = 0; x < sw; x++) {
            int i = y * sw + x;
            uint8_t cov = coverage[i];
            if (!cov) continue;
            float u = (float)x / sw;
            float nx = nrm_x[i], ny = nrm_y[i], nz = nrm_z[i];
            float ndl = fmaxf(0, nx * Lx + ny * Ly + nz * Lz);
            float ndh = fmaxf(0, nx * hx + ny * hy + nz * hz);
            float p2 = ndh * ndh, p4 = p2 * p2, p8 = p4 * p4, p16 = p8 * p8;
            float spec = p16 * p16 * p16;   /* ndh^48 */
            float br = brush[i] / 255.f;

            /* Brushed steel with an anisotropic highlight band that follows the light. */
            float base = .075f + .035f * br;
            Rgb c = scale(steel_tint, base * (.6f + .5f * ndl) + band * (.035f + .07f * br) + spec * .5f);

            /* Diagonal glint that sweeps across as the case tilts. */
            float gd = (u * .8f + v * .6f) - glint_at;
            float glint = gauss(gd * gd * 90);

            uint8_t m = mat[i];
            if (m == MAT_FOIL) {
                Rgb holo = mix(rgb(1, 1, 1), hue(u * .9f + v * .7f + lx * .55f + ly * .35f + (nx + ny) * 1.4f + t * .02f), .78f);
                float k = .16f + .75f * p4;
                c = add(scale(c, .6f), holo, k * .8f);
                c = add(c, rgb(1, 1, 1), glint * .5f + spec * .45f);
            } else if (m == MAT_GOLD) {
                /* Polished gold foil: the logo's own shading picks dark bronze to bright gold. */
                Rgb a = unpack(art_color[i]);
                float lum = a.r * .3f + a.g * .55f + a.b * .15f;
                Rgb gold = mix(rgb_hex(0x3A2408), rgb_hex(0xF7DC92), smooth(.05f, .8f, lum));
                c = scale(gold, .45f + .6f * ndl);
                Rgb holo = mix(rgb(1, 1, 1), hue(u * .9f + v * .7f + lx * .55f + ly * .35f + (nx + ny) * 1.2f), .55f);
                c = add(c, holo, p2 * ndh * .32f * lum);
                c = add(c, rgb(1, .96f, .85f), spec * .7f + glint * .5f);
            } else if (m == MAT_ART) {
                Rgb a = unpack(art_color[i]);
                /* Warm the Genesis palette's lime-yellow toward gold. */
                float lum = a.r * .3f + a.g * .55f + a.b * .15f;
                bool goldish = a.r > a.b + .15f && a.g > a.b + .1f;
                if (goldish) a = mix(a, mix(rgb_hex(0x4A2E0A), rgb_hex(0xFFE3A0), lum), .45f);
                c = scale(a, .42f + .38f * ndl);
                c = add(c, rgb(1, .88f, .58f), p16 * p8 * p4 * p2 * .45f);
                c = add(c, hue(u * .6f + v * .5f + lx * .5f + ly * .4f), p4 * .10f);
                c = add(c, rgb(1, .95f, .85f), glint * .28f);
            } else {
                c = add(c, rgb(.85f, .9f, 1), glint * .10f);
            }

            /* Sparkles: sparse points that flash at particular angles. */
            int gx = (int)(x / cell);
            uint32_t hcell = (uint32_t)(gx * 73856093) ^ (uint32_t)(gy * 19349663);
            float hp = hashf(hcell);
            if (hp < (m == MAT_STEEL ? .02f : m == MAT_ART ? .03f : .08f)) {
                float sx = (gx + .2f + .6f * hashf(hcell + 1)) * cell, sy = (gy + .2f + .6f * hashf(hcell + 2)) * cell;
                float d2 = ((x - sx) * (x - sx) + (y - sy) * (y - sy)) / (ui_s * ui_s);
                if (d2 < 3.5f * 3.5f) {
                    float sn = fmaxf(0, sinf(hashf(hcell + 3) * 40 + lx * 7 + ly * 5 + t * .6f));
                    float f2 = sn * sn, f4 = f2 * f2, f8 = f4 * f4, f16 = f8 * f8;
                    float flash = f16 * f8 * f4 * f2;   /* ^30 */
                    float dd = sqrtf(d2);
                    float star = fmaxf(0, 1 - dd / 1.6f) + .35f * fmaxf(0, 1 - dd / 3.5f);
                    c = add(c, mix(rgb(1, 1, 1), hue(hp * 9 + lx), .3f), flash * star);
                }
            }
            if (cov == 255) row[x] = pack(c);
            else row[x] = pack(mix(unpack(row[x]), c, cov / 255.f));
        }
    }
    return 0;
}

static void draw_case(float lx, float ly, float t) {
    enum { MAX_THREADS = 16 };
    int n = SDL_GetCPUCount();
    if (n > MAX_THREADS) n = MAX_THREADS;
    if (n < 1) n = 1;
    ShadeJob jobs[MAX_THREADS];
    SDL_Thread *threads[MAX_THREADS];
    for (int k = 0; k < n; k++) {
        jobs[k] = (ShadeJob){ lx, ly, t, sh * k / n, sh * (k + 1) / n };
        threads[k] = k ? SDL_CreateThread(shade_rows, "case", &jobs[k]) : NULL;
    }
    shade_rows(&jobs[0]);
    for (int k = 1; k < n; k++) {
        if (threads[k]) SDL_WaitThread(threads[k], NULL);
        else shade_rows(&jobs[k]);
    }
}

/* ---- Background ----------------------------------------------------------------- */
static void build_background(void) {
    free(background);
    background = malloc((size_t)ui_w * ui_h * 4);
    for (int y = 0; y < ui_h; y++)
        for (int x = 0; x < ui_w; x++) {
            float ux = x / ui_s, uy = y / ui_s;
            float r = sqrtf((ux - 760) * (ux - 760) + (uy - 300) * (uy - 300));
            Rgb c = mix(rgb_hex(0x1A1E29), rgb_hex(0x07080C), smooth(0, 900, r));
            /* Warm desert glow behind the case. */
            float g = sqrtf((ux - 290) * (ux - 290) + (uy - 380) * (uy - 380));
            c = add(c, rgb(.55f, .33f, .10f), .22f * expf(-g * g / (2 * 330.f * 330.f)));
            /* Soft shadow under the case. */
            float d = ui_rrect_dist(ux, uy, CASE_X + 10, CASE_Y + 22, CASE_W, CASE_H, CASE_R);
            c = scale(c, 1 - .65f * (1 - smooth(-6, 34, d)));
            float dither = (hashf((uint32_t)(y * ui_w + x)) - .5f) / 255.f;
            c = add(c, rgb(1, 1, 1), dither);
            background[y * ui_w + x] = pack(c);
        }
}

/* ---- Settings rows -------------------------------------------------------------- */
typedef enum { ROW_HEADER, ROW_OPTION, ROW_ROM } RowKind;
typedef struct { RowKind kind; const char *label; int id; } Row;

static const Row rows[] = {
    { ROW_HEADER, "DISPLAY", 0 },
    { ROW_OPTION, "Widescreen", OPT_WIDESCREEN },
    { ROW_OPTION, "Aspect ratio", OPT_ASPECT },
    { ROW_OPTION, "Fullscreen", OPT_FULLSCREEN },
    { ROW_OPTION, "Integer scaling", OPT_INTEGER },
    { ROW_OPTION, "Window size", OPT_WINDOW },
    { ROW_HEADER, "ENHANCEMENTS", 0 },
    { ROW_OPTION, "Internal resolution", OPT_INTERNAL },
    { ROW_OPTION, "Sprite flicker fix", OPT_FLICKER },
    { ROW_OPTION, "Rewind", OPT_REWIND },
    { ROW_OPTION, "Rewind buffer", OPT_REWIND_MB },
    { ROW_HEADER, "GAME", 0 },
    { ROW_ROM, "ROM", 0 },
    { ROW_OPTION, "Cheats", OPT_CHEATS },
    { ROW_OPTION, "Show launcher at startup", OPT_LAUNCHER },
};
#define ROW_COUNT ((int)(sizeof rows / sizeof *rows))

/* ---- Layout --------------------------------------------------------------------- */
#define PANEL_X 572
#define PANEL_R 1196
#define ROWS_Y  176
#define ROW_H   29
#define HEAD_H  26

/* Selectable items: the option/ROM rows, then PLAY and QUIT. */
enum { ITEM_PLAY = 100, ITEM_QUIT = 101 };
static int selected = 1;
static bool play_enabled(void) { return rom_ok; }

static float row_y(int r) {
    float y = ROWS_Y;
    for (int i = 0; i < r; i++) y += rows[i].kind == ROW_HEADER ? HEAD_H : ROW_H;
    return y;
}

#define BTN_Y 604
#define BTN_H 54
#define PLAY_X PANEL_X
#define PLAY_W 300
#define QUIT_X (PANEL_X + 320)
#define QUIT_W 150

static int hit_test(float mx, float my) {
    for (int r = 0; r < ROW_COUNT; r++) {
        if (rows[r].kind == ROW_HEADER) continue;
        float y = row_y(r);
        if (mx >= PANEL_X - 12 && mx <= PANEL_R + 4 && my >= y && my < y + ROW_H) return r;
    }
    if (my >= BTN_Y && my <= BTN_Y + BTN_H) {
        if (mx >= PLAY_X && mx <= PLAY_X + PLAY_W) return ITEM_PLAY;
        if (mx >= QUIT_X && mx <= QUIT_X + QUIT_W) return ITEM_QUIT;
    }
    return -1;
}

static void move_selection(int dir) {
    int order[ROW_COUNT + 2], n = 0, at = 0;
    for (int r = 0; r < ROW_COUNT; r++) if (rows[r].kind != ROW_HEADER) order[n++] = r;
    order[n++] = ITEM_PLAY; order[n++] = ITEM_QUIT;
    for (int i = 0; i < n; i++) if (order[i] == selected) at = i;
    selected = order[settings_cycle(at, n, dir)];
}

static void draw_button(float x, float w, const char *label, bool primary, bool focus, bool enabled, float lx, float ly, float t) {
    ui_button(x, BTN_Y, w, BTN_H, label, 20, primary, focus, enabled, lx, ly, t);
}

static char rom_status[96];

static void draw_ui(float lx, float ly, float t) {
    Rgb gold_top = rgb_hex(0xFBEBB8), gold_bot = rgb_hex(0xC08A2C);
    ui_draw_text("BEYOND OASIS", "Palatino Linotype", 54, 700, 7, PANEL_X - 3, 50, gold_top, gold_bot, 1);
    ui_draw_text("NATIVE PC EDITION  \xC2\xB7  RECOMPILED FROM YOUR CARTRIDGE", "Segoe UI", 12.5f, 600, 3.5f, PANEL_X, 122,
              rgb_hex(0x99A2B4), rgb_hex(0x99A2B4), 1);
    /* Divider fading out to the right. */
    for (int px = (int)(PANEL_X * ui_s); px < (int)(PANEL_R * ui_s); px++) {
        float u = (px / ui_s - PANEL_X) / (PANEL_R - PANEL_X);
        for (int py = (int)(152 * ui_s); py < (int)(152 * ui_s) + (ui_s > 1.5f ? 2 : 1); py++)
            ui_blend_px(px, py, mix(rgb_hex(0xD9B567), rgb_hex(0x2A2F3B), u), .9f * (1 - u * .8f));
    }

    for (int r = 0; r < ROW_COUNT; r++) {
        float y = row_y(r);
        const Row *row = &rows[r];
        if (row->kind == ROW_HEADER) {
            ui_draw_text(row->label, "Segoe UI Semibold", 11.5f, 600, 3.5f, PANEL_X, y + 12, rgb_hex(0xC9A45C), rgb_hex(0xC9A45C), 1);
            continue;
        }
        bool focus = selected == r;
        if (focus) {
            ui_fill_rect(PANEL_X - 12, y + 1, PANEL_R - PANEL_X + 16, ROW_H - 2, rgb_hex(0xD9B567), .07f);
            ui_fill_rect(PANEL_X - 12, y + 1, 3, ROW_H - 2, rgb_hex(0xE8C77A), .95f);
        }
        Rgb lc = focus ? rgb_hex(0xFFFFFF) : rgb_hex(0xC5CAD5);
        ui_draw_text(row->label, "Segoe UI", 16.5f, 400, 0, PANEL_X + 2, y + 5, lc, lc, 1);
        char value[96];
        Rgb vc = focus ? rgb_hex(0xF3DDA4) : rgb_hex(0xE6E9EF);
        if (row->kind == ROW_ROM) {
            snprintf(value, sizeof value, "%s", rom_status);
            vc = rom_ok ? rgb_hex(0x8FD19E) : rgb_hex(0xE58A7A);
            if (focus) {
                const char *hint = "Enter to choose\xE2\x80\xA6";
                float hw = ui_text_width(hint, "Segoe UI", 13, 400, 0);
                float vw = ui_text_width(value, "Segoe UI Semibold", 16.5f, 600, 0);
                ui_draw_text(hint, "Segoe UI", 13, 400, 0, PANEL_R - vw - hw - 18, y + 8, rgb_hex(0x8C93A3), rgb_hex(0x8C93A3), 1);
            }
        } else {
            settings_option_text(row->id, value, sizeof value);
        }
        float vw = ui_text_width(value, "Segoe UI Semibold", 16.5f, 600, 0);
        float vx = PANEL_R - vw;
        if (row->kind == ROW_OPTION) {
            vx -= 22;
            Rgb ac = focus ? rgb_hex(0xE8C77A) : rgb_hex(0x5B6274);
            ui_draw_text("\xE2\x80\xB9", "Segoe UI Semibold", 18, 600, 0, vx - 18, y + 3, ac, ac, 1);
            ui_draw_text("\xE2\x80\xBA", "Segoe UI Semibold", 18, 600, 0, PANEL_R - 12, y + 3, ac, ac, 1);
        }
        ui_draw_text(value, "Segoe UI Semibold", 16.5f, 600, 0, vx, y + 5, vc, vc, 1);
    }

    draw_button(PLAY_X, PLAY_W, "PLAY", true, selected == ITEM_PLAY, play_enabled(), lx, ly, t);
    draw_button(QUIT_X, QUIT_W, "QUIT", false, selected == ITEM_QUIT, true, lx, ly, t);
    const char *keys = "\xE2\x86\x91\xE2\x86\x93 select     \xE2\x86\x90\xE2\x86\x92 change     Enter  select     Esc  quit";
    ui_draw_text(keys, "Segoe UI", 12, 400, 1, PANEL_X, 684, rgb_hex(0x6C7385), rgb_hex(0x6C7385), 1);
}

/* ---- Frame -------------------------------------------------------------------- */
static void layout(int pixel_w) {
    ui_begin(pixel_w, (int)(BASE_H * (pixel_w / (float)BASE_W) + .5f));
    build_background();
    build_case();
}

static void render(float lx, float ly, float t) {
    memcpy(ui_px, background, (size_t)ui_w * ui_h * 4);
    draw_case(lx, ly, t);
    draw_ui(lx, ly, t);
}

static void update_rom_status(void) {
    if (rom_ok) snprintf(rom_status, sizeof rom_status, "Verified \xE2\x9C\x93");
    else if (!settings.rom_path[0]) snprintf(rom_status, sizeof rom_status, "Not selected");
    else snprintf(rom_status, sizeof rom_status, "Not the USA cartridge");
}

static void init_common(void) {
    char *bp = SDL_GetBasePath();
    snprintf(cache_path, sizeof cache_path, "%slauncher_art.cache", bp ? bp : "");
    if (bp) SDL_free(bp);
    art_prepare();
    update_rom_status();
}

static void art_write_png(const Art *a, const char *base, const char *suffix) {
    if (!a->w) return;
    char p[1200];
    snprintf(p, sizeof p, "%s%s", base, suffix);
    png_write(p, a->rgb, a->w, a->h, a->w);
}

int launcher_screenshot(const char *png, int width, float lx, float ly, float t) {
    init_common();
    art_write_png(&armlet, png, ".armlet.png");
    art_write_png(&logo, png, ".logo.png");
    layout(width);
    Uint64 t0 = SDL_GetPerformanceCounter();
    for (int i = 0; i < 10; i++) render(lx, ly, t);
    printf("launcher frame: %.1f ms at %dx%d\n", (SDL_GetPerformanceCounter() - t0) * 100.0 / SDL_GetPerformanceFrequency(), ui_w, ui_h);
    return png_write(png, ui_px, ui_w, ui_h, ui_w);
}

/* ---- ROM picker ------------------------------------------------------------------ */
static void choose_rom(SDL_Window *win) {
#ifdef _WIN32
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof ofn;
    (void)win;
    ofn.lpstrFilter = L"Genesis / Mega Drive ROM (*.md;*.bin;*.gen;*.smd)\0*.md;*.bin;*.gen;*.smd\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"Choose your Beyond Oasis (USA) ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    char path[1024];
    WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL);
    for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
    snprintf(settings.rom_path, sizeof settings.rom_path, "%s", path);
    remove(cache_path);
    art_prepare();
    update_rom_status();
    build_case();
#else
    (void)win;
#endif
}

/* ---- Interactive loop --------------------------------------------------------------- */
int launcher_run(const char *ini_path) {
    init_common();
    SDL_Window *win = SDL_CreateWindow("Beyond Oasis", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, BASE_W, BASE_H,
                                       SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) return 1;
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    int ow, oh;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    layout(ow);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, ui_w, ui_h);
    SDL_GameController *pad = NULL;
    if (!rom_ok) selected = 11;         /* the ROM row */
    else selected = ITEM_PLAY;

    float lx = 0, ly = 0, tx = 0, ty = 0;
    Uint64 freq = SDL_GetPerformanceFrequency(), start = SDL_GetPerformanceCounter(), last_mouse = 0;
    int result = -1;
    while (result < 0) {
        float t = (float)(SDL_GetPerformanceCounter() - start) / freq;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int activate = 0, change = 0;
            switch (e.type) {
            case SDL_QUIT: result = 0; break;
            case SDL_CONTROLLERDEVICEADDED: if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which); break;
            case SDL_MOUSEMOTION: {
                /* Mouse positions are in window points, which match the 1280x720 layout. */
                tx = clampf((e.motion.x - (CASE_X + CASE_W / 2.f)) / 420.f, -1.2f, 1.2f);
                ty = clampf((e.motion.y - (CASE_Y + CASE_H / 2.f)) / 320.f, -1.2f, 1.2f);
                last_mouse = SDL_GetPerformanceCounter();
                int h = hit_test(e.motion.x, e.motion.y);
                if (h >= 0) selected = h;
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    int h = hit_test(e.button.x, e.button.y);
                    if (h >= 0) { selected = h; activate = 1; }
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    int h = hit_test(e.button.x, e.button.y);
                    if (h >= 0 && h < ROW_COUNT) { selected = h; change = -1; }
                }
                break;
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                case SDLK_UP: case SDLK_w: move_selection(-1); break;
                case SDLK_DOWN: case SDLK_s: case SDLK_TAB: move_selection(1); break;
                case SDLK_LEFT: case SDLK_a: change = -1; break;
                case SDLK_RIGHT: case SDLK_d: change = 1; break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate = 1; break;
                case SDLK_ESCAPE: result = 0; break;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: move_selection(-1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move_selection(1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: change = -1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: change = 1; break;
                case SDL_CONTROLLER_BUTTON_A: activate = 1; break;
                case SDL_CONTROLLER_BUTTON_START: if (play_enabled()) result = 1; break;
                case SDL_CONTROLLER_BUTTON_B: result = 0; break;
                }
                break;
            }
            if (selected < ROW_COUNT && rows[selected].kind == ROW_OPTION && (change || activate))
                settings_option_change(rows[selected].id, change ? change : 1);
            else if (selected < ROW_COUNT && rows[selected].kind == ROW_ROM && activate)
                choose_rom(win);
            else if (activate && selected == ITEM_PLAY && play_enabled()) result = 1;
            else if (activate && selected == ITEM_QUIT) result = 0;
        }

        /* The light follows the mouse; when the mouse rests, the case sways gently. */
        float idle = last_mouse ? (float)(SDL_GetPerformanceCounter() - last_mouse) / freq : 99;
        float auto_x = .55f * sinf(t * .45f), auto_y = .35f * cosf(t * .33f);
        float w = smooth(2.5f, 5, idle);
        float gx = tx * (1 - w) + auto_x * w, gy = ty * (1 - w) + auto_y * w;
        lx += (gx - lx) * .12f; ly += (gy - ly) * .12f;

        render(lx, ly, t);
        SDL_UpdateTexture(tex, NULL, ui_px, ui_w * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    settings_save(ini_path);
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return result;
}
