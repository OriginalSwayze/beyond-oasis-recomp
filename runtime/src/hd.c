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
#include "texpack.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>


static inline uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }

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
        if (scale == 6) { xbr(src, pitch, w, h, 3, tmp); xbr(tmp, w * 3, w * 3, h * 3, 2, dst); }
        else xbr(src, pitch, w, h, scale, dst);
    }
    else if (scale == 6) { scale3x(src, pitch, w, h, tmp); scale2x(tmp, w * 3, w * 3, h * 3, dst); }
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

/* ---- Texture packs --------------------------------------------------------------------- *
 * With a pack loaded, the replacement colour of every picture pixel (texpack_texel: 0 none,
 * 1 transparent, else 0xFF000000 | RGB) is worked out next to each scaled layer: for the
 * planes and the whole sprite layer once per game frame, for each sprite with its scaled
 * copy (so it follows the sprite in smooth motion). While compositing, a replacement that
 * is transparent makes that layer's pixel transparent and an opaque one makes it opaque;
 * if that layer ends up on top, the pixel is the replacement's colour. */
bool hd_texpack;
static uint32_t *tex_layer[3];          /* plane A, plane B, sprites (whole layer) */
static uint32_t *sprite_tex_layer;      /* sprites at their in-between positions */
static uint32_t (*sprite_tex)[SPR_PAD_MAX * SPR_PAD_MAX];   /* [80], with sprite_scaled */
static size_t tex_cap;

static bool ensure_tex(size_t need) {
    if (need <= tex_cap) return true;
    for (int k = 0; k < 3; k++) { free(tex_layer[k]); tex_layer[k] = malloc(need * 4); }
    free(sprite_tex_layer); sprite_tex_layer = malloc(need * 4);
    if (!sprite_tex) sprite_tex = malloc(sizeof *sprite_tex * 80);
    bool ok = tex_layer[0] && tex_layer[1] && tex_layer[2] && sprite_tex_layer && sprite_tex;
    tex_cap = ok ? need : 0;
    return ok;
}
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

void hd_rows(void (*rows)(void *ctx, int y0, int y1), void *ctx, int h) { run_bands(rows, ctx, h); }

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

typedef struct { const uint32_t *src; uint32_t *dst; int pitch, w, h, scale, y0, y1; } XbrJob;

static int xbr_job(void *arg) {
    XbrJob *j = arg;
    xbr_rows(j->src, j->pitch, j->w, j->h, j->scale, j->dst, j->y0, j->y1);
    return 0;
}

/* One xBR pass over the three layers, every layer in bands over all cores. */
static void xbr_pass(const uint32_t *const src[3], int pitch, int w, int h, int n, uint32_t *const dst[3]) {
    XbrJob xj[3 * MAX_WORKERS];
    Job jobs[3 * MAX_WORKERS];
    int per = (workers() + 2) / 3, count = 0;
    if (per < 1) per = 1;
    for (int l = 0; l < 3; l++)
        for (int k = 0; k < per; k++, count++) {
            xj[count] = (XbrJob){ src[l], dst[l], pitch, w, h, n, h * k / per, h * (k + 1) / per };
            jobs[count] = (Job){ xbr_job, &xj[count] };
        }
    for (int at = 0; at < count; at += MAX_WORKERS) run_jobs(jobs + at, count - at > MAX_WORKERS ? MAX_WORKERS : count - at);
}

static int tex_scale;

static void tex_rows(void *ctx, int y0, int y1) {
    (void)ctx;
    int s = tex_scale;
    for (int y = y0; y < y1; y++) {
        int line = y / s, sy = y % s;
        size_t o = (size_t)y * hd_w;
        for (int x = 0; x < hd_w; x++) {
            int nx = x / s, sx = x % s;
            tex_layer[0][o + x] = texpack_texel(hd_frame.ta[line][nx], sx, sy, s);
            tex_layer[1][o + x] = texpack_texel(hd_frame.tb[line][nx], sx, sy, s);
            tex_layer[2][o + x] = texpack_texel(hd_frame.ts[line][nx], sx, sy, s);
        }
    }
}

