/* The steelbook look: dark brushed steel, holographic foil, embossed artwork and a misty
 * night around them.
 *
 * A surface is a height field with a material per pixel, built once per canvas size:
 * brushed steel (streaks that drift along each row), a rounded bevel at the edge, and
 * whatever is placed on it, raised or engraved: holographic frames and rules, gem
 * sockets, the title logo in polished gold, the Gold Armlet in its own colours, stamped
 * text. Every frame it is shaded for the light: diffuse and a tight specular from the
 * normals, a soft anisotropic band across the steel, holographic colour on the foil that
 * shifts with the light and the angle of the surface, a diagonal glint sweeping across,
 * and sparse sparkles that flash at their own angles. The light drifts slowly and leans
 * towards the mouse, like tilting a real case. Rows are shaded on all cores. */
#include "foil.h"

#include <stdlib.h>
#include <string.h>

#include "hd.h"                         /* hd_rows: the renderer's worker threads */

/* ---- Building a surface ---------------------------------------------------------------- */
static void blur(float *f, int w, int h, int r) {
    if (r < 1) return;
    float *tmp = malloc(sizeof(float) * (size_t)w * h);
    if (!tmp) return;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {
            float s = 0;
            int n = 0;
            for (int k = 0; k <= r && k < w; k++) { s += f[y * w + k]; n++; }
            for (int x = 0; x < w; x++) {
                tmp[y * w + x] = s / n;
                int in = x + r + 1, out = x - r;
                if (in < w) { s += f[y * w + in]; n++; }
                if (out >= 0) { s -= f[y * w + out]; n--; }
            }
        }
        for (int x = 0; x < w; x++) {
            float s = 0;
            int n = 0;
            for (int k = 0; k <= r && k < h; k++) { s += tmp[k * w + x]; n++; }
            for (int y = 0; y < h; y++) {
                f[y * w + x] = s / n;
                int in = y + r + 1, out = y - r;
                if (in < h) { s += tmp[in * w + x]; n++; }
                if (out >= 0) { s -= tmp[out * w + x]; n--; }
            }
        }
    }
    free(tmp);
}

void foil_free(FoilSurface *s) {
    free(s->height); free(s->mat); free(s->cov); free(s->brush); free(s->color);
    free(s->nx); free(s->ny); free(s->nz);
    s->height = NULL; s->mat = s->cov = s->brush = NULL; s->color = NULL; s->nx = s->ny = s->nz = NULL;
    s->built_s = 0;
}

bool foil_begin(FoilSurface *s, float x, float y, float w, float h, float r, Rgb tint) {
    if (s->built_s == ui_s && s->x == x && s->y == y && s->w == w && s->h == h) return false;
    foil_free(s);
    s->x = x; s->y = y; s->w = w; s->h = h; s->r = r; s->tint = tint;
    s->sw = (int)(w * ui_s + .5f); s->sh = (int)(h * ui_s + .5f);
    size_t n = (size_t)s->sw * s->sh;
    s->height = calloc(n, sizeof(float));
    s->mat = calloc(n, 1); s->cov = calloc(n, 1); s->brush = malloc(n);
    s->color = calloc(n, 4);
    if (!s->height || !s->mat || !s->cov || !s->brush || !s->color) { foil_free(s); return false; }
    s->built_s = ui_s;
    /* Brushed steel: horizontal streaks that drift slowly along each row. */
    int run = (int)(37 * ui_s + 1);
    for (int py = 0; py < s->sh; py++) {
        float row = hashf((uint32_t)py * 7919u);
        for (int px = 0; px < s->sw; px++) {
            float drift = hashf((uint32_t)(py * 31 + px / run) * 104729u);
            float fine = hashf((uint32_t)(py * 8191 + px) * 2654435761u);
            s->brush[py * s->sw + px] = (uint8_t)((row * .55f + drift * .25f + fine * .2f) * 255);
        }
    }
    /* Coverage and a rounded bevel at the edge. */
    for (int py = 0; py < s->sh; py++)
        for (int px = 0; px < s->sw; px++) {
            float ux = (px + .5f) / ui_s, uy = (py + .5f) / ui_s;
            float d = ui_rrect_dist(ux, uy, 0, 0, w, h, r);
            s->cov[py * s->sw + px] = (uint8_t)(clampf(.5f - d * ui_s, 0, 1) * 255);
            s->height[py * s->sw + px] = smooth(0, 7, -d) * .9f;
        }
    return true;
}

