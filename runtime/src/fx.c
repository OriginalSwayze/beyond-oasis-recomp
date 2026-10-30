/* Weather and elemental particles.
 *
 * Elements: each game frame the sprites are looked at pixel by pixel. Those of the player
 * character and the high-priority ones (the HUD) are left alone; in the others, bright
 * and vivid pixels of four colours give off particles: fire colours embers that rise and
 * fade, light blue and cyan drops that splash up and fall, violet wisps that drift, bright
 * green leaves that flutter down. That is what the spirits (Efreet, Dytto, Shade, Bow), the
 * magic and the flames in the game are drawn with. Particles live in world coordinates
 * (screen plus the game's camera), so they stay where they were made as the camera moves.
 *
 * Weather, only in the game world: Breeze (pollen and slow shafts of light), Sandstorm
 * (streaks of sand and a warm haze), Rain (falling streaks, splashes on the ground, a
 * cooler and darker light), Mist (soft banks of fog drifting through the world). */
#include "fx.h"

#include <math.h>
#include <string.h>

#include "settings.h"
#include "genesis.h"

enum { EMBER, DROP, WISP, LEAF, DUST, GRASS, RING };

typedef struct { float x, y, vx, vy; short life, age; uint8_t kind, r, g, b; } Particle;

#define MAX_PARTICLES 600
static Particle parts[MAX_PARTICLES];
static int nparts;
static uint32_t rng = 12345;
static float last_cam_x, last_cam_y;
static bool have_cam;

static inline uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static inline float frand(void) { return (next() & 0xFFFFFF) / 16777216.f; }
static inline uint32_t hash(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }

void fx_clear(void) { nparts = 0; have_cam = false; }

static int kind_of(uint16_t c) {
    int r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
    if (r >= 6 && g >= 2 && g <= 6 && b <= 2) return EMBER;
    if (b >= 7 && g >= 5 && r <= 3) return DROP;
    if (b >= 6 && r >= 4 && g <= 3) return WISP;
    if (g >= 6 && r <= 4 && b <= 3) return LEAF;
    return -1;
}

static void spawn(int kind, float x, float y) {
    if (nparts >= MAX_PARTICLES) return;
    Particle *p = &parts[nparts++];
    p->x = x; p->y = y; p->kind = (uint8_t)kind; p->age = 0;
    switch (kind) {
    case EMBER: p->vx = (frand() - .5f) * .5f; p->vy = -.5f - frand() * .7f; p->life = (short)(40 + next() % 30); break;
    case DROP:  p->vx = (frand() - .5f) * 1.2f; p->vy = -.8f - frand() * .6f; p->life = (short)(30 + next() % 20); break;
    case WISP:  p->vx = (frand() - .5f) * .4f; p->vy = -.2f - frand() * .3f; p->life = (short)(60 + next() % 40); break;
    case LEAF:  p->vx = (frand() - .5f) * .6f; p->vy = .2f + frand() * .3f; p->life = (short)(70 + next() % 40); break;
    case DUST:  p->vx = (frand() - .5f) * .5f; p->vy = -.15f - frand() * .15f; p->life = (short)(22 + next() % 14); break;
    case GRASS: p->vx = (frand() - .5f) * .9f; p->vy = -.7f - frand() * .5f; p->life = (short)(16 + next() % 8); break;
    default:    p->vx = 0; p->vy = 0; p->life = 18; break;
    }
}

/* ---- Footsteps ---------------------------------------------------------------------------- *
 * Where Ali walks (his sprites' box, from the renderer), the ground under his feet decides
 * what he kicks up: water splashes (a ring and drops), grass gives bits of green that hop
 * away, anything else a puff of dust in the ground's own colour. */
static float feet_x, feet_y;
static bool have_feet;
static int step_clock;

static uint32_t ground_at(const HdFrame *f, int x, int y) {
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return 0;
    uint32_t a = f->a[y][x], b = f->b[y][x];
    bool ao = (a >> 16) & 0x0F, ah = (a >> 16) & 0x40, bo = (b >> 16) & 0x0F, bh = (b >> 16) & 0x40;
    uint32_t top = ao && (ah || !(bo && bh)) ? a : bo ? b : 0;
    return top ? vdp_color_rgb((uint16_t)top) : 0;
}

