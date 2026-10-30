/* High internal resolution: each layer of the frame (plane A with the window, plane B,
 * sprites) is scaled on its own with Scale2x / Scale3x (4x = 2x twice), then the layers
 * are composited at that size with vdp_compose(), like the VDP does at 1x.
 *
 * Scale2x/3x pick, for every output pixel, one whole source pixel of the 3x3
 * neighbourhood, so colours, priorities and transparency are never blended: edges get
 * rounded and diagonals smoothed, and layers stay separate. Mid-frame palette changes
 * and line scrolls are kept because every line was captured with its own colours. */
#include "hd.h"
#include "motion.h"
#include "fx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

static inline uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

HdFrame hd_frame;
uint32_t *hd_px;
int hd_w, hd_h;
unsigned hd_serial = 1;
int hd_owned;
double hd_ms[4];

static uint32_t *scaled[3], *temp[3];
static size_t scaled_cap;

/* Source pixel with the edges clamped. */
#define SRC(x, y) src[(size_t)((y) < 0 ? 0 : (y) >= h ? h - 1 : (y)) * pitch + ((x) < 0 ? 0 : (x) >= w ? w - 1 : (x))]

static void scale2x(const uint32_t *src, int pitch, int w, int h, uint32_t *dst) {
    int dw = w * 2;
    for (int y = 0; y < h; y++) {
        uint32_t *d0 = dst + (size_t)(y * 2) * dw, *d1 = d0 + dw;
        for (int x = 0; x < w; x++) {
            uint32_t B = SRC(x, y - 1), D = SRC(x - 1, y), E = src[(size_t)y * pitch + x], F = SRC(x + 1, y), H = SRC(x, y + 1);
            if (B != H && D != F) {
                d0[x * 2]     = D == B ? D : E;
                d0[x * 2 + 1] = B == F ? F : E;
                d1[x * 2]     = D == H ? D : E;
                d1[x * 2 + 1] = H == F ? F : E;
            } else {
                d0[x * 2] = d0[x * 2 + 1] = d1[x * 2] = d1[x * 2 + 1] = E;
            }
        }
    }
}

static void scale3x(const uint32_t *src, int pitch, int w, int h, uint32_t *dst) {
    int dw = w * 3;
    for (int y = 0; y < h; y++) {
        uint32_t *d0 = dst + (size_t)(y * 3) * dw, *d1 = d0 + dw, *d2 = d1 + dw;
        for (int x = 0; x < w; x++) {
            uint32_t A = SRC(x - 1, y - 1), B = SRC(x, y - 1), C = SRC(x + 1, y - 1);
            uint32_t D = SRC(x - 1, y), E = src[(size_t)y * pitch + x], F = SRC(x + 1, y);
            uint32_t G = SRC(x - 1, y + 1), H = SRC(x, y + 1), I = SRC(x + 1, y + 1);
            uint32_t *o0 = d0 + x * 3, *o1 = d1 + x * 3, *o2 = d2 + x * 3;
            if (B != H && D != F) {
                o0[0] = D == B ? D : E;
                o0[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                o0[2] = B == F ? F : E;
                o1[0] = (D == B && E != G) || (D == H && E != A) ? D : E;
                o1[1] = E;
                o1[2] = (B == F && E != I) || (H == F && E != C) ? F : E;
                o2[0] = D == H ? D : E;
                o2[1] = (D == H && E != I) || (H == F && E != G) ? H : E;
                o2[2] = H == F ? F : E;
            } else {
                o0[0] = o0[1] = o0[2] = o1[0] = o1[1] = o1[2] = o2[0] = o2[1] = o2[2] = E;
            }
        }
    }
}

/* ---- xBR rules without blending ------------------------------------------------------- *
 * Hyllian's xBR looks at a 5x5 neighbourhood to tell along which diagonal an edge runs,
 * weighing colour differences, and rounds each output corner on the right side of it;
 * that follows curves and shallow slopes better than Scale2x's 3x3 test. The usual
 * version blends colours at the edge; here the corner takes one whole neighbouring source
 * pixel instead, so the result still contains only real layer pixels (colour, priority,
 * transparency) and the layers composite exactly as before. */
int hd_scaler;

static inline int px_diff(uint32_t a, uint32_t b) {
    if (a == b) return 0;
    bool ta = (a >> 16) & 0x0F, tb = (b >> 16) & 0x0F;
    if (!ta || !tb) return ta == tb ? 0 : 48;              /* opaque against transparent: a strong edge */
    int dr = (int)((a >> 1) & 7) - (int)((b >> 1) & 7), dg = (int)((a >> 5) & 7) - (int)((b >> 5) & 7),
        db = (int)((a >> 9) & 7) - (int)((b >> 9) & 7);
    int y = 3 * dr + 6 * dg + db, u = dr * 4 - (3 * dr + 6 * dg + db) * 4 / 10, v = db * 4 - (3 * dr + 6 * dg + db) * 4 / 10;
    return abs(y) + (abs(u) + abs(v)) / 2 + (((a ^ b) >> 16) & 0x40 ? 4 : 0);
}
#define XEQ(a, b) (px_diff(a, b) < 3)

/* The corner of E towards (mx, my) (+1 or -1 each): 0 if it keeps E, 1 if it takes the
 * pixel returned in *pick, 2 if the edge is strong enough to take the cells next to it. */
static int xbr_corner(const uint32_t *src, int pitch, int w, int h, int x, int y, int mx, int my, uint32_t *pick) {
#define P(dx, dy) SRC(x + (dx) * mx, y + (dy) * my)
    uint32_t E = P(0, 0), B = P(0, -1), C = P(1, -1), D = P(-1, 0), F = P(1, 0), G = P(-1, 1), H = P(0, 1), I = P(1, 1);
    uint32_t F4 = P(2, 0), I4 = P(2, 1), H5 = P(0, 2), I5 = P(1, 2);
#undef P
    int wd1 = px_diff(E, C) + px_diff(E, G) + px_diff(I, F4) + px_diff(I, H5) + 4 * px_diff(H, F);
    int wd2 = px_diff(H, D) + px_diff(H, I5) + px_diff(F, I4) + px_diff(F, B) + 4 * px_diff(E, I);
    if (wd1 >= wd2) return 0;
    if (!((!XEQ(F, B) && !XEQ(H, D)) || (XEQ(E, I) && !XEQ(F, I4) && !XEQ(H, I5)) || XEQ(E, G) || XEQ(E, C))) return 0;
    *pick = px_diff(E, F) <= px_diff(E, H) ? F : H;
    return wd1 * 2 <= wd2 ? 2 : 1;
}

/* Rows y0..y1 of the n-times picture (n = 2, 3 or 4, in one pass). */
static void xbr_rows(const uint32_t *src, int pitch, int w, int h, int n, uint32_t *dst, int y0, int y1) {
    int dw = w * n;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < w; x++) {
            uint32_t E = src[(size_t)y * pitch + x];
            uint32_t *o = dst + (size_t)(y * n) * dw + x * n;
            for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) o[j * dw + i] = E;
            uint32_t L = SRC(x - 1, y), R = SRC(x + 1, y), U = SRC(x, y - 1), Dn = SRC(x, y + 1);
            for (int c = 0; c < 4; c++) {
                int mx = (c & 1) ? 1 : -1, my = (c & 2) ? 1 : -1;
                /* The corner can only take the side or the vertical neighbour towards it:
                 * when both are E there is nothing to decide (flat areas, straight edges). */
                if ((mx > 0 ? R : L) == E && (my > 0 ? Dn : U) == E) continue;
                uint32_t pick;
                int kind = xbr_corner(src, pitch, w, h, x, y, mx, my, &pick);
                if (!kind) continue;
                int cx = mx > 0 ? n - 1 : 0, cy = my > 0 ? n - 1 : 0, ix = mx > 0 ? -1 : 1, iy = my > 0 ? -1 : 1;
                o[cy * dw + cx] = pick;
                if (n >= 3 && kind == 2) { o[cy * dw + cx + ix] = pick; o[(cy + iy) * dw + cx] = pick; }
                if (n == 4) { o[cy * dw + cx + ix] = pick; o[(cy + iy) * dw + cx] = pick; }
            }
        }
}