/* A line's profile at signed distance d from it (layout units), raising foil. */
static void raise_line(FoilSurface *s, int i, float d, float half, float lift) {
    float k = 1 - smooth(half * .5f, half + 1.2f, fabsf(d));
    s->height[i] += k * lift;
    if (k > .45f) s->mat[i] = FOIL_HOLO;
}

void foil_frame(FoilSurface *s, float inset, float r) {
    for (int py = 0; py < s->sh; py++)
        for (int px = 0; px < s->sw; px++) {
            float ux = (px + .5f) / ui_s, uy = (py + .5f) / ui_s;
            int i = py * s->sw + px;
            float d1 = ui_rrect_dist(ux, uy, inset, inset, s->w - 2 * inset, s->h - 2 * inset, r);
            float d2 = ui_rrect_dist(ux, uy, inset + 7, inset + 7, s->w - 2 * inset - 14, s->h - 2 * inset - 14, r > 3 ? r - 3 : r);
            raise_line(s, i, d1, 1.2f, .55f);
            raise_line(s, i, d2, .7f, .3f);
            /* Diamond studs at the corners of the outer line. */
            float cx[4] = { inset, s->w - inset, inset, s->w - inset }, cy[4] = { inset, inset, s->h - inset, s->h - inset };
            for (int k = 0; k < 4; k++) {
                float dd = fabsf(ux - cx[k]) + fabsf(uy - cy[k]);
                if (dd < 7) { s->height[i] += (7 - dd) / 7 * .9f; s->mat[i] = FOIL_HOLO; }
            }
        }
}

void foil_rule(FoilSurface *s, float x0, float x1, float y, float thick) {
    x0 -= s->x; x1 -= s->x; y -= s->y;
    for (int py = (int)((y - thick - 3) * ui_s); py <= (int)((y + thick + 3) * ui_s); py++)
        for (int px = (int)(x0 * ui_s); px < (int)(x1 * ui_s); px++) {
            if (px < 0 || py < 0 || px >= s->sw || py >= s->sh) continue;
            float ux = (px + .5f) / ui_s, uy = (py + .5f) / ui_s;
            float fade = smooth(0, 30, ux - x0) * smooth(0, 30, x1 - ux);   /* tapering ends */
            raise_line(s, py * s->sw + px, uy - y, thick * fade, .45f * fade);
        }
}

void foil_recess(FoilSurface *s, float x, float y, float w, float h, float r, float depth) {
    x -= s->x; y -= s->y;
    for (int py = 0; py < s->sh; py++)
        for (int px = 0; px < s->sw; px++) {
            float ux = (px + .5f) / ui_s, uy = (py + .5f) / ui_s;
            float d = ui_rrect_dist(ux, uy, x, y, w, h, r);
            if (d > 3) continue;
            int i = py * s->sw + px;
            s->height[i] -= smooth(3, -3, d) * depth;
            if (d < -1.5f && s->mat[i] == FOIL_STEEL) s->mat[i] = FOIL_DARK;
        }
}

void foil_ring(FoilSurface *s, float cx, float cy, float r, float thick) {
    cx -= s->x; cy -= s->y;
    for (int py = (int)((cy - r - thick - 3) * ui_s); py <= (int)((cy + r + thick + 3) * ui_s); py++)
        for (int px = (int)((cx - r - thick - 3) * ui_s); px <= (int)((cx + r + thick + 3) * ui_s); px++) {
            if (px < 0 || py < 0 || px >= s->sw || py >= s->sh) continue;
            float ux = (px + .5f) / ui_s, uy = (py + .5f) / ui_s;
            float d = sqrtf((ux - cx) * (ux - cx) + (uy - cy) * (uy - cy)) - r;
            int i = py * s->sw + px;
            raise_line(s, i, d, thick, .8f);
            if (d < -thick) { s->height[i] -= .5f; s->mat[i] = FOIL_DARK; }   /* the socket */
        }
}

