/* High internal resolution: each layer of the frame (plane A with the window, plane B,
 * sprites) is scaled on its own with Scale2x / Scale3x (4x = 2x twice), then the layers
 * are composited at that size with vdp_compose(), like the VDP does at 1x.
 *
 * Scale2x/3x pick, for every output pixel, one whole source pixel of the 3x3
 * neighbourhood, so colours, priorities and transparency are never blended: edges get
 * rounded and diagonals smoothed, and layers stay separate. Mid-frame palette changes
 * and line scrolls are kept because every line was captured with its own colours. */
#include "hd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

HdFrame hd_frame;
uint32_t *hd_px;
int hd_w, hd_h;
unsigned hd_serial = 1;
int hd_split;
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

/* Scales w x h pixels (row pitch `pitch`) into dst (w*scale wide, no padding). */
static void scale_any(const uint32_t *src, int pitch, int w, int h, int scale, uint32_t *dst, uint32_t *tmp) {
    if (scale == 1) for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, src + (size_t)y * pitch, (size_t)w * 4);
    else if (scale == 3) scale3x(src, pitch, w, h, dst);
    else if (scale == 4) { scale2x(src, pitch, w, h, tmp); scale2x(tmp, w * 2, w * 2, h * 2, dst); }
    else scale2x(src, pitch, w, h, dst);
}

/* ---- Motion ----------------------------------------------------------------------- *
 * The game moves things every other frame (its logic runs at 30 Hz). The in-between
 * frames glide from the previous motion update to the latest one over the time between
 * them, so motion is continuous at 60 Hz and above, one update (about 33 ms) late. */
typedef struct {
    int w, h;
    uint8_t flags[HD_MAX_H];
    int16_t hscroll[2][HD_MAX_H], vscroll[2][HD_MAX_H];
    struct { bool drawn; int x, y, w, h; uint16_t attr; } spr[80];
} Motion;

static Motion before, latest;           /* the previous and the latest motion update */
static bool have_before, have_latest;
static long frame_no, latest_at;        /* frames run, and when the latest update came */
static int period = 2;                  /* frames between updates */

static unsigned scaled_gen, sprites_gen;
static int scaled_scale, sprites_scale;
static uint32_t *sprite_layer;          /* sprites drawn at their in-between positions */

/* Each sprite of hd_frame scaled once per frame, with a transparent border of one pixel
 * so the scaler can round its outer edges. */
#define SPR_PAD_MAX ((32 + 2) * HD_MAX_SCALE)
static uint32_t sprite_scaled[80][SPR_PAD_MAX * SPR_PAD_MAX];
static uint32_t sprite_tmp[SPR_PAD_MAX * SPR_PAD_MAX];
static int match[80];                   /* hd_frame sprite -> `before` sprite, or -1 */

static void motion_of(Motion *m, const HdFrame *f) {
    m->w = f->w; m->h = f->h;
    memcpy(m->flags, f->flags, sizeof m->flags);
    memcpy(m->hscroll, f->hscroll, sizeof m->hscroll);
    memcpy(m->vscroll, f->vscroll, sizeof m->vscroll);
    for (int i = 0; i < 80; i++) {
        const HdSprite *s = &f->sprites[i];
        m->spr[i].drawn = s->drawn;
        m->spr[i].x = s->x; m->spr[i].y = s->y; m->spr[i].w = s->w; m->spr[i].h = s->h; m->spr[i].attr = s->attr;
    }
}

static bool moved(const Motion *a, const Motion *b) {
    if (a->w != b->w || a->h != b->h) return true;
    if (memcmp(a->hscroll, b->hscroll, sizeof a->hscroll) || memcmp(a->vscroll, b->vscroll, sizeof a->vscroll)) return true;
    for (int i = 0; i < 80; i++) {
        if (a->spr[i].drawn != b->spr[i].drawn) return true;
        if (a->spr[i].drawn && (a->spr[i].x != b->spr[i].x || a->spr[i].y != b->spr[i].y)) return true;
    }
    return false;
}