static void xbr(const uint32_t *src, int pitch, int w, int h, int n, uint32_t *dst) { xbr_rows(src, pitch, w, h, n, dst, 0, h); }

/* Scales w x h pixels (row pitch `pitch`) into dst (w*scale wide, no padding). */
static void scale_any(const uint32_t *src, int pitch, int w, int h, int scale, uint32_t *dst, uint32_t *tmp) {
    if (scale == 1) for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, src + (size_t)y * pitch, (size_t)w * 4);
    else if (hd_scaler == 1) {
        (void)tmp;
        xbr(src, pitch, w, h, scale, dst);
    }
    else if (scale == 3) scale3x(src, pitch, w, h, dst);
    else if (scale == 4) { scale2x(src, pitch, w, h, tmp); scale2x(tmp, w * 2, w * 2, h * 2, dst); }
    else scale2x(src, pitch, w, h, dst);
}

/* ---- Motion ----------------------------------------------------------------------- *
 * The game moves things once per tick (every other frame: its logic runs at 30 Hz). The
 * pictures between two ticks show each layer where it was at that moment: the planes from
 * how their scroll changed, each sprite from how its own object moved (motion.c, read from
 * the game's data as it builds its sprite list). Motion is continuous at any refresh rate,
 * one tick (about 33 ms) late. */
typedef struct {
    int w, h;
    uint8_t flags[HD_MAX_H];
    int16_t hscroll[2][HD_MAX_H], vscroll[2][HD_MAX_H];
} Scroll;

/* When the latest update of a layer came, and how many frames apart updates come. */
typedef struct { long at; int period; } Timeline;

static Scroll before, latest;           /* plane scroll at the previous and the latest update */
static bool have_before, have_latest;
static long frame_no;                   /* frames run */
static Timeline planes = { 0, 2 }, sprites = { 0, 2 };
static unsigned shown_tick;             /* the game tick whose sprites hd_frame shows */
static int move_x[80], move_y[80];      /* how far each sprite's object moved in that tick */

static unsigned scaled_gen, sprites_gen;
static int scaled_scale, sprites_scale;
static uint32_t *sprite_layer;          /* sprites drawn at their in-between positions */

/* Each sprite of hd_frame scaled once per frame, with a transparent border of one pixel
 * so the scaler can round its outer edges. */
#define SPR_PAD_MAX ((32 + 2) * HD_MAX_SCALE)
static uint32_t sprite_scaled[80][SPR_PAD_MAX * SPR_PAD_MAX];
static uint32_t sprite_tmp[SPR_PAD_MAX * SPR_PAD_MAX];

static void scroll_of(Scroll *m, const HdFrame *f) {
    m->w = f->w; m->h = f->h;
    memcpy(m->flags, f->flags, sizeof m->flags);
    memcpy(m->hscroll, f->hscroll, sizeof m->hscroll);
    memcpy(m->vscroll, f->vscroll, sizeof m->vscroll);
}

static bool scrolled(const Scroll *a, const Scroll *b) {
    return a->w != b->w || a->h != b->h || memcmp(a->hscroll, b->hscroll, sizeof a->hscroll) ||
           memcmp(a->vscroll, b->vscroll, sizeof a->vscroll);
}

static void timeline_update(Timeline *tl, bool consecutive) {
    long gap = frame_no - tl->at;
    if (consecutive && gap >= 1 && gap <= 4) tl->period = (int)gap;
    tl->at = frame_no;
}