float foil_art(FoilSurface *s, const Art *a, float cx, float top, float width, int material, float lift, float detail) {
    if (!a || !a->w) return top;
    cx -= s->x; top -= s->y;
    float k = width / a->w, h = a->h * k;
    float *m = calloc((size_t)s->sw * s->sh, sizeof(float));
    if (!m) return top + s->y + h;
    int x0 = (int)((cx - width / 2) * ui_s), y0 = (int)(top * ui_s), pw = (int)(width * ui_s), ph = (int)(h * ui_s);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int X = x0 + x, Y = y0 + y;
            if (X < 0 || Y < 0 || X >= s->sw || Y >= s->sh) continue;
            /* Bilinear, weighting colours by the mask so edges don't pick up the background. */
            float fx = (x + .5f) * a->w / pw - .5f, fy = (y + .5f) * a->h / ph - .5f;
            int ix = (int)floorf(fx), iy = (int)floorf(fy);
            float tx = fx - ix, ty = fy - iy, cov = 0;
            Rgb cc = rgb(0, 0, 0);
            for (int q = 0; q < 4; q++) {
                int sx = ix + (q & 1), sy = iy + (q >> 1);
                if (sx < 0 || sy < 0 || sx >= a->w || sy >= a->h) continue;
                float wgt = ((q & 1) ? tx : 1 - tx) * ((q >> 1) ? ty : 1 - ty) * a->mask[sy * a->w + sx] / 255.f;
                cc = add(cc, unpack(a->rgb[sy * a->w + sx]), wgt);
                cov += wgt;
            }
            if (cov < .5f) continue;
            cc = scale(cc, 1 / cov);
            int i = Y * s->sw + X;
            m[i] = 1;
            s->mat[i] = (uint8_t)material;
            s->color[i] = pack(cc);
            s->height[i] += (cc.r * .3f + cc.g * .5f + cc.b * .2f) * detail;
        }
    blur(m, s->sw, s->sh, (int)(1.5f * ui_s + .5f));
    for (size_t i = 0; i < (size_t)s->sw * s->sh; i++) s->height[i] += m[i] * lift;
    free(m);
    return s->y + top + h;
}

void foil_text(FoilSurface *s, const char *text, const char *face, float px, int weight, float tracking,
               float cx, float cy, float depth, int material) {
    const TextMask *t = ui_text_get(text, face, px, weight, tracking);
    if (!t || !t->a) return;
    int x0 = (int)((cx - s->x) * ui_s) - t->w / 2, y0 = (int)((cy - s->y) * ui_s) - t->h / 2;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            int X = x0 + x, Y = y0 + y;
            if (X < 0 || Y < 0 || X >= s->sw || Y >= s->sh) continue;
            float a = t->a[y * t->w + x] / 255.f;
            int i = Y * s->sw + X;
            s->height[i] -= a * depth;
            if (a > .5f) { s->mat[i] = (uint8_t)material; s->color[i] = pack(rgb(.85f, .62f, .25f)); }
        }
}

void foil_finish(FoilSurface *s) {
    size_t n = (size_t)s->sw * s->sh;
    free(s->nx); free(s->ny); free(s->nz);
    s->nx = malloc(n * sizeof(float)); s->ny = malloc(n * sizeof(float)); s->nz = malloc(n * sizeof(float));
    if (!s->nx || !s->ny || !s->nz) { foil_free(s); return; }
    float bump = 2.2f * ui_s;
    for (int y = 0; y < s->sh; y++)
        for (int x = 0; x < s->sw; x++) {
            int i = y * s->sw + x;
            float hl = s->height[i - (x > 0)], hr = s->height[i + (x < s->sw - 1)];
            float hu = s->height[i - (y > 0) * s->sw], hd = s->height[i + (y < s->sh - 1) * s->sw];
            float nx = (hl - hr) * bump, ny = (hu - hd) * bump, nn = sqrtf(nx * nx + ny * ny + 1);
            s->nx[i] = nx / nn; s->ny[i] = ny / nn; s->nz[i] = 1 / nn;
        }
}

