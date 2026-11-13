/* Dynamic light. Once per game frame, from the captured layers (hd_frame):
 *  - the light sources: the bright, vivid pixels of the spirits and their magic (the
 *    sprites Spirit effects gives particles to: not Ali, people, enemies, items or the HUD),
 *    and fire-coloured scenery (torches, braziers, lava);
 *  - their light, gathered into cells of 4x4 game pixels and spread out at two sizes, a
 *    tight pool and a wide one, so it falls off softly over the ground and walls;
 *  - how dark the scene is, from the scenery's mean brightness: dark places (caves,
 *    dungeons, nights indoors) dim a little, bright ones not at all.
 * Every picture is then multiplied by its game pixel's light, so lit ground keeps its own
 * colours and texture, warmed or tinted by the light. The HUD and the lights themselves
 * are never dimmed. Colours are read as the console makes them, so the colour tone and
 * colour-blind help do not change what gives off light. */
#include "light.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "genesis.h"
#include "fx.h"
#include "motion.h"

#define CELL 4
#define GW (HD_MAX_W / CELL + 1)
#define GH (HD_MAX_H / CELL + 1)

static float src_grid[GH][GW][3], tight[GH][GW][3], wide[GH][GW][3], tmp[GH > GW ? GH : GW][3];
static uint8_t keep[HD_MAX_H][HD_MAX_W];        /* never dimmed: the HUD and the lights */
static uint16_t factor[HD_MAX_H][HD_MAX_W][3];  /* each game pixel's light, in 256ths */
static unsigned built_serial;
static int built_level = -1, built_w, built_h;

static inline float chan(uint32_t c, int sh) { return ((c >> sh) & 255) / 255.f; }

/* How much light a sprite pixel gives off: bright and vivid. */
static float sprite_glow(uint32_t c) {
    float r = chan(c, 16), g = chan(c, 8), b = chan(c, 0);
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
    float sat = mx > 0 ? (mx - mn) / mx : 0;
    float k = (mx - .55f) / .45f * (sat - .35f) / .45f;
    return k > 0 ? (k > 1 ? 1 : k) : 0;
}

/* How much light a scenery pixel gives off: only fire colours (red the brightest, then
 * green, little blue), so red roofs and yellow sand do not. */
static float fire_glow(uint32_t c) {
    float r = chan(c, 16), g = chan(c, 8), b = chan(c, 0);
    if (r < .72f || g < r * .32f || g > r * .92f || b > r * .45f) return 0;
    return (r - .72f) / .28f * .7f;
}

static void add_light(int x, int y, uint32_t c, float k) {
    float *o = src_grid[y / CELL][x / CELL];
    float r = chan(c, 16), g = chan(c, 8), b = chan(c, 0), mx = fmaxf(r, fmaxf(g, b));
    if (mx <= 0) return;
    /* The light's colour at full brightness: a dim pixel of a vivid colour still lights. */
    o[0] += r / mx * k; o[1] += g / mx * k; o[2] += b / mx * k;
}

/* One box blur of radius r along n values `stride` floats apart, edges held: a running sum. */
static void box(float *v, int n, int stride, int r) {
    float inv = 1.f / (2 * r + 1);
    for (int c = 0; c < 3; c++) {
        float s = 0;
        for (int k = -r; k <= r; k++) s += v[(k < 0 ? 0 : k >= n ? n - 1 : k) * stride + c];
        for (int i = 0; i < n; i++) {
            tmp[i][c] = s * inv;
            int in = i + r + 1, out = i - r;
            s += v[(in >= n ? n - 1 : in) * stride + c] - v[(out < 0 ? 0 : out) * stride + c];
        }
    }
    for (int i = 0; i < n; i++) for (int c = 0; c < 3; c++) v[i * stride + c] = tmp[i][c];
}

/* Box blur of radius r over a gw x gh grid, twice each way (close to a gaussian). */
static void blur(float (*g)[GW][3], int gw, int gh, int r) {
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < gh; y++) box(g[y][0], gw, 3, r);
        for (int x = 0; x < gw; x++) box(g[0][x], gh, GW * 3, r);
    }
}

/* The scenery pixel on top at (x, y): plane A or B by priority (sprites left out). */
static uint32_t scenery(const HdFrame *f, int x, int y, bool *window) {
    uint32_t a = f->a[y][x], b = f->b[y][x];
    bool ao = (a >> 16) & 0x0F, ah = (a >> 16) & 0x40, bo = (b >> 16) & 0x0F, bh = (b >> 16) & 0x40;
    *window = (a >> 16) & HD_WINDOW;
    if (ao && (ah || !(bo && bh))) return vdp_color_raw((uint16_t)a);
    if (bo) return vdp_color_raw((uint16_t)b);
    return vdp_color_raw(f->backdrop[y]);
}

static float grid_k[GH][GW][3];         /* each cell's light: the factor at its centre */
static int fact_w, fact_h, fact_gw, fact_gh;

/* Where a coordinate falls between two cell centres, and how far towards the second. */
static void between(int v, int n, int *i0, int *i1, float *t) {
    float f = (v + .5f) / CELL - .5f;
    int a = (int)floorf(f);
    *t = f - a;
    *i0 = a < 0 ? 0 : a >= n ? n - 1 : a;
    *i1 = a + 1 < 0 ? 0 : a + 1 >= n ? n - 1 : a + 1;
}