static float timeline_t(const Timeline *tl, float frac) {
    float t = (frame_no + frac - tl->at) / (float)tl->period;
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

void hd_frame_done(void) {
    static Scroll now;
    frame_no++;
    scroll_of(&now, &hd_frame);
    if (!have_latest) {
        latest = now; have_latest = true; planes.at = frame_no;
    } else if (scrolled(&now, &latest)) {
        long gap = frame_no - planes.at;
        timeline_update(&planes, true);
        before = latest;
        have_before = gap <= 8;         /* after a long still moment, start from here */
        latest = now;
    }
    unsigned tick = motion_sprite_moves(&hd_frame, move_x, move_y, &hd_owned);
    if (tick != shown_tick) {
        timeline_update(&sprites, tick && tick == shown_tick + 1);
        shown_tick = tick;
    }
}

static bool ensure(int scale) {
    int w = hd_frame.w, h = hd_frame.h;
    if (w <= 0 || h <= 0) return false;
    size_t need = (size_t)w * scale * h * scale;
    if (need > scaled_cap) {
        for (int k = 0; k < 3; k++) { free(scaled[k]); scaled[k] = malloc(need * 4); }
        for (int k = 0; k < 3; k++) { free(temp[k]); temp[k] = malloc(need * 4); }
        free(hd_px); hd_px = malloc(need * 4);
        free(sprite_layer); sprite_layer = malloc(need * 4);
        scaled_cap = need;
        scaled_gen = sprites_gen = 0;
    }
    hd_w = w * scale;
    hd_h = h * scale;
    return true;
}

/* ---- Threads: the work is split across cores by a pool of workers started once ----- *
 * (Starting threads for every picture, over 2000 a second at 144 Hz, made some
 * pictures stall for 10-25 ms.) */
typedef struct { int (*fn)(void *); void *arg; } Job;

#define MAX_WORKERS 24

static Job pool_job[MAX_WORKERS];
static SDL_sem *pool_go[MAX_WORKERS], *pool_done;
static int pool_size;                   /* workers running (jobs 1..pool_size) */

static int pool_worker(void *arg) {
    int k = (int)(intptr_t)arg;
    for (;;) {
        SDL_SemWait(pool_go[k]);
        pool_job[k].fn(pool_job[k].arg);
        SDL_SemPost(pool_done);
    }
    return 0;
}

static void run_jobs(Job *jobs, int n) {
    if (!pool_done) pool_done = SDL_CreateSemaphore(0);
    while (pool_size < n - 1 && pool_size < MAX_WORKERS - 1 && pool_done) {
        int k = pool_size + 1;
        pool_go[k] = SDL_CreateSemaphore(0);
        if (!pool_go[k] || !SDL_CreateThread(pool_worker, "hd", (void *)(intptr_t)k)) break;
        pool_size = k;
    }
    int sent = 0;
    for (int k = 1; k < n; k++) {
        if (k <= pool_size) { pool_job[k] = jobs[k]; SDL_SemPost(pool_go[k]); sent++; }
    }
    jobs[0].fn(jobs[0].arg);
    for (int k = pool_size + 1; k < n; k++) jobs[k].fn(jobs[k].arg);   /* no worker: do it here */
    for (int k = 0; k < sent; k++) SDL_SemWait(pool_done);
}

/* Rows in small bands that each thread takes as it becomes free: on a CPU with fast and
 * slow cores, equal shares would leave every picture waiting for the slow ones. */
typedef struct { void (*rows)(void *ctx, int y0, int y1); void *ctx; int h, band; SDL_atomic_t next; } Bands;

static int band_worker(void *arg) {
    Bands *b = arg;
    for (;;) {
        int y0 = SDL_AtomicAdd(&b->next, 1) * b->band;
        if (y0 >= b->h) break;
        b->rows(b->ctx, y0, y0 + b->band < b->h ? y0 + b->band : b->h);
    }
    return 0;
}

static int workers(void);

static void run_bands(void (*rows)(void *ctx, int y0, int y1), void *ctx, int h) {
    Bands b = { rows, ctx, h, 8, { 0 } };
    SDL_AtomicSet(&b.next, 0);
    Job jobs[MAX_WORKERS];
    int n = workers();
    for (int k = 0; k < n; k++) jobs[k] = (Job){ band_worker, &b };
    run_jobs(jobs, n);
}

static int workers(void) {
    static int n;
    if (!n) { n = SDL_GetCPUCount(); n = n < 1 ? 1 : n > MAX_WORKERS ? MAX_WORKERS : n; }
    return n;
}

typedef struct { const uint32_t *src; int scale; uint32_t *dst, *tmp; } ScaleJob;

static int scale_job(void *arg) {
    ScaleJob *j = arg;
    scale_any(j->src, HD_MAX_W, hd_frame.w, hd_frame.h, j->scale, j->dst, j->tmp);
    return 0;
}

static int scaled_with = -1;            /* hd_scaler the cached layers were made with */

typedef struct { const uint32_t *src; uint32_t *dst; int scale, y0, y1; } XbrJob;

static int xbr_job(void *arg) {
    XbrJob *j = arg;
    xbr_rows(j->src, HD_MAX_W, hd_frame.w, hd_frame.h, j->scale, j->dst, j->y0, j->y1);
    return 0;
}

static void scale_planes(int scale) {
    if (scaled_gen == hd_serial && scaled_scale == scale && scaled_with == hd_scaler) return;
    scaled_with = hd_scaler;
    if (hd_scaler == 1) {               /* the heavier scaler: every layer in bands over all cores */
        XbrJob xj[3 * MAX_WORKERS];
        Job jobs[3 * MAX_WORKERS];
        const uint32_t *layers[3] = { &hd_frame.a[0][0], &hd_frame.b[0][0], &hd_frame.s[0][0] };
        int per = (workers() + 2) / 3, n = 0;
        if (per < 1) per = 1;
        for (int l = 0; l < 3; l++)
            for (int k = 0; k < per; k++, n++) {
                xj[n] = (XbrJob){ layers[l], scaled[l], scale, hd_frame.h * k / per, hd_frame.h * (k + 1) / per };
                jobs[n] = (Job){ xbr_job, &xj[n] };
            }
        for (int at = 0; at < n; at += MAX_WORKERS) run_jobs(jobs + at, n - at > MAX_WORKERS ? MAX_WORKERS : n - at);
        scaled_gen = hd_serial;
        scaled_scale = scale;
        return;
    }
    ScaleJob sj[3] = {
        { &hd_frame.a[0][0], scale, scaled[0], temp[0] },
        { &hd_frame.b[0][0], scale, scaled[1], temp[1] },
        { &hd_frame.s[0][0], scale, scaled[2], temp[2] },
    };
    Job jobs[3] = { { scale_job, &sj[0] }, { scale_job, &sj[1] }, { scale_job, &sj[2] } };
    run_jobs(jobs, workers() >= 3 ? 3 : 1);
    if (workers() < 3) { scale_job(&sj[1]); scale_job(&sj[2]); }
    scaled_gen = hd_serial;
    scaled_scale = scale;
}

typedef struct {
    int scale, y0, y1;
    const uint32_t *sprites;
    const int *ox_a, *oy_a, *ox_b, *oy_b;
} ComposeJob;

/* ---- Shadows ------------------------------------------------------------------------- *
 * The low-priority sprites (characters and objects; the HUD is high priority) make a
 * mask at the game's resolution, moved down and right as if lit from the top left and
 * blurred; plane pixels under it are darkened, sprite pixels are not. */
bool hd_shadows;
static uint8_t *shadow_map;
static int shadow_w, shadow_h;

static void make_shadows(int scale, const uint32_t *sprites) {
    int w = hd_frame.w, h = hd_frame.h;
    if (w != shadow_w || h != shadow_h) {
        free(shadow_map);
        shadow_map = malloc((size_t)w * h * 2);
        shadow_w = w; shadow_h = h;
    }
    if (!shadow_map) return;
    uint8_t *m = shadow_map, *t = shadow_map + (size_t)w * h;
    const int ox = 2, oy = 3;                   /* the shadow falls this far down and right */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = x - ox, sy = y - oy;
            uint8_t v = 0;
            if (sx >= 0 && sy >= 0) {
                uint32_t p = sprites[(size_t)(sy * scale + scale / 2) * hd_w + sx * scale + scale / 2];
                uint8_t lb = (uint8_t)(p >> 16);
                if ((lb & 0x0F) && !(lb & 0x40)) v = 255;
            }
            t[y * w + x] = v;
        }
    for (int pass = 0; pass < 2; pass++) {      /* box blur, radius 2, rows then columns */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = -2; k <= 2; k++) { int xx = x + k; s += t[y * w + (xx < 0 ? 0 : xx >= w ? w - 1 : xx)]; }
                m[y * w + x] = (uint8_t)(s / 5);
            }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int s = 0;
                for (int k = -2; k <= 2; k++) { int yy = y + k; s += m[(yy < 0 ? 0 : yy >= h ? h - 1 : yy) * w + x]; }
                t[y * w + x] = (uint8_t)(s / 5);
            }
    }
    memcpy(m, t, (size_t)w * h);
}

/* ---- Water ----------------------------------------------------------------------------- *
 * Water is found by colour at the game's resolution: the plane pixel on top is clearly
 * blue (its blue well above its red and green), in runs at least five pixels wide so blue
 * details on land are left out. Under no sprite, a water pixel ripples in brightness,
 * sparkles at points fixed in the world (from the plane's scroll), and reflects the
 * sprites above it: mirrored at the water's top edge, or at a character's feet when one
 * stands in it, rippling, fading with the distance and tinted by the water. */
bool hd_water;
float hd_time;
static uint8_t *water_map;              /* per game pixel: 0 land, else the rows up to the edge (capped) */
static int8_t *water_wx;                /* per game pixel: which plane (for its scroll) */
static int water_w, water_h;
static int8_t wave_lut[256];

static inline bool is_water_colour(uint16_t c) {
    int r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
    return b >= 3 && b >= r + 2 && b >= g + 1;
}