/* ---- Shading ---------------------------------------------------------------------------- */
typedef struct { const FoilSurface *s; float lx, ly, t; } ShadeJob;

static inline float gauss(float x2k) {  /* ~exp(-x2k), cheap */
    float q = 1 - x2k * .25f;
    if (q <= 0) return 0;
    q *= q;
    return q * q;
}

static void shade_rows(void *arg, int y0, int y1) {
    const ShadeJob *j = arg;
    const FoilSurface *s = j->s;
    float lx = j->lx, ly = j->ly, t = j->t;
    int ox = (int)ui_x(s->x), oy = (int)ui_y(s->y);
    float ln = sqrtf(lx * lx + ly * ly + 1);
    float Lx = lx / ln, Ly = ly / ln, Lz = 1 / ln;
    float hx = Lx, hy = Ly, hz = Lz + 1, hn = sqrtf(hx * hx + hy * hy + hz * hz);
    hx /= hn; hy /= hn; hz /= hn;
    float cell = 5 * ui_s;
    /* Positions across the whole canvas, so neighbouring surfaces share one light. */
    float gu0 = s->x / UI_BASE_W, gv0 = s->y / UI_BASE_H, gus = s->w / UI_BASE_W / s->sw, gvs = s->h / UI_BASE_H / s->sh;
    float glint_at = .7f + .6f * lx + .3f * ly;
    for (int y = y0; y < y1; y++) {
        int Y = oy + y;
        if (Y < 0 || Y >= ui_h) continue;
        float v = gv0 + y * gvs;
        float bd = (v - .5f - ly * .42f) * 3.6f;
        float band = gauss(bd * bd);
        uint32_t *row = ui_px + (size_t)Y * ui_w;
        int gy = (int)(y / cell), cell_x = -1;
        uint32_t hcell = 0;
        float hp = 1;
        for (int x = 0; x < s->sw; x++) {
            int X = ox + x;
            if (X < 0 || X >= ui_w) continue;
            int i = y * s->sw + x;
            uint8_t cov = s->cov[i];
            if (!cov) continue;
            float nx = s->nx[i], ny = s->ny[i], nz = s->nz[i];
            float ndl = fmaxf(0, nx * Lx + ny * Ly + nz * Lz);
            uint8_t m = s->mat[i];
            if (m == FOIL_DARK) {               /* recessed: dark steel, the band muted, no glints */
                float br = s->brush[i] / 255.f;
                Rgb c = scale(s->tint, (.062f + .03f * br) * (.6f + .5f * ndl) * .55f + band * (.016f + .033f * br));
                row[X] = cov == 255 ? pack(c) : pack(mix(unpack(row[X]), c, cov / 255.f));
                continue;
            }
            float u = gu0 + x * gus;
            float ndh = fmaxf(0, nx * hx + ny * hy + nz * hz);
            float p2 = ndh * ndh, p4 = p2 * p2, p8 = p4 * p4, p16 = p8 * p8;
            float spec = p16 * p16 * p16;   /* ndh^48 */
            float br = s->brush[i] / 255.f;

            /* Brushed steel with an anisotropic highlight band that follows the light. */
            float base = .062f + .03f * br;
            Rgb c = scale(s->tint, base * (.6f + .5f * ndl) + band * (.03f + .06f * br) + spec * .45f);
            float gd = (u * .8f + v * .6f) - glint_at;      /* a diagonal glint sweeping across */
            float glint = gauss(gd * gd * 90);

            if (m == FOIL_HOLO) {
                Rgb holo = mix(rgb(1, 1, 1), hue(u * 1.6f + v * 1.1f + lx * .55f + ly * .35f + (nx + ny) * 1.4f + t * .02f), .78f);
                float k = .16f + .75f * p4;
                c = add(scale(c, .6f), holo, k * .8f);
                c = add(c, rgb(1, 1, 1), glint * .5f + spec * .45f);
            } else if (m == FOIL_GOLD) {
                Rgb a = unpack(s->color[i]);
                float lum = a.r * .3f + a.g * .55f + a.b * .15f;
                Rgb gold = mix(rgb_hex(0x3A2408), rgb_hex(0xF7DC92), smooth(.05f, .8f, lum));
                c = scale(gold, .45f + .6f * ndl);
                Rgb holo = mix(rgb(1, 1, 1), hue(u * 1.6f + v * 1.1f + lx * .55f + ly * .35f + (nx + ny) * 1.2f), .55f);
                c = add(c, holo, p2 * ndh * .32f * lum);
                c = add(c, rgb(1, .96f, .85f), spec * .7f + glint * .5f);
            } else if (m == FOIL_ART) {
                Rgb a = unpack(s->color[i]);
                float lum = a.r * .3f + a.g * .55f + a.b * .15f;
                bool goldish = a.r > a.b + .15f && a.g > a.b + .1f;      /* the Genesis lime-yellow, warmed to gold */
                if (goldish) a = mix(a, mix(rgb_hex(0x4A2E0A), rgb_hex(0xFFE3A0), lum), .45f);
                c = scale(a, .42f + .38f * ndl);
                c = add(c, rgb(1, .88f, .58f), p16 * p8 * p4 * p2 * .45f);
                c = add(c, hue(u * 1.2f + v * .9f + lx * .5f + ly * .4f), p4 * .10f);
                c = add(c, rgb(1, .95f, .85f), glint * .28f);
            } else {
                c = add(c, rgb(.85f, .9f, 1), glint * .08f);
            }

            /* Sparkles: sparse points that flash at their own angles. */
            {
                int gx = (int)(x / cell);
                if (gx != cell_x) {
                    cell_x = gx;
                    hcell = (uint32_t)(gx * 73856093) ^ (uint32_t)(gy * 19349663) ^ (uint32_t)(s->x * 977 + s->y * 131);
                    hp = hashf(hcell);
                }
                if (hp < (m == FOIL_STEEL ? .015f : m == FOIL_ART ? .03f : .08f)) {
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
            }
            row[X] = cov == 255 ? pack(c) : pack(mix(unpack(row[X]), c, cov / 255.f));
        }
    }
}

void foil_draw(const FoilSurface *s, float lx, float ly, float t) {
    if (!s->built_s || !s->nx) return;
    ShadeJob j = { s, lx, ly, t };
    hd_rows(shade_rows, &j, s->sh);
}

void foil_light(float t, float mouse_x, float mouse_y, float mouse_t, float *lx, float *ly) {
    float ax = .55f * sinf(t * .37f) + .12f * sinf(t * 1.13f), ay = .35f * cosf(t * .29f) + .08f * cosf(t * .91f);
    float w = mouse_t < 0 ? 0 : 1 - smooth(2.5f, 5.f, t - mouse_t);
    float mx = clampf((mouse_x - 640) / 640, -1, 1) * .9f, my = clampf((mouse_y - 360) / 360, -1, 1) * .6f;
    *lx = ax + (mx - ax) * w * .85f;
    *ly = ay + (my - ay) * w * .85f;
}

/* ---- The night -------------------------------------------------------------------------- */
static uint32_t *night;                 /* the backdrop at canvas size */
static int night_w, night_h;
static float *fog;                      /* tileable value noise, FOG_N x FOG_N */
#define FOG_N 128

static float noise_at(const float *grid, int n, float x, float y) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float tx = x - ix, ty = y - iy;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
    int x0 = ((ix % n) + n) % n, y0 = ((iy % n) + n) % n, x1 = (x0 + 1) % n, y1 = (y0 + 1) % n;
    float a = grid[y0 * n + x0], b = grid[y0 * n + x1], c = grid[y1 * n + x0], d = grid[y1 * n + x1];
    return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
}