void hd_frame_done(void) {
    static Motion now;
    frame_no++;
    motion_of(&now, &hd_frame);
    if (!have_latest) { latest = now; have_latest = true; latest_at = frame_no; return; }
    if (!moved(&now, &latest)) return;
    long gap = frame_no - latest_at;
    if (gap >= 1 && gap <= 4) period = (int)gap;
    before = latest;
    have_before = gap <= 8;             /* after a long still moment, start from here */
    latest = now;
    latest_at = frame_no;
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

static Job pool_job[16];
static SDL_sem *pool_go[16], *pool_done;
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
    while (pool_size < n - 1 && pool_size < 15 && pool_done) {
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

static int workers(void) {
    static int n;
    if (!n) { n = SDL_GetCPUCount(); n = n < 1 ? 1 : n > 16 ? 16 : n; }
    return n;
}

typedef struct { const uint32_t *src; int scale; uint32_t *dst, *tmp; } ScaleJob;

static int scale_job(void *arg) {
    ScaleJob *j = arg;
    scale_any(j->src, HD_MAX_W, hd_frame.w, hd_frame.h, j->scale, j->dst, j->tmp);
    return 0;
}

static void scale_planes(int scale) {
    if (scaled_gen == hd_serial && scaled_scale == scale) return;
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
        if (mask) {
            memset(out, 0, (size_t)mask * 4);
            memset(out + hd_w - mask, 0, (size_t)mask * 4);
        }
    }
    return 0;
}

static void compose_rows(int scale, const uint32_t *sprites, const int *ox_a, const int *oy_a,
                         const int *ox_b, const int *oy_b) {
    ComposeJob cj[16];
    Job jobs[16];
    int n = workers();
    for (int k = 0; k < n; k++) {
        cj[k] = (ComposeJob){ scale, hd_h * k / n, hd_h * (k + 1) / n, sprites, ox_a, oy_a, ox_b, oy_b };
        jobs[k] = (Job){ compose_band, &cj[k] };
    }
    run_jobs(jobs, n);
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

/* Pairs each sprite with the same sprite at the previous update: same tiles and size,
 * nearest, at most 32 pixels away. Unpaired sprites (new, gone, animation frame
 * changed) are drawn where they are now. */
static void match_sprites(void) {
    bool used[80] = { false };
    for (int i = 0; i < 80; i++) {
        match[i] = -1;
        const HdSprite *c = &hd_frame.sprites[i];
        if (!c->drawn) continue;
        int best = -1, best_d = 33 * 33 * 2;
        for (int k = 0; k < 80; k++) {
            if (!before.spr[k].drawn || used[k] || before.spr[k].attr != c->attr ||
                before.spr[k].w != c->w || before.spr[k].h != c->h) continue;
            int dx = c->x - before.spr[k].x, dy = c->y - before.spr[k].y, d = dx * dx + dy * dy;
            if (k == i) d -= 1;         /* prefer the same SAT entry when equally near */
            if (d < best_d) { best_d = d; best = k; }
        }
        if (best >= 0 && best_d <= 32 * 32 * 2) { match[i] = best; used[best] = true; }
    }
}

static void scale_sprites(int scale) {
    if (sprites_gen == hd_serial && sprites_scale == scale) return;
    static uint32_t padded[34 * 34];
    for (int i = 0; i < 80; i++) {
        const HdSprite *c = &hd_frame.sprites[i];
        if (!c->drawn) continue;
        int pw = c->w + 2, ph = c->h + 2;
        memset(padded, 0, sizeof padded);
        for (int y = 0; y < c->h; y++) memcpy(padded + (y + 1) * pw + 1, c->px + y * 32, (size_t)c->w * 4);
        scale_any(padded, pw, pw, ph, scale, sprite_scaled[i], sprite_tmp);
    }
    match_sprites();
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
    /* How far each sprite moved since the previous update. A character is several
     * sprites; when its animation changes, some pieces get new tiles and cannot be
     * paired. Those take the movement of the nearest paired piece, so the character
     * glides as one instead of coming apart for a frame. */
    static int mx[80], my[80];
    static bool moved_known[80];
    for (int i = 0; i < 80; i++) {
        moved_known[i] = match[i] >= 0;
        if (moved_known[i]) {
            mx[i] = hd_frame.sprites[i].x - before.spr[match[i]].x;
            my[i] = hd_frame.sprites[i].y - before.spr[match[i]].y;
        }
    }
    /* A character's pieces touch and sit next to each other in the sprite list, so an
     * unpaired piece first looks for a touching paired piece close in the list, then for
     * the nearest paired piece. */
    for (int i = 0; i < 80; i++) {
        const HdSprite *c = &hd_frame.sprites[i];
        if (!c->drawn || moved_known[i]) continue;
        int best = -1, best_score = 1 << 30;
        for (int k = 0; k < 80; k++) {
            if (!moved_known[k]) continue;
            const HdSprite *o = &hd_frame.sprites[k];
            int dx = (o->x + o->w / 2) - (c->x + c->w / 2), dy = (o->y + o->h / 2) - (c->y + c->h / 2);
            int d2 = dx * dx + dy * dy;
            if (d2 > 40 * 40) continue;
            bool touch = o->x <= c->x + c->w && c->x <= o->x + o->w && o->y <= c->y + c->h && c->y <= o->y + o->h;
            int score = touch ? abs(o->rank - c->rank) : 1000 + d2;
            if (score < best_score) { best_score = score; best = k; }
        }
        mx[i] = best >= 0 ? mx[best] : 0;
        my[i] = best >= 0 ? my[best] : 0;
    }
    /* For checks: the largest difference in movement between touching pieces next to each
     * other in the sprite list (one character); above 0 it would come apart in between. */
    hd_split = 0;
    for (int i = 0; i < 80; i++)
        for (int k = 0; k < 80; k++) {
            const HdSprite *p = &hd_frame.sprites[i], *q = &hd_frame.sprites[k];
            if (!p->drawn || !q->drawn || q->rank != p->rank + 1) continue;
            bool touch = p->x <= q->x + q->w && q->x <= p->x + p->w && p->y <= q->y + q->h && q->y <= p->y + p->h;
            int d = abs(mx[i] - mx[k]) + abs(my[i] - my[k]);
            if (touch && d > hd_split) hd_split = d;
        }
    float back = 1 - t;
    for (int k = 0; k < n; k++) {
        int i = order[k];
        const HdSprite *c = &hd_frame.sprites[i];
        float x = c->x - mx[i] * back, y = c->y - my[i] * back;
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

/* Draws the moment t between the previous motion update (0) and the latest (1), with
 * the newest frame's content. */
static double ms_since(Uint64 *t0) {
    Uint64 now = SDL_GetPerformanceCounter();
    double ms = (double)(now - *t0) * 1000 / SDL_GetPerformanceFrequency();
    *t0 = now;
    return ms;
}

static void compose_between(int scale, float t) {
    Uint64 t0 = SDL_GetPerformanceCounter();
    scale_planes(scale);
    hd_ms[0] = ms_since(&t0);
    static int ox[2][HD_MAX_H], oy[2][HD_MAX_H];
    float back = 1 - t;
    for (int line = 0; line < hd_frame.h; line++) {
        bool still = (hd_frame.flags[line] & HD_LINE_COLUMN_VSCROLL) || !(before.flags[line] & HD_LINE_DISPLAY);
        for (int p = 0; p < 2; p++) {
            int dh = scroll_delta(hd_frame.hscroll[p][line], before.hscroll[p][line]);
            int dv = scroll_delta(hd_frame.vscroll[p][line], before.vscroll[p][line]);
            if (still || abs(dh) > 24 || abs(dv) > 24) dh = dv = 0;    /* a cut, not a scroll */
            ox[p][line] = (int)lroundf(back * dh * scale);
            oy[p][line] = (int)lroundf(-back * dv * scale);
        }
    }
    const uint32_t *sprites = scaled[2];
    hd_ms[1] = hd_ms[2] = 0;
    if (!hd_frame.sprite_mask) {
        ms_since(&t0);
        scale_sprites(scale);
        hd_ms[1] = ms_since(&t0);
        draw_sprite_layer(scale, t);
        hd_ms[2] = ms_since(&t0);
        sprites = sprite_layer;
    }
    ms_since(&t0);
    compose_rows(scale, sprites, ox[0], oy[0], ox[1], oy[1]);
    hd_ms[3] = ms_since(&t0);
}

float hd_motion_t(float frac) {
    if (!have_before) return 1;
    float t = (frame_no + frac - latest_at) / (float)period;
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

void hd_compose_at(int scale, float frac) {
    scale = clamp_scale(scale);
    if (!ensure(scale)) return;
    /* Draw every picture the same way, also when nothing is moving (t = 1): switching
     * between whole-layer and per-sprite drawing would flicker where sprites overlap. */
    if (!have_before || before.w != hd_frame.w || before.h != hd_frame.h) { hd_compose(scale); return; }
    compose_between(scale, hd_motion_t(frac));
}