/* The plane pixel the VDP would show (ignoring sprites); *plane gets 0 for A, 1 for B. */
static inline uint32_t plane_top(int line, int x, int *plane) {
    uint32_t a = hd_frame.a[line][x], b = hd_frame.b[line][x];
    bool ao = (a >> 16) & 0x0F, bo = (b >> 16) & 0x0F, ah = (a >> 16) & 0x40, bh = (b >> 16) & 0x40;
    if (ao && (ah || !(bo && bh))) { *plane = 0; return a; }
    *plane = 1;
    return bo ? b : 0;
}

static void make_water(int scale, const uint32_t *sprites) {
    int w = hd_frame.w, h = hd_frame.h;
    if (w != water_w || h != water_h) {
        free(water_map); free(water_wx);
        water_map = malloc((size_t)w * h);
        water_wx = malloc((size_t)w * h);
        water_w = w; water_h = h;
        for (int i = 0; i < 256; i++) wave_lut[i] = (int8_t)(sinf(i * 6.2831853f / 256) * 127);
    }
    if (!water_map || !water_wx) return;
    for (int y = 0; y < h; y++) {
        uint8_t *m = water_map + (size_t)y * w;
        int8_t *pl = water_wx + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int plane;
            uint32_t p = plane_top(y, x, &plane);
            uint32_t s = sprites[(size_t)(y * scale + scale / 2) * hd_w + x * scale + scale / 2];
            m[x] = ((p >> 16) & 0x0F) && is_water_colour((uint16_t)p) && !((s >> 16) & 0x0F);
            pl[x] = (int8_t)plane;
        }
        /* Runs of at least five, so blue details on land are not water. */
        for (int x = 0; x < w;) {
            if (!m[x]) { x++; continue; }
            int e = x;
            while (e < w && m[e]) e++;
            if (e - x < 5) for (int k = x; k < e; k++) m[k] = 0;
            x = e;
        }
    }
    /* Rows up to the edge above (a sprite in the water is an edge too), 1-60. */
    for (int x = 0; x < w; x++) {
        int run = 0;
        for (int y = 0; y < h; y++) {
            uint8_t *m = &water_map[(size_t)y * w + x];
            run = *m ? run + 1 : 0;
            *m = (uint8_t)(run > 60 ? 60 : run);
        }
    }
}