static void make_fog(void) {
    if (fog) return;
    fog = malloc(sizeof(float) * FOG_N * FOG_N);
    float g[3][32 * 32];
    for (int o = 0; o < 3; o++) for (int i = 0; i < 32 * 32; i++) g[o][i] = hashf((uint32_t)(i * 97 + o * 7919 + 13));
    for (int y = 0; y < FOG_N; y++)
        for (int x = 0; x < FOG_N; x++) {
            float v = 0, amp = .55f;
            for (int o = 0; o < 3; o++, amp *= .5f) {
                int n = 4 << o;             /* cells across the tile in this octave: it wraps */
                v += amp * noise_at(g[o], n, x * n / (float)FOG_N, y * n / (float)FOG_N);
            }
            fog[y * FOG_N + x] = v;
        }
}

static void build_night(void) {
    free(night);
    night = malloc((size_t)ui_w * ui_h * 4);
    night_w = ui_w; night_h = ui_h;
    if (!night) return;
    for (int y = 0; y < ui_h; y++)
        for (int x = 0; x < ui_w; x++) {
            float ux = (x - ui_ox) / ui_s, uy = (y - ui_oy) / ui_s;
            float v = clampf(uy / UI_BASE_H, -.2f, 1.2f);
            Rgb c = mix(rgb_hex(0x0D0A18), rgb_hex(0x050408), smooth(0, 1, v));
            /* A pale moon's halo high on the right, a warm glow low behind the case. */
            float mr = sqrtf((ux - 1080) * (ux - 1080) + (uy - 40) * (uy - 40));
            c = add(c, rgb(.30f, .30f, .45f), .10f * expf(-mr * mr / (2 * 260.f * 260.f)));
            float g = sqrtf((ux - 290) * (ux - 290) + (uy - 560) * (uy - 560));
            c = add(c, rgb(.55f, .26f, .08f), .16f * expf(-g * g / (2 * 300.f * 300.f)));
            /* Dune silhouettes along the bottom, two layers. */
            float d1 = 560 + 24 * sinf(ux * .006f + 1.3f) + 12 * sinf(ux * .017f);
            float d2 = 610 + 18 * sinf(ux * .009f + 4.1f) + 8 * sinf(ux * .023f + 2);
            if (uy > d1) c = mix(c, rgb_hex(0x0A0710), .7f * smooth(d1, d1 + 6, uy));
            if (uy > d2) c = mix(c, rgb_hex(0x040306), .85f * smooth(d2, d2 + 4, uy));
            /* Stars, fewer towards the horizon. */
            uint32_t hs = hash32((uint32_t)(x / 2) * 73856093u ^ (uint32_t)(y / 2) * 19349663u);
            if ((hs & 1023) < 3 && uy < 520) c = add(c, rgb(.8f, .82f, 1), .25f + .5f * hashf(hs) * (1 - v));
            /* Vignette and a little dither against banding. */
            float ex = (ux - 640) / 700, ey = (uy - 360) / 420;
            c = scale(c, 1 - .5f * smooth(.4f, 1.3f, sqrtf(ex * ex + ey * ey)));
            c = add(c, rgb(1, 1, 1), (hashf((uint32_t)(y * ui_w + x)) - .5f) / 255.f);
            night[(size_t)y * ui_w + x] = pack(c);
        }
}