static void footsteps(const HdFrame *f, float cam_x, float cam_y) {
    if (!vdp_player_w) { have_feet = false; return; }
    float sx = vdp_player_x + vdp_player_w / 2.f + f->extra, sy = vdp_player_y + vdp_player_h - 3.f;
    float wx = sx + cam_x, wy = sy + cam_y;
    bool moving = have_feet && fabsf(wx - feet_x) + fabsf(wy - feet_y) > .4f && fabsf(wx - feet_x) + fabsf(wy - feet_y) < 12;
    feet_x = wx; feet_y = wy; have_feet = true;
    if (!moving || ++step_clock % 7) return;
    uint32_t c = ground_at(f, (int)sx, (int)sy + 2);
    if (!c) return;
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    int kind = b > r + 40 && b > g + 10 ? RING : g > r + 25 && g > b + 25 ? GRASS : DUST;
    int n = kind == RING ? 1 : 3;
    for (int k = 0; k < n; k++) {
        if (nparts >= MAX_PARTICLES) return;
        spawn(kind, wx + (frand() - .5f) * 6, wy + (frand() - .5f) * 2);
        Particle *p = &parts[nparts - 1];
        p->r = (uint8_t)(r + (255 - r) / 3); p->g = (uint8_t)(g + (255 - g) / 3); p->b = (uint8_t)(b + (255 - b) / 3);
    }
    if (kind == RING) for (int k = 0; k < 3; k++) spawn(DROP, wx + (frand() - .5f) * 6, wy);
}

void fx_game_frame(const HdFrame *f, bool world, float cam_x, float cam_y) {
    /* A jump of the camera is a new scene: the old particles do not belong in it. */
    if (have_cam && (fabsf(cam_x - last_cam_x) > 64 || fabsf(cam_y - last_cam_y) > 64)) nparts = 0;
    last_cam_x = cam_x; last_cam_y = cam_y; have_cam = true;
    /* Move on, and drop the old. */
    int n = 0;
    for (int i = 0; i < nparts; i++) {
        Particle *p = &parts[i];
        if (++p->age >= p->life) continue;
        switch (p->kind) {
        case EMBER: p->vx += (frand() - .5f) * .08f; break;
        case DROP:  p->vy += .07f; break;
        case WISP:  p->vx += sinf(p->age * .15f + i) * .02f; break;
        case LEAF:  p->vx = sinf(p->age * .12f + i) * .6f; break;
        case GRASS: p->vy += .09f; break;
        case DUST:  p->vx *= .94f; p->vy *= .96f; break;
        default: break;
        }
        p->x += p->vx; p->y += p->vy;
        parts[n++] = *p;
    }
    nparts = n;
    if (settings.footsteps && world) footsteps(f, cam_x, cam_y);
    if (!settings.elements || !world) return;
    for (int i = 0; i < 80; i++) {
        const HdSprite *s = &f->sprites[i];
        if (!s->drawn || s->player || (s->attr & 0x8000)) continue;
        /* Count each kind's pixels, keeping one at random of each to spawn from. */
        int count[4] = { 0, 0, 0, 0 }, px[4] = { 0 }, py[4] = { 0 };
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++) {
                uint32_t v = s->px[y * 32 + x];
                if (!((v >> 16) & 0x0F)) continue;
                int k = kind_of((uint16_t)v);
                if (k < 0) continue;
                if (next() % (uint32_t)(++count[k]) == 0) { px[k] = x; py[k] = y; }
            }
        for (int k = 0; k < 4; k++) {
            if (count[k] < 10) continue;    /* a few bright pixels are a detail, not an element */
            int spawns = count[k] > 60 ? 2 : 1;
            for (int j = 0; j < spawns; j++)
                if (frand() < .6f) spawn(k, s->x + px[k] + (frand() - .5f) * 4 + f->extra + cam_x, s->y + py[k] + cam_y);
        }
    }
}

/* ---- Drawing ------------------------------------------------------------------------------ */
static inline void blend(uint32_t *q, int r, int g, int b, int a) {   /* a: 0-256 */
    uint32_t d = *q;
    int dr = (int)((d >> 16) & 255), dg = (int)((d >> 8) & 255), db = (int)(d & 255);
    dr += (r - dr) * a >> 8; dg += (g - dg) * a >> 8; db += (b - db) * a >> 8;
    *q = (uint32_t)(dr << 16 | dg << 8 | db);
}

static inline void glow(uint32_t *q, int r, int g, int b, int a) {    /* additive, a: 0-256 */
    uint32_t d = *q;
    int dr = (int)((d >> 16) & 255) + (r * a >> 8), dg = (int)((d >> 8) & 255) + (g * a >> 8), db = (int)(d & 255) + (b * a >> 8);
    *q = (uint32_t)((dr > 255 ? 255 : dr) << 16 | (dg > 255 ? 255 : dg) << 8 | (db > 255 ? 255 : db));
}