static inline uint32_t water_pixel(uint32_t c, int x, int y, int line, int scale, const uint32_t *sprites) {
    int nx = x / scale;
    int d = water_map[(size_t)line * water_w + nx];
    int t = (int)(hd_time * 90);
    /* Soft horizontal swells that wobble sideways as they roll: -127..127. */
    int wobble = wave_lut[(nx * 3 + t / 3) & 255] / 6;
    int wave = wave_lut[(line * 19 + wobble + t) & 255];
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    /* Ripples: brightness rising and falling along the water. */
    int k = 256 + wave / 12;
    r = r * k >> 8; g = g * k >> 8; b = b * k >> 8;
    /* Reflections of the sprites above the edge. */
    int sy = y - (2 * d - 1) * scale, sx = x + wave * scale / 160;
    if (sy >= 0 && sx >= 0 && sx < hd_w && d < 48) {
        uint32_t sp = sprites[(size_t)sy * hd_w + sx];
        uint8_t lb = (uint8_t)(sp >> 16);
        if ((lb & 0x0F) && !(lb & 0x40)) {
            uint32_t rc = vdp_color_rgb((uint16_t)sp);
            int a = 100 * (48 - d) / 48;                        /* out of 256: about 40% at the edge */
            int rr = (int)((rc >> 16) & 255) * 3 / 4, rg = (int)((rc >> 8) & 255) * 3 / 4, rb = (int)(rc & 255) * 3 / 4 + 30;
            r += (rr - r) * a >> 8; g += (rg - g) * a >> 8; b += ((rb > 255 ? 255 : rb) - b) * a >> 8;
        }
    }
    /* Sparkles at points fixed in the world: plane coordinates from the scroll. */
    int plane = water_wx[(size_t)line * water_w + nx];
    int wx = nx - hd_frame.extra - hd_frame.hscroll[plane][line], wy = line + hd_frame.vscroll[plane][line];
    if (((x % scale) == scale / 2 || scale == 1) && ((y % scale) == scale / 2 || scale == 1)) {
        uint32_t hsh = hash32((uint32_t)(wx & 1023) * 73856093u ^ (uint32_t)(wy & 1023) * 19349663u);
        if ((hsh & 63) == 0) {
            int ph = wave_lut[(int)((hd_time * (40 + (hsh >> 8 & 31))) + (hsh >> 16)) & 255];
            if (ph > 90) { int s = (ph - 90) * 6; r += s; g += s; b += s; }
        }
    }
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static inline uint32_t shade(uint32_t c, int s) {     /* s: 0-255 shadow */
    int k = 256 - s * 100 / 255;                        /* up to about 40% darker */
    return (uint32_t)((((c >> 16) & 255) * k >> 8) << 16 | (((c >> 8) & 255) * k >> 8) << 8 | ((c & 255) * k >> 8));
}

static int compose_band(void *arg) {
    const ComposeJob *j = arg;
    int scale = j->scale;
    const uint32_t *sprites = j->sprites;
    const int *ox_a = j->ox_a, *oy_a = j->oy_a, *ox_b = j->ox_b, *oy_b = j->oy_b;
    int mask = hd_frame.mask_sides ? hd_frame.extra * scale : 0;
    for (int y = j->y0; y < j->y1; y++) {
        int line = y / scale;
        uint16_t backdrop = hd_frame.backdrop[line];
        uint8_t flags = hd_frame.flags[line];
        uint32_t *out = hd_px + (size_t)y * hd_w;
        if (!(flags & HD_LINE_DISPLAY)) {
            uint32_t c = vdp_compose(0, 0, 0, backdrop, false);
            for (int x = 0; x < hd_w; x++) out[x] = c;
            continue;
        }
        bool sh = flags & HD_LINE_SH;
        const uint32_t *rs = sprites + (size_t)y * hd_w;
        const uint32_t *ra0 = scaled[0] + (size_t)y * hd_w;
        if (!ox_a) {
            const uint32_t *rb = scaled[1] + (size_t)y * hd_w;
            for (int x = 0; x < hd_w; x++) out[x] = vdp_compose(ra0[x], rb[x], rs[x], backdrop, sh);
        } else {
            /* Planes sampled where their content was at this moment; window pixels stay. */
            int ya = y + oy_a[line], yb = y + oy_b[line];
            ya = ya < 0 ? 0 : ya >= hd_h ? hd_h - 1 : ya;
            yb = yb < 0 ? 0 : yb >= hd_h ? hd_h - 1 : yb;
            const uint32_t *ra = scaled[0] + (size_t)ya * hd_w, *rb = scaled[1] + (size_t)yb * hd_w;
            int dxa = ox_a[line], dxb = ox_b[line];
            for (int x = 0; x < hd_w; x++) {
                int xa = x + dxa, xb = x + dxb;
                xa = xa < 0 ? 0 : xa >= hd_w ? hd_w - 1 : xa;
                xb = xb < 0 ? 0 : xb >= hd_w ? hd_w - 1 : xb;
                uint32_t a = (ra0[x] >> 16) & HD_WINDOW ? ra0[x] : ra[xa];
                out[x] = vdp_compose(a, rb[xb], rs[x], backdrop, sh);
            }
        }
        if (hd_water && water_map) {
            const uint8_t *wm = water_map + (size_t)line * water_w;
            for (int x = 0; x < hd_w; x++) {
                int nx = x / scale;
                if (nx < water_w && wm[nx] && !((rs[x] >> 16) & 0x0F)) out[x] = water_pixel(out[x], x, y, line, scale, sprites);
            }
        }
        if (hd_shadows && shadow_map) {
            const uint8_t *sm = shadow_map + (size_t)line * shadow_w;
            for (int x = 0; x < hd_w; x++) {
                int s = sm[x / scale < shadow_w ? x / scale : shadow_w - 1];
                if (s && !((rs[x] >> 16) & 0x0F)) out[x] = shade(out[x], s);
            }
        }
        if (mask) {
            memset(out, 0, (size_t)mask * 4);
            memset(out + hd_w - mask, 0, (size_t)mask * 4);
        }
    }
    return 0;
}

static void compose_part(void *ctx, int y0, int y1) {
    ComposeJob j = *(const ComposeJob *)ctx;
    j.y0 = y0; j.y1 = y1;
    compose_band(&j);
}

static void compose_rows(int scale, const uint32_t *sprites, const int *ox_a, const int *oy_a,
                         const int *ox_b, const int *oy_b) {
    if (hd_shadows) make_shadows(scale, sprites);
    if (hd_water) make_water(scale, sprites);
    ComposeJob cj = { scale, 0, 0, sprites, ox_a, oy_a, ox_b, oy_b };
    run_bands(compose_part, &cj, hd_h);
}

/* ---- Sharpening -------------------------------------------------------------------- */
typedef struct { const uint32_t *src; uint32_t *dst; int w, h, pitch, y0, y1; float peak; } SharpenJob;

static inline int luma(uint32_t p) { return (int)(((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8; }

static int sharpen_rows(void *arg) {
    const SharpenJob *j = arg;
    for (int y = j->y0; y < j->y1; y++) {
        const uint32_t *r = j->src + (size_t)y * j->pitch;
        const uint32_t *up = y > 0 ? r - j->pitch : r, *dn = y < j->h - 1 ? r + j->pitch : r;
        uint32_t *out = j->dst + (size_t)y * j->w;
        for (int x = 0; x < j->w; x++) {
            uint32_t e = r[x], b = up[x], hh = dn[x], d = r[x > 0 ? x - 1 : x], f = r[x < j->w - 1 ? x + 1 : x];
            int le = luma(e), lb = luma(b), lh = luma(hh), ld = luma(d), lf = luma(f);
            int mn = le, mx = le;
            if (lb < mn) mn = lb; if (lh < mn) mn = lh; if (ld < mn) mn = ld; if (lf < mn) mn = lf;
            if (lb > mx) mx = lb; if (lh > mx) mx = lh; if (ld > mx) mx = ld; if (lf > mx) mx = lf;
            if (mx == mn) { out[x] = e; continue; }
            /* How much room there is before clipping, relative to the brightest neighbour (AMD's CAS). */
            float room = (float)(mn < 255 - mx ? mn : 255 - mx) / (mx > 0 ? mx : 1);
            float amp = room > 1 ? 1 : sqrtf(room);
            float wgt = amp * j->peak;      /* negative: the neighbours are subtracted */
            float norm = 1 / (1 + 4 * wgt);
            int c[3];
            for (int k = 0; k < 3; k++) {
                int sh = 16 - k * 8;
                float v = (((e >> sh) & 255) + wgt * (((b >> sh) & 255) + ((hh >> sh) & 255) + ((d >> sh) & 255) + ((f >> sh) & 255))) * norm;
                c[k] = v < 0 ? 0 : v > 255 ? 255 : (int)(v + .5f);
            }
            out[x] = (uint32_t)(c[0] << 16 | c[1] << 8 | c[2]);
        }
    }
    return 0;
}

static void sharpen_part(void *ctx, int y0, int y1) {
    SharpenJob j = *(const SharpenJob *)ctx;
    j.y0 = y0; j.y1 = y1;
    sharpen_rows(&j);
}

const uint32_t *hd_sharpen(const uint32_t *src, int w, int h, int pitch, int level) {
    static uint32_t *buf;
    static size_t cap;
    if (level <= 0 || w <= 0 || h <= 0) return src;
    size_t need = (size_t)w * h;
    if (need > cap) { free(buf); buf = malloc(need * 4); cap = buf ? need : 0; }
    if (!buf) return src;
    static const float peaks[4] = { 0, -1 / 8.f, -1 / 6.5f, -1 / 5.f };
    float peak = peaks[level > 3 ? 3 : level];
    SharpenJob sj = { src, buf, w, h, pitch, 0, 0, peak };
    run_bands(sharpen_part, &sj, h);
    return buf;
}

/* ---- Lighting and ambience ------------------------------------------------------------ *
 * Lighting: the bright and the strongly coloured pixels (fire, magic, sunlit stone, the
 * spirits) are gathered into a picture an eighth the size, blurred at two sizes, and
 * laid over the picture with a screen blend, so they glow into their surroundings.
 * Ambience: a soft vignette, and in the game world dust motes that drift in the air;
 * they keep their place in the world as the camera moves. */
typedef struct { const uint32_t *src; uint32_t *dst; int w, h, pitch, y0, y1; } PostJob;

static float *bloom, *bloom_tmp;
static int bloom_w, bloom_h, bloom_f;
static float *vignette_x, *vignette_y;
static int vig_w, vig_h;
static float light_k;

static void blur_bloom(float *img, int r) {
    int bw = bloom_w, bh = bloom_h;
    for (int c = 0; c < 3; c++) {
        for (int y = 0; y < bh; y++) {          /* rows, then columns: a box blur twice is close to a gaussian */
            float *row = img + (size_t)y * bw * 3 + c, *t = bloom_tmp;
            for (int pass = 0; pass < 2; pass++) {
                float s = 0;
                for (int x = -r; x <= r; x++) s += row[(x < 0 ? 0 : x >= bw ? bw - 1 : x) * 3];
                for (int x = 0; x < bw; x++) {
                    t[x] = s / (2 * r + 1);
                    int xa = x + r + 1, xb = x - r;
                    s += row[(xa >= bw ? bw - 1 : xa) * 3] - row[(xb < 0 ? 0 : xb) * 3];
                }
                for (int x = 0; x < bw; x++) row[x * 3] = t[x];
            }
        }
        for (int x = 0; x < bw; x++) {
            for (int pass = 0; pass < 2; pass++) {
                float s = 0;
                for (int y = -r; y <= r; y++) s += img[((size_t)(y < 0 ? 0 : y >= bh ? bh - 1 : y) * bw + x) * 3 + c];
                for (int y = 0; y < bh; y++) {
                    bloom_tmp[y] = s / (2 * r + 1);
                    int ya = y + r + 1, yb = y - r;
                    s += img[((size_t)(ya >= bh ? bh - 1 : ya) * bw + x) * 3 + c] - img[((size_t)(yb < 0 ? 0 : yb) * bw + x) * 3 + c];
                }
                for (int y = 0; y < bh; y++) img[((size_t)y * bw + x) * 3 + c] = bloom_tmp[y];
            }
        }
    }
}

static float lights_amb;                /* how much the surroundings dim in dark scenes */

static void make_bloom(const uint32_t *src, int w, int h, int pitch, int level, bool lights) {
    int f = w / 160 > 1 ? w / 160 : 1;
    int bw = (w + f - 1) / f, bh = (h + f - 1) / f;
    if (bw != bloom_w || bh != bloom_h || f != bloom_f) {
        free(bloom); free(bloom_tmp);
        bloom = malloc(sizeof(float) * 3 * 3 * (size_t)bw * bh);
        bloom_tmp = malloc(sizeof(float) * (size_t)(bw > bh ? bw : bh) * 2);
        bloom_w = bw; bloom_h = bh; bloom_f = f;
    }
    float threshold = level >= 2 ? .74f : .8f;
    float *lmap = bloom + (size_t)6 * bw * bh;
    int step = f / 3 > 1 ? f / 3 : 1;          /* a few samples per cell are plenty for a blur */
    long samples = 0;
    double scene = 0;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            float acc[3] = { 0, 0, 0 }, lit[3] = { 0, 0, 0 };
            int n = 0;
            for (int y = by * f + step / 2; y < by * f + f && y < h; y += step)
                for (int x = bx * f + step / 2; x < bx * f + f && x < w; x += step) {
                    uint32_t p = src[(size_t)y * pitch + x];
                    float r = ((p >> 16) & 255) / 255.f, g = ((p >> 8) & 255) / 255.f, b = (p & 255) / 255.f;
                    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
                    float lum = r * .3f + g * .55f + b * .15f;
                    /* Near white, or vivid and bright (fire, magic, gems): what would give off light.
                     * Sunlit ground is bright but neither, so a daylight scene does not wash out. */
                    float vivid = (mx - mn) * mx;
                    float glow = level ? fmaxf((lum - threshold) / (1 - threshold), (vivid - .5f) / .5f) * (mx > .6f) : 0;
                    if (glow > 0) { acc[0] += r * glow; acc[1] += g * glow; acc[2] += b * glow; }
                    /* A light source: vivid and bright, like fire, magic, glowing crystals. */
                    float source = clampf((vivid - .45f) / .35f, 0, 1) * (mx > .55f);
                    if (source > 0) { lit[0] += r * source; lit[1] += g * source; lit[2] += b * source; }
                    scene += lum;
                    n++;
                    samples++;
                }
            float *o = bloom + ((size_t)by * bw + bx) * 3, *lo = lmap + ((size_t)by * bw + bx) * 3;
            for (int c = 0; c < 3; c++) { o[c] = n ? acc[c] / n : 0; lo[c] = n ? lit[c] / n : 0; }
        }
    if (lights) {
        /* Light spreads far: two wide blurs. In a dark scene (a cave, a night) the
         * surroundings dim a little so the lights stand out; daylight stays as it is. */
        blur_bloom(lmap, 6);
        blur_bloom(lmap, 10);
        float avg = (float)(scene / (samples + 1));
        lights_amb = avg < .32f ? .8f + .2f * avg / .32f : 1;
    }
    /* Two sizes of glow: a tight one and a wide one. */
    float *wide = bloom + (size_t)3 * bw * bh;
    memcpy(wide, bloom, sizeof(float) * 3 * (size_t)bw * bh);
    if (level) {
        blur_bloom(bloom, 1);
        blur_bloom(wide, 5);
        for (size_t i = 0; i < (size_t)3 * bw * bh; i++) bloom[i] = bloom[i] * .6f + wide[i] * 1.1f;
    }
    light_k = level >= 2 ? .75f : .45f;
}

static void make_vignette(int w, int h) {
    if (w == vig_w && h == vig_h) return;
    free(vignette_x); free(vignette_y);
    vignette_x = malloc(sizeof(float) * (size_t)w);
    vignette_y = malloc(sizeof(float) * (size_t)h);
    for (int x = 0; x < w; x++) { float u = (x + .5f) / w * 2 - 1; vignette_x[x] = 1 - .22f * powf(fabsf(u), 2.6f); }
    for (int y = 0; y < h; y++) { float v = (y + .5f) / h * 2 - 1; vignette_y[y] = 1 - .3f * powf(fabsf(v), 2.4f); }
    vig_w = w; vig_h = h;
}

static bool post_light, post_vignette, post_lights, post_cinematic;
static int *col_x0, *col_x1;            /* per picture column: the two map cells it lies between */
static float *col_tx;
static int col_w, col_f;
static uint8_t tone_lut[256];           /* the cinematic tone curve, per channel */
static bool tone_ready;
static int8_t grain[256 * 256];         /* film grain, shifted every picture */
static int16_t tint_r[256], tint_g[256], tint_b[256];   /* the grade's shift for each brightness */
static float *fringe_x;                 /* per column: (x from the middle, -1..1) squared */
static int fringe_w;

static void make_columns(int w) {
    if (w == col_w && bloom_f == col_f) return;
    free(col_x0); free(col_x1); free(col_tx);
    col_x0 = malloc(sizeof(int) * (size_t)w); col_x1 = malloc(sizeof(int) * (size_t)w); col_tx = malloc(sizeof(float) * (size_t)w);
    for (int x = 0; x < w; x++) {
        float fx = (x + .5f) / bloom_f - .5f;
        int x0 = (int)floorf(fx), x1 = x0 + 1;
        col_tx[x] = fx - x0;
        col_x0[x] = x0 < 0 ? 0 : x0 >= bloom_w ? bloom_w - 1 : x0;
        col_x1[x] = x1 < 0 ? 0 : x1 >= bloom_w ? bloom_w - 1 : x1;
    }
    col_w = w; col_f = bloom_f;
}

static void make_tone(int w) {
    if (w != fringe_w) {
        free(fringe_x);
        fringe_x = malloc(sizeof(float) * (size_t)w);
        for (int x = 0; x < w; x++) { float cx = (x + .5f) / w * 2 - 1; fringe_x[x] = cx * cx; }
        fringe_w = w;
    }
    if (tone_ready) return;
    for (int i = 0; i < 256 * 256; i++) grain[i] = (int8_t)((int)(hash32((uint32_t)i * 2654435761u) & 255) - 128);
    for (int l = 0; l < 256; l++) {        /* cooler shadows, warmer highlights */
        float L = l / 255.f, sh = (1 - L) * (1 - L) * .09f, hi = L * L * .08f;
        tint_r[l] = (int16_t)((hi * 1.0f - sh * .3f) * 255);
        tint_g[l] = (int16_t)((hi * .45f + sh * .35f) * 255);
        tint_b[l] = (int16_t)((-hi * .35f + sh * .7f) * 255);
    }
    for (int i = 0; i < 256; i++) {
        /* An S-curve: more contrast in the midtones, black and white stay where they are. */
        float c = i / 255.f, sc = c * c * (3 - 2 * c);
        tone_lut[i] = (uint8_t)(clampf(c * .45f + sc * .55f, 0, 1) * 255 + .5f);
    }
    tone_ready = true;
}
static uint32_t post_seed;              /* the grain's pattern changes every picture */

static inline void sample_map(const float *map, int x0, int x1, int y0, int y1, float tx, float ty, float out[3]) {
    int bw = bloom_w;
    const float *a = map + ((size_t)y0 * bw + x0) * 3, *b = map + ((size_t)y0 * bw + x1) * 3;
    const float *c = map + ((size_t)y1 * bw + x0) * 3, *d = map + ((size_t)y1 * bw + x1) * 3;
    for (int k = 0; k < 3; k++) {
        float top = a[k] + (b[k] - a[k]) * tx, bot = c[k] + (d[k] - c[k]) * tx;
        out[k] = top + (bot - top) * ty;
    }
}

static int post_rows(void *arg) {
    const PostJob *j = arg;
    int f = bloom_f, bw = bloom_w, bh = bloom_h;
    for (int y = j->y0; y < j->y1; y++) {
        const uint32_t *s = j->src + (size_t)y * j->pitch;
        uint32_t *o = j->dst + (size_t)y * j->w;
        float fy = (y + .5f) / f - .5f;
        int y0 = (int)floorf(fy), y1 = y0 + 1;
        float ty = fy - y0;
        y0 = y0 < 0 ? 0 : y0 >= bh ? bh - 1 : y0; y1 = y1 < 0 ? 0 : y1 >= bh ? bh - 1 : y1;
        float vy = post_vignette ? vignette_y[y] : 1;
        float cy = (y + .5f) / j->h * 2 - 1, cy2 = cy * cy;
        const int8_t *grain_row = grain + (((uint32_t)y + post_seed) & 255) * 256;
        int grain_shift = (int)(post_seed >> 8);
        for (int x = 0; x < j->w; x++) {
            uint32_t p = s[x];
            float r = ((p >> 16) & 255) * (1 / 255.f), g = ((p >> 8) & 255) * (1 / 255.f), b = (p & 255) * (1 / 255.f);
            if (post_cinematic) {
                /* Lens fringing: red and blue a touch apart, more towards the edges. */
                int d = (int)((fringe_x[x] + cy2) * j->w * .0012f);
                if (d) {
                    if (x < j->w / 2) d = -d;
                    int xr = x + d, xb = x - d;
                    xr = xr < 0 ? 0 : xr >= j->w ? j->w - 1 : xr;
                    xb = xb < 0 ? 0 : xb >= j->w ? j->w - 1 : xb;
                    r = ((s[xr] >> 16) & 255) / 255.f;
                    b = (s[xb] & 255) / 255.f;
                }
            }
            if (post_light || post_lights) {
                int x0 = col_x0[x], x1 = col_x1[x];
                float tx = col_tx[x];
                if (post_lights) {
                    /* Surfaces near a light take its colour; the rest dims in dark scenes. */
                    float l[3];
                    sample_map(bloom + (size_t)6 * bw * bh, x0, x1, y0, y1, tx, ty, l);
                    r = r * lights_amb + r * l[0] * 2.2f + l[0] * .12f;
                    g = g * lights_amb + g * l[1] * 2.2f + l[1] * .12f;
                    b = b * lights_amb + b * l[2] * 2.2f + l[2] * .12f;
                }
                if (post_light) {
                    float l[3];
                    sample_map(bloom, x0, x1, y0, y1, tx, ty, l);
                    for (int k = 0; k < 3; k++) l[k] = fminf(l[k] * light_k, 1);
                    r = 1 - (1 - fminf(r, 1)) * (1 - l[0]); g = 1 - (1 - fminf(g, 1)) * (1 - l[1]); b = 1 - (1 - fminf(b, 1)) * (1 - l[2]);
                }
            }
            int ir = (int)(clampf(r, 0, 1) * 255 + .5f), ig = (int)(clampf(g, 0, 1) * 255 + .5f), ib = (int)(clampf(b, 0, 1) * 255 + .5f);
            if (post_cinematic) {
                /* Tone curve, then the grade by brightness, a little more colour, and grain. */
                ir = tone_lut[ir]; ig = tone_lut[ig]; ib = tone_lut[ib];
                int l = (ir * 77 + ig * 141 + ib * 38) >> 8;
                ir += tint_r[l]; ig += tint_g[l]; ib += tint_b[l];
                int n = grain_row[(x + grain_shift) & 255] * 9 >> 8;
                ir = l + (((ir - l) * 302) >> 8) + n;
                ig = l + (((ig - l) * 302) >> 8) + n;
                ib = l + (((ib - l) * 302) >> 8) + n;
                ir = ir < 0 ? 0 : ir > 255 ? 255 : ir; ig = ig < 0 ? 0 : ig > 255 ? 255 : ig; ib = ib < 0 ? 0 : ib > 255 ? 255 : ib;
            }
            if (post_vignette) { int v = (int)(vignette_x[x] * vy * 256); ir = ir * v >> 8; ig = ig * v >> 8; ib = ib * v >> 8; }
            o[x] = (uint32_t)(ir << 16 | ig << 8 | ib);
        }
    }
    return 0;
}

/* Dust motes: a field of points that repeats every 512 game pixels, scrolled with the camera. */
static void draw_motes(uint32_t *px, int w, int h, const HdPost *p) {
    int s = p->scale;
    for (int i = 0; i < 70; i++) {
        uint32_t hsh = (uint32_t)i * 2654435761u;
        float wx = (hsh & 511) + p->t * (4 + (hsh >> 9 & 7)) + 6 * sinf(p->t * .7f + i);
        float wy = ((hsh >> 12) & 511) + 5 * sinf(p->t * .5f + i * 1.3f) - p->t * 2;
        float sx = fmodf(wx - p->cam_x * .9f, 512), sy = fmodf(wy - p->cam_y * .9f, 512);
        if (sx < 0) sx += 512;
        if (sy < 0) sy += 512;
        float tw = .5f + .5f * sinf(p->t * (1.5f + (hsh >> 20 & 3)) + i);
        for (int rep_x = 0; rep_x * 512 < w / s + 512; rep_x++)
            for (int rep_y = 0; rep_y * 512 < h / s + 512; rep_y++) {
                float cx = (sx + rep_x * 512 - 64) * s, cy = (sy + rep_y * 512 - 64) * s;
                float r = s * (1.1f + (hsh >> 24 & 1) * .6f);
                int x0 = (int)(cx - r * 2), x1 = (int)(cx + r * 2), y0 = (int)(cy - r * 2), y1 = (int)(cy + r * 2);
                if (x1 < 0 || y1 < 0 || x0 >= w || y0 >= h) continue;
                for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < h; y++)
                    for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < w; x++) {
                        float dx = (x - cx) / r, dy = (y - cy) / r, d2 = dx * dx + dy * dy;
                        if (d2 > 4) continue;
                        float k = (1 - d2 / 4) * (1 - d2 / 4) * .28f * tw;
                        uint32_t q = px[(size_t)y * w + x];
                        int cr = (int)((q >> 16) & 255), cg = (int)((q >> 8) & 255), cb = (int)(q & 255);
                        cr += (int)((255 - cr) * k); cg += (int)((240 - cg) * k * .95f); cb += (int)((200 - cb) * k * .8f);
                        px[(size_t)y * w + x] = (uint32_t)(cr << 16 | cg << 8 | (cb < 0 ? 0 : cb));
                    }
            }
    }
}

static void post_part(void *ctx, int y0, int y1) {
    PostJob j = *(const PostJob *)ctx;
    j.y0 = y0; j.y1 = y1;
    post_rows(&j);
}

const uint32_t *hd_post(const uint32_t *src, int w, int h, int pitch, const HdPost *p) {
    static uint32_t *buf;
    static size_t cap;
    post_light = p->lighting > 0;
    post_vignette = p->ambience;
    post_lights = p->lights;
    post_cinematic = p->cinematic;
    post_seed = hash32((uint32_t)(p->t * 1000));
    bool motes = p->ambience && p->world;
    bool world_fx = p->world && (p->weather || p->elements);
    if (post_light || post_vignette || motes || post_lights || post_cinematic || world_fx) {
        size_t need = (size_t)w * h;
        if (need > cap) { free(buf); buf = malloc(need * 4); cap = buf ? need : 0; }
        if (buf) {
            static unsigned maps_for;
            static int maps_w, maps_level;
            static bool maps_lights;
            if ((post_light || post_lights) && (maps_for != hd_serial || maps_w != w || maps_level != p->lighting || maps_lights != post_lights)) {
                make_bloom(src, w, h, pitch, p->lighting, post_lights);   /* once per game frame */
                maps_for = hd_serial; maps_w = w; maps_level = p->lighting; maps_lights = post_lights;
            }
            if (post_light || post_lights) make_columns(w);
            if (post_cinematic) make_tone(w);
            if (post_vignette) make_vignette(w, h);
            PostJob pj = { src, buf, w, h, pitch, 0, 0 };
            run_bands(post_part, &pj, h);
            if (motes) draw_motes(buf, w, h, p);
            if (world_fx) fx_draw(buf, w, h, p);
            src = buf;
            pitch = w;
        }
    }
    return hd_sharpen(src, w, h, pitch, p->sharpen);
}

static int clamp_scale(int scale) { return scale < 1 ? 1 : scale > HD_MAX_SCALE ? HD_MAX_SCALE : scale; }

void hd_compose(int scale) {
    scale = clamp_scale(scale);
    if (!ensure(scale)) return;
    scale_planes(scale);
    compose_rows(scale, scaled[2], NULL, NULL, NULL, NULL);
}

/* Signed difference of two scroll values that wrap at 1024. */
static int scroll_delta(int now, int was) { return ((now - was + 512) & 1023) - 512; }

static void scale_sprites(int scale) {
    static int made_with = -1;
    if (sprites_gen == hd_serial && sprites_scale == scale && made_with == hd_scaler) return;
    made_with = hd_scaler;
    static uint32_t padded[34 * 34];
    for (int i = 0; i < 80; i++) {
        const HdSprite *c = &hd_frame.sprites[i];
        if (!c->drawn) continue;
        int pw = c->w + 2, ph = c->h + 2;
        memset(padded, 0, sizeof padded);
        for (int y = 0; y < c->h; y++) memcpy(padded + (y + 1) * pw + 1, c->px + y * 32, (size_t)c->w * 4);
        scale_any(padded, pw, pw, ph, scale, sprite_scaled[i], sprite_tmp);
    }
    sprites_gen = hd_serial;
    sprites_scale = scale;
}

/* Sprites at their positions for moment t, back to front so earlier sprites win. */
static void draw_sprite_layer(int scale, float t) {
    memset(sprite_layer, 0, (size_t)hd_w * hd_h * 4);
    int order[80], n = 0;
    for (int i = 0; i < 80; i++) if (hd_frame.sprites[i].drawn) order[n++] = i;
    for (int a = 1; a < n; a++) {       /* by rank, highest (backmost) first */
        int v = order[a], b = a;
        while (b > 0 && hd_frame.sprites[order[b - 1]].rank < hd_frame.sprites[v].rank) { order[b] = order[b - 1]; b--; }
        order[b] = v;
    }
    float back = 1 - t;
    for (int k = 0; k < n; k++) {
        int i = order[k];
        const HdSprite *c = &hd_frame.sprites[i];
        float x = c->x - move_x[i] * back, y = c->y - move_y[i] * back;
        int sw = (c->w + 2) * scale, sh = (c->h + 2) * scale;
        int X0 = (int)lroundf((x + hd_frame.extra) * scale) - scale, Y0 = (int)lroundf(y * scale) - scale;
        const uint32_t *src = sprite_scaled[i];
        for (int yy = 0; yy < sh; yy++) {
            int Y = Y0 + yy;
            if (Y < 0 || Y >= hd_h) continue;
            uint32_t *row = sprite_layer + (size_t)Y * hd_w;
            for (int xx = 0; xx < sw; xx++) {
                int X = X0 + xx;
                uint32_t v = src[yy * sw + xx];
                if (X < 0 || X >= hd_w || !((v >> 16) & 0x0F)) continue;
                row[X] = v;
            }
        }
    }
}

static double ms_since(Uint64 *t0) {
    Uint64 now = SDL_GetPerformanceCounter();
    double ms = (double)(now - *t0) * 1000 / SDL_GetPerformanceFrequency();
    *t0 = now;
    return ms;
}

/* Draws the newest frame's content with the planes at moment tp and the sprites at
 * moment ts between the previous update (0) and the latest (1). */
static void compose_between(int scale, float tp, float ts) {
    Uint64 t0 = SDL_GetPerformanceCounter();
    scale_planes(scale);
    hd_ms[0] = ms_since(&t0);
    static int ox[2][HD_MAX_H], oy[2][HD_MAX_H];
    bool planes_move = have_before && before.w == hd_frame.w && before.h == hd_frame.h;
    float back = 1 - tp;
    for (int line = 0; line < hd_frame.h; line++) {
        bool still = !planes_move || (hd_frame.flags[line] & HD_LINE_COLUMN_VSCROLL) || !(before.flags[line] & HD_LINE_DISPLAY);
        for (int p = 0; p < 2; p++) {
            int dh = still ? 0 : scroll_delta(hd_frame.hscroll[p][line], before.hscroll[p][line]);
            int dv = still ? 0 : scroll_delta(hd_frame.vscroll[p][line], before.vscroll[p][line]);
            if (abs(dh) > 24 || abs(dv) > 24) dh = dv = 0;    /* a cut, not a scroll */
            ox[p][line] = (int)lroundf(back * dh * scale);
            oy[p][line] = (int)lroundf(-back * dv * scale);
        }
    }
    const uint32_t *layer = scaled[2];
    hd_ms[1] = hd_ms[2] = 0;
    if (!hd_frame.sprite_mask) {
        ms_since(&t0);
        scale_sprites(scale);
        hd_ms[1] = ms_since(&t0);
        draw_sprite_layer(scale, ts);
        hd_ms[2] = ms_since(&t0);
        layer = sprite_layer;
    }
    ms_since(&t0);
    compose_rows(scale, layer, ox[0], oy[0], ox[1], oy[1]);
    hd_ms[3] = ms_since(&t0);
}

float hd_motion_t(float frac) { return timeline_t(&sprites, frac); }

void hd_compose_at(int scale, float frac) {
    scale = clamp_scale(scale);
    if (!ensure(scale)) return;
    /* Draw every picture the same way, also when nothing is moving (t = 1): switching
     * between whole-layer and per-sprite drawing would flicker where sprites overlap. */
    compose_between(scale, timeline_t(&planes, frac), timeline_t(&sprites, frac));
}

void hd_compose_sprites(int scale) {
    scale = clamp_scale(scale);
    if (!ensure(scale)) return;
    compose_between(scale, 1, 1);
}