bool foil_backdrop(float t) {
    bool fresh = !night || night_w != ui_w || night_h != ui_h;
    if (fresh) build_night();
    if (night) memcpy(ui_px, night, (size_t)ui_w * ui_h * 4);
    /* A few stars twinkle. */
    for (int i = 0; i < 40; i++) {
        float x = hashf((uint32_t)i * 977u) * UI_BASE_W, y = hashf((uint32_t)i * 613u) * 470;
        float tw = sinf(t * (1.3f + hashf((uint32_t)i * 41u) * 2) + i);
        if (tw > .6f) scene_glow(x, y, 2.2f, rgb(.85f, .88f, 1), (tw - .6f) * .9f);
    }
    return fresh;
}

static void shadow(uint32_t *buf, uint32_t *also, float x, float y, float w, float h, float r) {
    float spread = 26;
    int x0 = (int)ui_x(x - spread), y0 = (int)ui_y(y - spread), x1 = (int)ui_x(x + w + spread), y1 = (int)ui_y(y + h + spread);
    for (int py = y0 < 0 ? 0 : y0; py < y1 && py < ui_h; py++)
        for (int px = x0 < 0 ? 0 : x0; px < x1 && px < ui_w; px++) {
            float ux = (px + .5f - ui_ox) / ui_s, uy = (py + .5f - ui_oy) / ui_s;
            float d = ui_rrect_dist(ux, uy, x, y, w, h, r);
            float k = .62f * (1 - smooth(-10, spread, d));
            if (k <= 0) continue;
            size_t i = (size_t)py * ui_w + px;
            buf[i] = pack(scale(unpack(buf[i]), 1 - k));
            if (also) also[i] = buf[i];
        }
}

