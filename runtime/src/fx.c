/* Elemental particles and footsteps.
 *
 * Elements: each game frame the sprites of the spirits and their magic are looked at pixel
 * by pixel (not Ali's, the enemies', the people's, the items' or the HUD's: motion.c tells
 * which object drew each sprite); bright and vivid pixels of four colours give off particles: fire
 * colours embers that rise and fade, light blue and cyan drops that splash up and fall,
 * violet wisps that drift, bright green leaves that flutter down. Particles live in world
 * coordinates (screen plus the game's camera), so they stay where they were made. */
#include "fx.h"

#include <math.h>
#include <string.h>

#include "settings.h"
#include "genesis.h"
#include "motion.h"

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

void fx_clear(void) { nparts = 0; have_cam = false; }

bool fx_stress;

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
    return top ? vdp_color_raw((uint16_t)top) : 0;
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

/* ---- Objects: who drew each sprite ------------------------------------------------------- *
 * motion.c knows the object behind every sprite. Items are told apart from people by how
 * they are drawn: an object of the main list (the characters, from $FF19E8; not Ali) that
 * draws a single sprite of at most 16x16, is not one of the people or creatures (types $0032,
 * $003C: villagers, birds, the lizard) and has stayed in place for a third of a second. */
#define MAIN_LIST 0xFF19E8
#define MAIN_END  (0xFF19E8 + 21 * 0xBC)
/* The spirit's slots of the main list: a summon waits for slot 1 ($FF1AA4) to be free;
 * the enemies and people start at slot 4. */
#define SPIRIT_END (0xFF19E8 + 4 * 0xBC)

typedef struct { uint32_t obj; float wx, wy; int still; bool seen; } Track;
static Track tracks[24];

static Track *track_of(uint32_t obj) {
    Track *free_slot = NULL;
    for (int k = 0; k < 24; k++) {
        if (tracks[k].obj == obj) return &tracks[k];
        if (!tracks[k].obj && !free_slot) free_slot = &tracks[k];
    }
    if (!free_slot) return NULL;
    *free_slot = (Track){ obj, 0, 0, -1, false };
    return free_slot;
}

/* Marks the dropped items among the sprites (they give no spirit particles). */
static void find_items(HdFrame *f, float cam_x, float cam_y) {
    uint32_t objs[80];
    int pieces[80];
    for (int k = 0; k < 24; k++) tracks[k].seen = false;
    for (int i = 0; i < 80; i++) {
        objs[i] = 0;
        uint16_t kind;
        if (f->sprites[i].drawn) motion_entry_owner(i, &objs[i], &kind);
    }
    for (int i = 0; i < 80; i++) {
        pieces[i] = 0;
        if (!objs[i]) continue;
        for (int k = 0; k < 80; k++) pieces[i] += objs[k] == objs[i];
    }
    for (int i = 0; i < 80; i++) {
        HdSprite *s = &f->sprites[i];
        uint32_t obj = objs[i];
        if (!obj || s->player || obj < MAIN_LIST || obj >= MAIN_END || obj == MAIN_LIST) continue;
        if (pieces[i] != 1 || s->w > 16 || s->h > 16) continue;
        uint16_t kind = (uint16_t)(bus.ram[obj & 0xFFFF] << 8 | bus.ram[(obj + 1) & 0xFFFF]);
        if (kind == 0x0032 || kind == 0x003C) continue;  /* people and creatures (a bird on the path), not items */
        Track *t = track_of(obj);
        if (!t) continue;
        float wx = s->x + f->extra + s->w / 2.f + cam_x, wy = s->y + s->h + cam_y;
        t->still = t->still >= 0 && fabsf(wx - t->wx) < .5f && fabsf(wy - t->wy) < .5f ? t->still + 1 : 0;
        t->wx = wx; t->wy = wy; t->seen = true;
        if (t->still < 20) continue;            /* a third of a second in place */
        s->item = true;
    }
    for (int k = 0; k < 24; k++) if (!tracks[k].seen) tracks[k].obj = 0;   /* gone */
}

/* The sprites that may give off elemental particles: the spirits and their magic, not Ali,
 * the enemies, people, items or the HUD. */
bool fx_elemental(const HdFrame *f, int i) {
    const HdSprite *s = &f->sprites[i];
    if (!s->drawn || s->player || s->item || (s->attr & 0x8000)) return false;
    uint32_t obj;
    uint16_t kind;
    if (!motion_entry_owner(i, &obj, &kind)) return false;
    /* Of the main list only the spirit's slots: the rest are enemies, people and items. */
    if (obj >= MAIN_LIST && obj < MAIN_END) return obj >= MAIN_LIST + 0xBC && obj < SPIRIT_END;
    return true;
}