/* A soft round dot at (cx, cy) picture pixels, radius r. */
static void dot(uint32_t *px, int w, int h, float cx, float cy, float r, int cr, int cg, int cb, float alpha, bool additive) {
    int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1), y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
    if (x1 < 0 || y1 < 0 || x0 >= w || y0 >= h) return;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < h; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < w; x++) {
            float dx = (x + .5f - cx) / r, dy = (y + .5f - cy) / r, d2 = dx * dx + dy * dy;
            if (d2 >= 1) continue;
            int a = (int)((1 - d2) * alpha * 256);
            if (additive) glow(&px[(size_t)y * w + x], cr, cg, cb, a);
            else blend(&px[(size_t)y * w + x], cr, cg, cb, a);
        }
}

/* A short soft line, drawn as dots along it. */
static void streak(uint32_t *px, int w, int h, float x0, float y0, float x1, float y1, float r, int cr, int cg, int cb, float alpha) {
    float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    int n = (int)(len / (r > 1 ? r : 1)) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        dot(px, w, h, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r, cr, cg, cb, alpha * (.4f + .6f * t), false);
    }
}

/* Value noise that tiles every 256 world pixels, for mist. */
static float noise2(float x, float y) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = (hash((uint32_t)(ix & 15) * 97u + (uint32_t)(iy & 15) * 7919u) & 255) / 255.f;
    float b = (hash((uint32_t)((ix + 1) & 15) * 97u + (uint32_t)(iy & 15) * 7919u) & 255) / 255.f;
    float c = (hash((uint32_t)(ix & 15) * 97u + (uint32_t)((iy + 1) & 15) * 7919u) & 255) / 255.f;
    float d = (hash((uint32_t)((ix + 1) & 15) * 97u + (uint32_t)((iy + 1) & 15) * 7919u) & 255) / 255.f;
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

static void tint(uint32_t *px, int w, int h, int r, int g, int b, int a, int dark) {   /* toward a colour, and darker */
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t d = px[i];
        int dr = (int)((d >> 16) & 255), dg = (int)((d >> 8) & 255), db = (int)(d & 255);
        dr += (r - dr) * a >> 8; dg += (g - dg) * a >> 8; db += (b - db) * a >> 8;
        dr = dr * (256 - dark) >> 8; dg = dg * (256 - dark) >> 8; db = db * (256 - dark) >> 8;
        px[i] = (uint32_t)(dr << 16 | dg << 8 | db);
    }
}

