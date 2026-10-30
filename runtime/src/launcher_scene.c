/* The launcher's scene. Everything is drawn in software into ui_px:
 *  - built once per canvas size: the dusk sky with the sun setting behind the dunes, the
 *    stone tablet with its gold trim, and the armlet and logo from the ROM at their size
 *    (with the light they cast);
 *  - every frame: twinkling stars, sand blowing across, embers rising from the armlet, a
 *    sheen sweeping over the gold, and the four spirits circling the armlet, the one of the
 *    open tab turning to the front.
 * The spirits' orbs and emblems (a drop, a flame, a leaf, a crescent) are drawn here from
 * shapes; nothing is taken from the game but the armlet and the title logo. */
#include "launcher_scene.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const Spirit spirits[5] = {
    { "DYTTO",  "Water",  { .37f, .80f, 1.0f }, { .04f, .20f, .48f } },
    { "EFREET", "Fire",   { 1.0f, .56f, .22f }, { .48f, .10f, .02f } },
    { "BOW",    "Plant",  { .58f, .90f, .40f }, { .08f, .30f, .07f } },
    { "SHADE",  "Shadow", { .74f, .58f, 1.0f }, { .17f, .06f, .36f } },
    { "ARMLET", "Gold",   { 1.0f, .82f, .36f }, { .40f, .26f, .04f } },
};

/* ---- Pixels ----------------------------------------------------------------------- */
static inline int clamp255(int v) { return v > 255 ? 255 : v; }

/* Adds light: colour c (0-1) times k. */
static inline void add_px(int x, int y, Rgb c, float k) {
    if (x < 0 || y < 0 || x >= ui_w || y >= ui_h || k <= 0) return;
    uint32_t *p = &ui_px[y * ui_w + x], d = *p;
    float kk = k * 255;
    *p = (uint32_t)(clamp255((int)((d >> 16) & 255) + (int)(c.r * kk)) << 16 | clamp255((int)((d >> 8) & 255) + (int)(c.g * kk)) << 8 |
                    clamp255((int)(d & 255) + (int)(c.b * kk)));
}

/* Blends packed colour c over the pixel with coverage a (0-256). */
static inline void over_packed(uint32_t *p, uint32_t c, int a) {
    uint32_t d = *p;
    int r = (int)((d >> 16) & 255), g = (int)((d >> 8) & 255), b = (int)(d & 255);
    r += (((int)((c >> 16) & 255) - r) * a) >> 8;
    g += (((int)((c >> 8) & 255) - g) * a) >> 8;
    b += (((int)(c & 255) - b) * a) >> 8;
    *p = (uint32_t)(r << 16 | g << 8 | b);
}

static inline void over_px(int x, int y, Rgb c, float a) {
    if (x < 0 || y < 0 || x >= ui_w || y >= ui_h || a <= 0) return;
    over_packed(&ui_px[y * ui_w + x], pack(c), (int)(clampf(a, 0, 1) * 256));
}

void scene_glow(float cx, float cy, float r, Rgb c, float k) {
    int px = (int)ui_x(cx), py = (int)ui_y(cy), pr = (int)(r * ui_s) + 1;
    float inv = 1.f / (r * ui_s);
    for (int y = py - pr; y <= py + pr; y++)
        for (int x = px - pr; x <= px + pr; x++) {
            float dx = (x - px) * inv, dy = (y - py) * inv, d2 = dx * dx + dy * dy;
            if (d2 >= 1) continue;
            float f = 1 - d2;
            add_px(x, y, c, k * f * f * f);
        }
}

/* ---- Spirits ------------------------------------------------------------------------ */
/* The emblem's shape at p (about -1..1, y down); true inside. */
static bool emblem(int spirit, float x, float y, float t) {
    switch (spirit) {
    case 0: {                           /* a drop of water */
        float cy = .22f;
        if (x * x + (y - cy) * (y - cy) < .5f * .5f) return true;
        if (y < cy && y > -.9f) return fabsf(x) < .5f * (y + .9f) / (cy + .9f) * sqrtf(fmaxf(0, 1 - (cy - y) * .1f));
        return false;
    }
    case 1: {                           /* a flame, flickering */
        float sway = .16f * sinf(t * 5 + y * 3.2f) * (.7f - y) / 1.6f;
        float xx = x - sway;
        if (y < -.95f || y > .78f) return false;
        float h = (y + .95f) / 1.73f;   /* 0 at the tip, 1 at the base */
        float w = .58f * powf(h, .65f);
        if (y > .4f) w *= sqrtf(fmaxf(0, 1 - (y - .4f) * (y - .4f) / (.38f * .38f)));
        bool outer = fabsf(xx) < w;
        bool inner = fabsf(xx + sway * .5f) < w * .38f && y > .05f && y < .62f;
        return outer && !inner;
    }
    case 2: {                           /* a leaf with its vein */
        float c = .766f, s = .643f;     /* turned 40 degrees */
        float u = x * c - y * s, v = x * s + y * c;
        bool lens = (u - .42f) * (u - .42f) + v * v < .74f * .74f && (u + .42f) * (u + .42f) + v * v < .74f * .74f;
        bool vein = fabsf(u) < .045f && fabsf(v) < .6f;
        bool stem = fabsf(u) < .05f && v > .6f && v < .92f;
        return (lens && !vein) || stem;
    }
    default: {                          /* a crescent moon */
        bool disc = x * x + y * y < .72f * .72f;
        bool bite = (x - .34f) * (x - .34f) + (y + .2f) * (y + .2f) < .6f * .6f;
        return disc && !bite;
    }
    }
}

void scene_orb(int spirit, float cx, float cy, float r, float t, float bright) {
    const Spirit *sp = &spirits[spirit];
    scene_glow(cx, cy, r * 2.8f, sp->light, .32f * bright);
    int px = (int)ui_x(cx), py = (int)ui_y(cy);
    float pr = r * ui_s;
    int R = (int)pr + 2;
    for (int y = -R; y <= R; y++)
        for (int x = -R; x <= R; x++) {
            float dx = x / pr, dy = y / pr, d2 = dx * dx + dy * dy;
            if (d2 > 1.1f) continue;
            float d = sqrtf(d2);
            float cov = clampf((1 - d) * pr + .5f, 0, 1);
            if (cov <= 0) continue;
            /* A glass sphere: deep colour at the rim, light inside, a highlight top left. */
            Rgb c = mix(sp->light, sp->deep, smooth(.1f, 1, d));
            float hx = dx + .38f, hy = dy + .42f;
            c = add(c, rgb(1, 1, 1), .55f * expf(-(hx * hx + hy * hy) * 14));
            c = add(c, sp->light, .5f * smooth(.75f, 1, d));        /* rim light */
            /* The emblem, supersampled 3x3 for smooth edges. */
            int in = 0;
            for (int s = 0; s < 9; s++) {
                float ex = (x + (s % 3 - 1) * .33f) / pr / .62f, ey = (y + (s / 3 - 1) * .33f) / pr / .62f;
                in += emblem(spirit, ex, ey, t);
            }
            c = mix(c, mix(rgb(1, .98f, .92f), sp->light, .25f), in / 9.f * .92f);
            c = scale(c, .55f + .45f * bright);
            over_px(px + x, py + y, c, cov);
        }
}