void fx_game_frame(HdFrame *f, bool world, float cam_x, float cam_y) {
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
    if (fx_stress && world)             /* testing: the screen kept full of every kind */
        while (nparts < MAX_PARTICLES)
            spawn((int)(next() % 7), cam_x + frand() * f->w, cam_y + frand() * f->h);
    if (settings.footsteps && world) footsteps(f, cam_x, cam_y);
    if (settings.elements && world) find_items(f, cam_x, cam_y);
    if (!settings.elements || !world) return;
    for (int i = 0; i < 80; i++) {
        const HdSprite *s = &f->sprites[i];
        if (!fx_elemental(f, i)) continue;
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

/* A soft round dot at (cx, cy) picture pixels, radius r, on rows ya to yb - 1 of the picture. */
static void dot(uint32_t *px, int w, int ya, int yb, float cx, float cy, float r, int cr, int cg, int cb, float alpha, bool additive) {
    int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1), y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
    if (x1 < 0 || y1 < ya || x0 >= w || y0 >= yb) return;
    float inv = 1 / r;
    for (int y = y0 < ya ? ya : y0; y <= y1 && y < yb; y++) {
        float dy = (y + .5f - cy) * inv, rest = 1 - dy * dy;
        if (rest <= 0) continue;
        /* Only the columns the circle covers on this row (with a pixel to spare: the test
         * below decides). */
        float half = sqrtf(rest) * r + 1;
        int xa = (int)(cx - half), xb = (int)(cx + half);
        if (xa < x0) xa = x0;
        if (xb > x1) xb = x1;
        uint32_t *row = px + (size_t)y * w;
        for (int x = xa < 0 ? 0 : xa; x <= xb && x < w; x++) {
            float dx = (x + .5f - cx) * inv, d2 = dx * dx + dy * dy;
            if (d2 >= 1) continue;
            int a = (int)((1 - d2) * alpha * 256);
            if (additive) glow(&row[x], cr, cg, cb, a);
            else blend(&row[x], cr, cg, cb, a);
        }
    }
}

typedef struct { uint32_t *px; int w; const HdPost *p; } Band;

/* Every particle, on rows ya to yb - 1: the picture is split into bands of rows over all
 * cores, each drawing the particles in the same order, so the result is the same. */
static void particle_band(void *ctx, int ya, int yb) {
    const Band *bd = ctx;
    uint32_t *px = bd->px;
    int w = bd->w;
    const HdPost *p = bd->p;
    float s = (float)p->scale;
    for (int i = 0; i < nparts; i++) {
        const Particle *q = &parts[i];
        float life = 1 - (float)q->age / q->life;
        float x = (q->x - p->cam_x) * s, y = (q->y - p->cam_y) * s;    /* made from picture positions: the sides are in */
        switch (q->kind) {
        case EMBER: dot(px, w, ya, yb, x, y, s * 1.3f, 255, (int)(120 + 100 * life), 40, .9f * life, true); break;
        case DROP:  dot(px, w, ya, yb, x, y, s * 1.0f, 150, 220, 255, .8f * life, true); break;
        case WISP:  dot(px, w, ya, yb, x, y, s * 2.4f, 170, 110, 255, .35f * life, true); break;
        case LEAF:  dot(px, w, ya, yb, x, y, s * 1.2f, 120, 220, 90, .85f * (life > .2f ? 1 : life * 5), false); break;
        case DUST:  dot(px, w, ya, yb, x, y, s * (2 + 4 * (1 - life)), q->r, q->g, q->b, .65f * life, false); break;
        case GRASS: dot(px, w, ya, yb, x, y, s * .9f, 90, 200, 70, .9f * life, false); break;
        default: {                      /* a ring on the water, widening */
            float rr = s * (2 + 7 * (1 - life));
            if (y + rr * .45f + s < ya - 1 || y - rr * .45f - s > yb + 1) break;
            for (int a = 0; a < 24; a++) {
                float ang = a * .2618f;
                dot(px, w, ya, yb, x + cosf(ang) * rr, y + sinf(ang) * rr * .45f, s * .6f, 220, 235, 255, .5f * life, false);
            }
        }
        }
    }
}

void fx_draw(uint32_t *px, int w, int h, const HdPost *p) {
    if (!p->world || !nparts) return;
    Band bd = { px, w, p };
    hd_rows(particle_band, &bd, h);
}