static void draw_weather(uint32_t *px, int w, int h, const HdPost *p) {
    float s = (float)p->scale, t = p->t;
    float gw = w / s, gh = h / s;             /* the picture in game pixels */
    switch (p->weather) {
    case WEATHER_BREEZE: {
        /* Shafts of light leaning from the top left, drifting slowly. */
        /* Worked out once per game pixel: the shafts change slowly across the picture. */
        int S = p->scale;
        for (int gy = 0; gy * S < h; gy++)
            for (int gx = 0; gx * S < w; gx++) {
                float u = (gx + p->cam_x * .3f + gy * .55f) * .02f + t * .03f;
                float band = sinf(u) * sinf(u * .37f + 1.3f);
                if (band <= .55f) continue;
                int a = (int)((band - .55f) * 70);
                for (int y = gy * S; y < gy * S + S && y < h; y++)
                    for (int x = gx * S; x < gx * S + S && x < w; x++) glow(&px[(size_t)y * w + x], 255, 236, 190, a);
            }
        for (int i = 0; i < 90; i++) {
            uint32_t hh = hash((uint32_t)i * 2654435761u);
            float wx = (hh & 511) + t * (9 + (hh >> 9 & 7)) + 8 * sinf(t * .8f + i), wy = ((hh >> 12) & 511) + 6 * sinf(t * .6f + i);
            float sx = fmodf(wx - p->cam_x, 512), sy = fmodf(wy - p->cam_y, 512);
            if (sx < 0) sx += 512;
            if (sy < 0) sy += 512;
            for (float ox = sx - 64; ox < gw; ox += 512)
                for (float oy = sy - 64; oy < gh; oy += 512)
                    dot(px, w, h, ox * s, oy * s, s * 1.1f, 240, 240, 170, .55f, false);
        }
        break;
    }
    case WEATHER_SAND: {
        tint(px, w, h, 226, 190, 130, 34, 0);
        for (int i = 0; i < 260; i++) {
            uint32_t hh = hash((uint32_t)i * 2246822519u);
            float speed = 110 + (hh & 63);
            float wx = fmodf((hh & 1023) + t * speed - p->cam_x, 640), wy = fmodf(((hh >> 10) & 1023) + t * 8 - p->cam_y, 400);
            if (wx < 0) wx += 640;
            if (wy < 0) wy += 400;
            for (float ox = wx - 80; ox < gw; ox += 640)
                for (float oy = wy - 40; oy < gh; oy += 400)
                    streak(px, w, h, ox * s, oy * s, (ox + 7) * s, (oy + 1.2f) * s, s * .55f, 236, 206, 150, .45f);
        }
        break;
    }
    case WEATHER_RAIN: {
        tint(px, w, h, 120, 140, 170, 30, 26);
        for (int i = 0; i < 240; i++) {
            uint32_t hh = hash((uint32_t)i * 3266489917u);
            float speed = 260 + (hh & 127);
            float sx = fmodf((hh & 1023) - t * speed * .25f - p->cam_x * .5f, 600), sy = fmodf(((hh >> 10) & 1023) + t * speed - p->cam_y * .5f, 400);
            if (sx < 0) sx += 600;
            if (sy < 0) sy += 400;
            for (float ox = sx - 60; ox < gw + 20; ox += 600)
                for (float oy = sy - 40; oy < gh; oy += 400)
                    streak(px, w, h, ox * s, oy * s, (ox - 2.5f) * s, (oy + 10) * s, s * .4f, 200, 210, 235, .5f);
        }
        /* Splashes on the ground: small rings at places fixed in the world. */
        for (int i = 0; i < 40; i++) {
            uint32_t hh = hash((uint32_t)i * 668265263u + (uint32_t)(t * 6));
            float wx = (float)(hh & 511), wy = (float)((hh >> 9) & 511);
            float sx = fmodf(wx - p->cam_x, 512), sy = fmodf(wy - p->cam_y, 512);
            if (sx < 0) sx += 512;
            if (sy < 0) sy += 512;
            float age = fmodf(t * 6, 1);
            for (float ox = sx - 32; ox < gw; ox += 512)
                for (float oy = sy - 32; oy < gh; oy += 512)
                    dot(px, w, h, ox * s, oy * s, s * (1 + age * 2), 220, 230, 245, .35f * (1 - age), false);
        }
        break;
    }
    case WEATHER_MIST: {
        int S = p->scale;
        for (int gy = 0; gy * S < h; gy++) {
            float wy = (gy + p->cam_y) / 48;
            for (int gx = 0; gx * S < w; gx++) {
                float wx = (gx + p->cam_x) / 64 + t * .04f;
                float n = noise2(wx, wy + t * .01f) * .6f + noise2(wx * 2.1f + 7, wy * 2.1f) * .4f;
                if (n <= .45f) continue;
                int a = (int)((n - .45f) * 150);
                for (int y = gy * S; y < gy * S + S && y < h; y++)
                    for (int x = gx * S; x < gx * S + S && x < w; x++) blend(&px[(size_t)y * w + x], 226, 230, 236, a);
            }
        }
        break;
    }
    }
}

static void draw_particles(uint32_t *px, int w, int h, const HdPost *p) {
    float s = (float)p->scale;
    for (int i = 0; i < nparts; i++) {
        const Particle *q = &parts[i];
        float life = 1 - (float)q->age / q->life;
        float x = (q->x - p->cam_x) * s, y = (q->y - p->cam_y) * s;    /* made from picture positions: the sides are in */
        switch (q->kind) {
        case EMBER: dot(px, w, h, x, y, s * 1.3f, 255, (int)(120 + 100 * life), 40, .9f * life, true); break;
        case DROP:  dot(px, w, h, x, y, s * 1.0f, 150, 220, 255, .8f * life, true); break;
        case WISP:  dot(px, w, h, x, y, s * 2.4f, 170, 110, 255, .35f * life, true); break;
        case LEAF:  dot(px, w, h, x, y, s * 1.2f, 120, 220, 90, .85f * (life > .2f ? 1 : life * 5), false); break;
        case DUST:  dot(px, w, h, x, y, s * (2 + 4 * (1 - life)), q->r, q->g, q->b, .65f * life, false); break;
        case GRASS: dot(px, w, h, x, y, s * .9f, 90, 200, 70, .9f * life, false); break;
        default: {                      /* a ring on the water, widening */
            float rr = s * (2 + 7 * (1 - life));
            for (int a = 0; a < 24; a++) {
                float ang = a * .2618f;
                dot(px, w, h, x + cosf(ang) * rr, y + sinf(ang) * rr * .45f, s * .6f, 220, 235, 255, .5f * life, false);
            }
        }
        }
    }
}

void fx_draw(uint32_t *px, int w, int h, const HdPost *p) {
    if (!p->world) return;
    if (p->weather) draw_weather(px, w, h, p);
    if (nparts) draw_particles(px, w, h, p);
}