static unsigned tex_gen;                /* the frame the replacement layers were made for */
static int tex_gen_scale;

static void make_tex_layers(int scale) {
    if (!hd_texpack || (tex_gen == hd_serial && tex_gen_scale == scale) || !ensure_tex((size_t)hd_w * hd_h)) return;
    tex_scale = scale;
    run_bands(tex_rows, NULL, hd_h);
    tex_gen = hd_serial;
    tex_gen_scale = scale;
}

static void scale_planes(int scale) {
    if (scaled_gen == hd_serial && scaled_scale == scale && scaled_with == hd_scaler) { make_tex_layers(scale); return; }
    scaled_with = hd_scaler;
    if (hd_scaler == 1 && scale > 1) {  /* the heavier scaler: every layer in bands over all cores */
        const uint32_t *layers[3] = { &hd_frame.a[0][0], &hd_frame.b[0][0], &hd_frame.s[0][0] };
        int w = hd_frame.w, h = hd_frame.h;
        if (scale == 6) {               /* 3x, then 2x of that */
            const uint32_t *mid[3] = { temp[0], temp[1], temp[2] };
            xbr_pass(layers, HD_MAX_W, w, h, 3, temp);
            xbr_pass(mid, w * 3, w * 3, h * 3, 2, scaled);
        } else {
            xbr_pass(layers, HD_MAX_W, w, h, scale, scaled);
        }
        scaled_gen = hd_serial;
        scaled_scale = scale;
        make_tex_layers(scale);
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
    make_tex_layers(scale);
}

typedef struct {
    int scale, y0, y1;
    const uint32_t *sprites;
    const int *ox_a, *oy_a, *ox_b, *oy_b;
} ComposeJob;

float hd_time;

/* ---- Water ----------------------------------------------------------------------------- *
 * Water is found by colour at the game's resolution: the plane pixel on top is clearly
 * blue (its blue well above its red and green), in runs at least five pixels wide so blue
 * details on land are left out. Under no sprite, a water pixel ripples in brightness,
 * sparkles at points fixed in the world (from the plane's scroll), glints that drift along
 * the crests, foams where it meets the shore or a character standing in it (the foam laps
 * in and out), and reflects the sprites above it: mirrored at the water's top edge, or at
 * a character's feet, rippling, fading with the distance and tinted by the water. */
bool hd_water;
static uint8_t *water_map;              /* per game pixel: 0 land, else the rows up to the edge (capped) */
static uint8_t *water_edge;             /* per game pixel: how far to the nearest edge, any side (capped) */
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
        free(water_map); free(water_wx); free(water_edge);
        water_map = malloc((size_t)w * h);
        water_edge = malloc((size_t)w * h);
        water_wx = malloc((size_t)w * h);
        water_w = w; water_h = h;
        for (int i = 0; i < 256; i++) wave_lut[i] = (int8_t)(sinf(i * 6.2831853f / 256) * 127);
    }
    if (!water_map || !water_wx || !water_edge) return;
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
    /* How far to the nearest edge in any direction (for the foam), 0 on land, capped at 8:
     * two passes of a chamfer distance over the water. */
    for (size_t i = 0; i < (size_t)w * h; i++) water_edge[i] = water_map[i] ? 8 : 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *e = &water_edge[(size_t)y * w + x];
            if (!*e) continue;
            int v = *e;
            if (x > 0 && water_edge[(size_t)y * w + x - 1] + 1 < v) v = water_edge[(size_t)y * w + x - 1] + 1;
            if (y > 0 && water_edge[(size_t)(y - 1) * w + x] + 1 < v) v = water_edge[(size_t)(y - 1) * w + x] + 1;
            if (x == 0 || y == 0) v = v < 8 ? v : 8;
            *e = (uint8_t)v;
        }
    for (int y = h - 1; y >= 0; y--)
        for (int x = w - 1; x >= 0; x--) {
            uint8_t *e = &water_edge[(size_t)y * w + x];
            if (!*e) continue;
            int v = *e;
            if (x < w - 1 && water_edge[(size_t)y * w + x + 1] + 1 < v) v = water_edge[(size_t)y * w + x + 1] + 1;
            if (y < h - 1 && water_edge[(size_t)(y + 1) * w + x] + 1 < v) v = water_edge[(size_t)(y + 1) * w + x] + 1;
            *e = (uint8_t)v;
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
    /* Foam where the water meets the shore or a character: a white edge whose width laps
     * in and out, at its own pace along the shore, with a little froth just past it. */
    int edge = water_edge[(size_t)line * water_w + nx];
    if (edge < 8) {
        int lap = wave_lut[(t / 2 + wx * 3) & 255] + wave_lut[(t + wx * 9 + wy * 5) & 255] / 2;   /* -190..190 */
        int width = 512 + lap * 2;                          /* foam width in 1/256 game pixels: 0.5-3.5 */
        int dist = edge * 256 - 256;                        /* 0 right at the edge */
        if (dist < width) {
            int f = (width - dist) * 300 / (width + 160);    /* out of 256: up to about 80% at the edge */
            f = f > 210 ? 210 : f;
            uint32_t froth = hash32((uint32_t)(wx & 1023) * 2654435761u ^ (uint32_t)(wy & 1023) * 40503u ^ (uint32_t)(t / 12));
            if (dist > width - 200 && (froth & 7)) f /= 3;   /* the outer rim breaks into froth */
            r += (238 - r) * f >> 8; g += (246 - g) * f >> 8; b += (255 - b) * f >> 8;
        }
    }
    /* Glints that drift along the crests of the swells. */
    if (wave > 96) {
        uint32_t gh = hash32((uint32_t)((wx + t / 6) >> 2) * 2246822519u ^ (uint32_t)(wy >> 1) * 3266489917u);
        if ((gh & 31) == 0) { int s = (wave - 96) * 3; r += s; g += s; b += s; }
    }
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


/* A layer pixel as its replacement makes it: transparent or opaque. */
static inline uint32_t with_tex(uint32_t p, uint32_t t) {
    if (t == 1) return p & ~0x0F0000u;
    if (t >= 0xFF000000u && !((p >> 16) & 0x0F)) return p | 0x010000u;
    return p;
}

/* Which layer the VDP shows: 0 sprites, 1 plane A, 2 plane B, -1 the backdrop. */
static inline int top_layer(uint32_t a, uint32_t b, uint32_t s) {
    uint8_t ab = (uint8_t)(a >> 16), bb = (uint8_t)(b >> 16), sb = (uint8_t)(s >> 16);
    bool ao = ab & 0x0F, bo = bb & 0x0F, so = sb & 0x0F;
    if (so && (sb & 0x40)) return 0;
    if (ao && (ab & 0x40)) return 1;
    if (bo && (bb & 0x40)) return 2;
    if (so) return 0;
    if (ao) return 1;
    if (bo) return 2;
    return -1;
}

static inline uint32_t compose_tex(uint32_t a, uint32_t b, uint32_t s, uint32_t ta, uint32_t tb, uint32_t ts, uint16_t backdrop, bool sh) {
    a = with_tex(a, ta); b = with_tex(b, tb); s = with_tex(s, ts);
    int top = top_layer(a, b, s);
    uint32_t t = top == 0 ? ts : top == 1 ? ta : top == 2 ? tb : 0;
    return t >= 0xFF000000u ? t & 0xFFFFFF : vdp_compose(a, b, s, backdrop, sh);
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
        bool tex = hd_texpack && tex_cap;
        const uint32_t *tsrow = tex ? (sprites == sprite_layer ? sprite_tex_layer : tex_layer[2]) + (size_t)y * hd_w : NULL;
        if (!ox_a) {
            const uint32_t *rb = scaled[1] + (size_t)y * hd_w;
            if (tex) {
                const uint32_t *ta = tex_layer[0] + (size_t)y * hd_w, *tb = tex_layer[1] + (size_t)y * hd_w;
                for (int x = 0; x < hd_w; x++) out[x] = compose_tex(ra0[x], rb[x], rs[x], ta[x], tb[x], tsrow[x], backdrop, sh);
            } else {
                for (int x = 0; x < hd_w; x++) out[x] = vdp_compose(ra0[x], rb[x], rs[x], backdrop, sh);
            }
        } else if (tex) {
            int ya = y + oy_a[line], yb = y + oy_b[line];
            ya = ya < 0 ? 0 : ya >= hd_h ? hd_h - 1 : ya;
            yb = yb < 0 ? 0 : yb >= hd_h ? hd_h - 1 : yb;
            const uint32_t *ra = scaled[0] + (size_t)ya * hd_w, *rb = scaled[1] + (size_t)yb * hd_w;
            const uint32_t *ta0 = tex_layer[0] + (size_t)y * hd_w, *ta = tex_layer[0] + (size_t)ya * hd_w, *tb = tex_layer[1] + (size_t)yb * hd_w;
            int dxa = ox_a[line], dxb = ox_b[line];
            for (int x = 0; x < hd_w; x++) {
                int xa = x + dxa, xb = x + dxb;
                xa = xa < 0 ? 0 : xa >= hd_w ? hd_w - 1 : xa;
                xb = xb < 0 ? 0 : xb >= hd_w ? hd_w - 1 : xb;
                bool window = (ra0[x] >> 16) & HD_WINDOW;
                out[x] = compose_tex(window ? ra0[x] : ra[xa], rb[xb], rs[x], window ? ta0[x] : ta[xa], tb[xb], tsrow[x], backdrop, sh);
            }
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

/* ---- Lighting ------------------------------------------------------------------------ *
 * The bright and the strongly coloured pixels (fire, magic, gems, the HUD's gold) are
 * gathered into a picture an eighth the size, blurred at two sizes, and laid over the
 * picture with a screen blend, so they glow into their surroundings. The glow is made
 * once per game frame, not for every in-between picture. */
typedef struct { const uint32_t *src; uint32_t *dst; int w, h, pitch, y0, y1; } PostJob;

static float *bloom, *bloom_tmp;
static int bloom_w, bloom_h, bloom_f;
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

static void make_bloom(const uint32_t *src, int w, int h, int pitch, int level) {
    int f = w / 160 > 1 ? w / 160 : 1;
    int bw = (w + f - 1) / f, bh = (h + f - 1) / f;
    if (bw != bloom_w || bh != bloom_h || f != bloom_f) {
        free(bloom); free(bloom_tmp);
        bloom = malloc(sizeof(float) * 3 * 2 * (size_t)bw * bh);
        bloom_tmp = malloc(sizeof(float) * (size_t)(bw > bh ? bw : bh) * 2);
        bloom_w = bw; bloom_h = bh; bloom_f = f;
    }
    float threshold = level >= 2 ? .74f : .8f;
    int step = f / 3 > 1 ? f / 3 : 1;          /* a few samples per cell are plenty for a blur */
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            float acc[3] = { 0, 0, 0 };
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
                    float glow = fmaxf((lum - threshold) / (1 - threshold), (vivid - .5f) / .5f) * (mx > .6f);
                    if (glow > 0) { acc[0] += r * glow; acc[1] += g * glow; acc[2] += b * glow; }
                    n++;
                }
            float *o = bloom + ((size_t)by * bw + bx) * 3;
            for (int c = 0; c < 3; c++) o[c] = n ? acc[c] / n : 0;
        }
    /* Two sizes of glow: a tight one and a wide one. */
    float *wide = bloom + (size_t)3 * bw * bh;
    memcpy(wide, bloom, sizeof(float) * 3 * (size_t)bw * bh);
    blur_bloom(bloom, 1);
    blur_bloom(wide, 5);
    for (size_t i = 0; i < (size_t)3 * bw * bh; i++) bloom[i] = bloom[i] * .6f + wide[i] * 1.1f;
    light_k = level >= 2 ? .75f : .45f;
}

static int *col_x0, *col_x1;            /* per picture column: the two glow cells it lies between */
static float *col_tx;
static int col_w, col_f;

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
        for (int x = 0; x < j->w; x++) {
            uint32_t p = s[x];
            float r = ((p >> 16) & 255) * (1 / 255.f), g = ((p >> 8) & 255) * (1 / 255.f), b = (p & 255) * (1 / 255.f);
            int x0 = col_x0[x], x1 = col_x1[x];
            float tx = col_tx[x];
            const float *a = bloom + ((size_t)y0 * bw + x0) * 3, *bb = bloom + ((size_t)y0 * bw + x1) * 3;
            const float *c = bloom + ((size_t)y1 * bw + x0) * 3, *d = bloom + ((size_t)y1 * bw + x1) * 3;
            float l[3];
            for (int k = 0; k < 3; k++) {
                float top = a[k] + (bb[k] - a[k]) * tx, bot = c[k] + (d[k] - c[k]) * tx;
                l[k] = fminf((top + (bot - top) * ty) * light_k, 1);
            }
            r = 1 - (1 - r) * (1 - l[0]); g = 1 - (1 - g) * (1 - l[1]); b = 1 - (1 - b) * (1 - l[2]);
            o[x] = (uint32_t)((int)(r * 255 + .5f) << 16 | (int)(g * 255 + .5f) << 8 | (int)(b * 255 + .5f));
        }
    }
    return 0;
}