/* Each game pixel's light: down the columns of cells, then along the row. */
static void fact_rows(void) {
    int w = fact_w, gw = fact_gw, gh = fact_gh;
    static int x0s[HD_MAX_W], x1s[HD_MAX_W];
    static float txs[HD_MAX_W], row[GW][3];
    for (int x = 0; x < w; x++) between(x, gw, &x0s[x], &x1s[x], &txs[x]);
    for (int y = 0; y < fact_h; y++) {
        int y0, y1;
        float ty;
        between(y, gh, &y0, &y1, &ty);
        for (int x = 0; x < gw; x++)
            for (int c = 0; c < 3; c++) row[x][c] = grid_k[y0][x][c] + (grid_k[y1][x][c] - grid_k[y0][x][c]) * ty;
        for (int x = 0; x < w; x++) {
            const float *a = row[x0s[x]], *b = row[x1s[x]];
            float tx = txs[x];
            for (int c = 0; c < 3; c++) {
                float k = a[c] + (b[c] - a[c]) * tx;
                if (keep[y][x] && k < 1) k = 1;
                factor[y][x][c] = (uint16_t)(k * 256 + .5f);
            }
        }
    }
}

static void build(const HdFrame *f, int level) {
    int w = f->w, h = f->h, gw = (w + CELL - 1) / CELL, gh = (h + CELL - 1) / CELL;
    memset(src_grid, 0, sizeof src_grid);
    memset(keep, 0, sizeof keep);
    /* The scenery: fire, and how bright the scene is. */
    double lum = 0;
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool window;
            uint32_t c = scenery(f, x, y, &window);
            if (window) { keep[y][x] = 1; continue; }
            float k = fire_glow(c);
            if (k > 0) { add_light(x, y, c, k); keep[y][x] = 1; }
            if (((x | y) & 1) == 0) { lum += chan(c, 16) * .3f + chan(c, 8) * .55f + chan(c, 0) * .15f; n++; }
        }
    /* The sprites: those of no object, or with priority, are the HUD's and are never dimmed;
     * the spirits' and their magic's give off light. */
    for (int i = 0; i < 80; i++) {
        const HdSprite *s = &f->sprites[i];
        uint32_t obj;
        uint16_t kind;
        if (!s->drawn) continue;
        bool hud = (s->attr & 0x8000) || !motion_entry_owner(i, &obj, &kind);
        bool lights = fx_elemental(f, i);
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++) {
                uint32_t v = s->px[y * 32 + x];
                int gx = s->x + f->extra + x, gy = s->y + y;
                if (!((v >> 16) & 0x0F) || gx < 0 || gy < 0 || gx >= w || gy >= h) continue;
                if (hud) { keep[gy][gx] = 1; continue; }
                if (!lights) continue;
                uint32_t c = vdp_color_raw((uint16_t)v);
                float k = sprite_glow(c);
                if (k > 0) { add_light(gx, gy, c, k); keep[gy][gx] = 1; }
            }
    }
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
            for (int c = 0; c < 3; c++) src_grid[y][x][c] /= CELL * CELL;
    memcpy(tight, src_grid, sizeof tight);
    memcpy(wide, src_grid, sizeof wide);
    blur(tight, gw, gh, 2);
    blur(wide, gw, gh, 7);
    /* Dark places dim: from a mean brightness of .42 (no dimming) down to .2 (the most). */
    float mean = n ? (float)(lum / n) : 1;
    float dark = (.42f - mean) / .22f;
    dark = dark < 0 ? 0 : dark > 1 ? 1 : dark;
    float ambient = 1 - dark * (level >= 2 ? .38f : .24f);
    float gain = level >= 2 ? 9.f : 6.f;
    /* Each cell's light; light saturates softly, so a crowd of lights does not burn the
     * picture out. Each game pixel then takes the cells' light bilinearly between their
     * centres (fact_rows). */
    float most = level >= 2 ? .85f : .55f;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++)
            for (int c = 0; c < 3; c++)
                grid_k[y][x][c] = ambient + (1 - expf(-(tight[y][x][c] * .5f + wide[y][x][c]) * gain)) * most;
    fact_w = w; fact_h = h; fact_gw = gw; fact_gh = gh;
    fact_rows();
}

typedef struct { const uint32_t *src; uint32_t *dst; int w, pitch, scale, gw, gh; } Job;

static void rows(void *ctx, int y0, int y1) {
    const Job *j = ctx;
    for (int y = y0; y < y1; y++) {
        int gy = y / j->scale;
        if (gy >= j->gh) gy = j->gh - 1;
        const uint32_t *s = j->src + (size_t)y * j->pitch;
        uint32_t *o = j->dst + (size_t)y * j->w;
        const uint16_t (*f)[3] = factor[gy];
        for (int gx = 0, x = 0; gx < j->gw; gx++) {
            uint32_t kr = f[gx][0], kg = f[gx][1], kb = f[gx][2];
            for (int e = x + j->scale; x < e; x++) {
                uint32_t c = s[x];
                uint32_t r = (((c >> 16) & 255) * kr) >> 8, g = (((c >> 8) & 255) * kg) >> 8, b = ((c & 255) * kb) >> 8;
                o[x] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
        }
    }
}

uint32_t *light_apply(const uint32_t *src, int w, int h, int pitch, const HdPost *p) {
    static uint32_t *buf;
    static size_t cap;
    const HdFrame *f = &hd_frame;
    int scale = p->scale < 1 ? 1 : p->scale;
    if (!p->dynlight || !p->world || f->w <= 0 || f->h <= 0 || w != f->w * scale || h != f->h * scale) return NULL;
    if (built_serial != hd_serial || built_level != p->dynlight || built_w != f->w || built_h != f->h) {
        build(f, p->dynlight);
        built_serial = hd_serial; built_level = p->dynlight; built_w = f->w; built_h = f->h;
    }
    size_t need = (size_t)w * h;
    if (need > cap) { free(buf); buf = malloc(need * 4); cap = buf ? need : 0; }
    if (!buf) return NULL;
    Job j = { src, buf, w, pitch, scale, f->w, f->h };
    hd_rows(rows, &j, h);
    return buf;
}