void foil_shadow(float x, float y, float w, float h, float r) { if (night) shadow(night, ui_px, x, y, w, h, r); }
void foil_shadow_into(uint32_t *buf, float x, float y, float w, float h, float r) { if (buf) shadow(buf, NULL, x, y, w, h, r); }

/* Mist: two layers of the noise sliding sideways at different speeds, thicker low down. */
typedef struct { float t, strength; } MistJob;

static void mist_rows(void *arg, int y0, int y1) {
    const MistJob *j = arg;
    Rgb tint = rgb(.42f, .34f, .52f), warm = rgb(.55f, .36f, .22f);
    float per = FOG_N / (420.f * ui_s), per2 = FOG_N / (260.f * ui_s);
    for (int y = y0; y < y1; y++) {
        float uy = (y - ui_oy) / ui_s;
        float low = smooth(260, 700, uy);
        if (low * j->strength < .02f) continue;
        uint32_t *row = ui_px + (size_t)y * ui_w;
        float fy = y * per + j->t * .4f, fy2 = y * per2 - j->t * .25f;
        for (int x = 0; x < ui_w; x += 2) {     /* the mist is soft: one sample for two pixels */
            float a = noise_at(fog, FOG_N, x * per + j->t * 2.2f, fy);
            float b = noise_at(fog, FOG_N, x * per2 - j->t * 3.4f + 40, fy2);
            float m = smooth(.38f, .85f, a * .6f + b * .55f) * low * j->strength;
            if (m < .004f) continue;
            Rgb mist = mix(tint, warm, b * .6f);
            for (int k = x; k < x + 2 && k < ui_w; k++) row[k] = pack(add(scale(unpack(row[k]), 1 - m * .25f), mist, m * .22f));
        }
    }
}

void foil_atmosphere(float t, float strength) {
    make_fog();
    if (!fog) return;
    MistJob j = { t, strength };
    hd_rows(mist_rows, &j, ui_h);
    /* Embers rising and swaying, flickering as they go. */
    for (int i = 0; i < 46; i++) {
        float life = 7 + 5 * hashf((uint32_t)i * 29u);
        float age = fmodf(t + hashf((uint32_t)i * 31u) * life, life) / life;
        float x = hashf((uint32_t)i * 37u) * UI_BASE_W + sinf(t * .8f + i * 1.7f) * 18 * age;
        float y = 750 - age * 800;
        float flick = .7f + .3f * sinf(t * 9 + i * 3.1f);
        float k = sinf(age * 3.14159f) * (.16f + .26f * hashf((uint32_t)i * 41u)) * flick * strength;
        scene_glow(x, y, 2.6f + 1.4f * hashf((uint32_t)i * 43u), rgb(1, .58f, .22f), k);
    }
}