static void post_part(void *ctx, int y0, int y1) {
    PostJob j = *(const PostJob *)ctx;
    j.y0 = y0; j.y1 = y1;
    post_rows(&j);
}

const uint32_t *hd_post(const uint32_t *src, int w, int h, int pitch, const HdPost *p) {
    static uint32_t *buf;
    static size_t cap;
    bool light = p->lighting > 0;
    bool world_fx = p->world && (p->elements || p->footsteps);
    if (light || world_fx) {
        size_t need = (size_t)w * h;
        if (need > cap) { free(buf); buf = malloc(need * 4); cap = buf ? need : 0; }
        if (buf) {
            if (light) {
                static unsigned made_for;
                static int made_w, made_level;
                if (made_for != hd_serial || made_w != w || made_level != p->lighting) {
                    make_bloom(src, w, h, pitch, p->lighting);   /* once per game frame */
                    made_for = hd_serial; made_w = w; made_level = p->lighting;
                }
                make_columns(w);
                PostJob pj = { src, buf, w, h, pitch, 0, 0 };
                run_bands(post_part, &pj, h);
            } else {
                for (int y = 0; y < h; y++) memcpy(buf + (size_t)y * w, src + (size_t)y * pitch, (size_t)w * 4);
            }
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
    static bool made_tex;
    if (sprites_gen == hd_serial && sprites_scale == scale && made_with == hd_scaler && made_tex == hd_texpack) return;
    made_with = hd_scaler;
    made_tex = hd_texpack;
    static uint32_t padded[34 * 34];
    for (int i = 0; i < 80; i++) {
        const HdSprite *c = &hd_frame.sprites[i];
        if (!c->drawn) continue;
        int pw = c->w + 2, ph = c->h + 2;
        memset(padded, 0, sizeof padded);
        for (int y = 0; y < c->h; y++) memcpy(padded + (y + 1) * pw + 1, c->px + y * 32, (size_t)c->w * 4);
        scale_any(padded, pw, pw, ph, scale, sprite_scaled[i], sprite_tmp);
        if (hd_texpack && sprite_tex)
            for (int yy = 0; yy < ph * scale; yy++)
                for (int xx = 0; xx < pw * scale; xx++) {
                    int nx = xx / scale - 1, ny = yy / scale - 1;
                    uint32_t ref = nx >= 0 && ny >= 0 && nx < c->w && ny < c->h ? c->ref[ny * 32 + nx] : 0;
                    sprite_tex[i][yy * pw * scale + xx] = texpack_texel(ref, xx % scale, yy % scale, scale);
                }
    }
    sprites_gen = hd_serial;
    sprites_scale = scale;
}

/* Sprites at their positions for moment t, back to front so earlier sprites win. */
static void draw_sprite_layer(int scale, float t) {
    memset(sprite_layer, 0, (size_t)hd_w * hd_h * 4);
    bool tex = hd_texpack && sprite_tex && sprite_tex_layer;
    if (tex) memset(sprite_tex_layer, 0, (size_t)hd_w * hd_h * 4);
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
                if (tex) sprite_tex_layer[(size_t)Y * hd_w + X] = sprite_tex[i][yy * sw + xx];
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
