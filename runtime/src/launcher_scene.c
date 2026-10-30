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

const Spirit spirits[4] = {
    { "DYTTO",  "Water",  { .37f, .80f, 1.0f }, { .04f, .20f, .48f } },
    { "EFREET", "Fire",   { 1.0f, .56f, .22f }, { .48f, .10f, .02f } },
    { "BOW",    "Plant",  { .58f, .90f, .40f }, { .08f, .30f, .07f } },
    { "SHADE",  "Shadow", { .74f, .58f, 1.0f }, { .17f, .06f, .36f } },
};

#define ARM_CX 318                      /* the armlet's centre and width */
#define ARM_CY 420
#define ARM_W  372
#define LOGO_W 430
#define LOGO_Y 36
#define SUN_X  318
#define SUN_Y  548

static uint32_t *backdrop;              /* sky, dunes and tablet */
static uint8_t *arm_light;              /* the armlet's glow on the scene, in its box */
static int light_x0, light_y0, light_w, light_h;
/* Art at its size on screen: colour (already toned), coverage and brightness per pixel. */
typedef struct { int x0, y0, w, h; uint32_t *px; uint8_t *a, *lum; } Sprite;
static Sprite arm, logo_s;

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

/* ---- Static parts ------------------------------------------------------------------ */
static float dune(float x, int layer) {
    float base[3] = { 520, 566, 624 };
    float a = sinf(x * .0062f + layer * 1.7f) * (22 - layer * 4) + sinf(x * .017f + layer * 3.1f) * (9 - layer * 2) +
              sinf(x * .0021f + layer) * 30;
    return base[layer] + a;
}

/* How the art is toned: the armlet's lime-yellow warmed toward gold, the logo as gold foil. */
enum { TONE_WARM, TONE_GOLD };

static Rgb tone(Rgb c, float lum, int how) {
    if (how == TONE_GOLD) {             /* dark bronze, rich gold, pale highlight */
        Rgb g = mix(rgb_hex(0x3A2006), rgb_hex(0xE0A93E), smooth(.05f, .55f, lum));
        return mix(g, rgb_hex(0xFFF0C2), smooth(.55f, .95f, lum));
    }
    if (c.r > c.b + .15f && c.g > c.b + .1f) c = mix(c, mix(rgb_hex(0x4A2E0A), rgb_hex(0xFFE3A0), lum), .45f);
    return c;
}

static void scale_art(Sprite *s, const Art *a, float cx, float top, float width, int how) {
    free(s->px); free(s->a); free(s->lum);
    memset(s, 0, sizeof *s);
    if (!a->w) return;
    float k = width / a->w;
    s->x0 = (int)ui_x(cx - width / 2);
    s->y0 = (int)ui_y(top);
    s->w = (int)(width * ui_s);
    s->h = (int)(a->h * k * ui_s);
    s->px = calloc((size_t)s->w * s->h, 4);
    s->a = calloc((size_t)s->w * s->h, 1);
    s->lum = calloc((size_t)s->w * s->h, 1);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            /* Bilinear, weighting colours by the mask so edges don't pick up the background. */
            float fx = (x + .5f) * a->w / s->w - .5f, fy = (y + .5f) * a->h / s->h - .5f;
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
            int i = y * s->w + x;
            if (cov > 0) cc = scale(cc, 1 / cov);
            float lum = cc.r * .3f + cc.g * .55f + cc.b * .15f;
            s->a[i] = (uint8_t)(clampf(cov * 1.6f - .3f, 0, 1) * 255);
            s->px[i] = pack(tone(cc, lum, how));
            s->lum[i] = (uint8_t)(clampf(lum, 0, 1) * 255);
        }
}

static void build_backdrop(void) {
    free(backdrop);
    backdrop = malloc((size_t)ui_w * ui_h * 4);
    for (int y = 0; y < ui_h; y++)
        for (int x = 0; x < ui_w; x++) {
            float ux = (x - ui_ox) / ui_s, uy = (y - ui_oy) / ui_s;
            /* Dusk: deep indigo overhead, violet, then amber at the horizon. */
            float v = clampf(uy / 560, 0, 1);
            Rgb c = mix(rgb_hex(0x07071A), rgb_hex(0x2B1640), smooth(0, .6f, v));
            c = mix(c, rgb_hex(0x8A3B3A), smooth(.55f, .9f, v));
            c = mix(c, rgb_hex(0xF0A050), smooth(.86f, 1.0f, v));
            /* The setting sun behind the armlet, and its haze. */
            float sx = ux - SUN_X, sy = uy - SUN_Y, sd = sqrtf(sx * sx + sy * sy * 1.6f);
            c = add(c, rgb(1, .62f, .28f), .55f * expf(-sd * sd / (2 * 170.f * 170.f)));
            c = add(c, rgb(1, .85f, .55f), .7f * expf(-sd * sd / (2 * 48.f * 48.f)));
            /* Dunes, far to near, lit from the sun on their crests. */
            static const uint32_t dune_c[3] = { 0x4A2638, 0x2C1626, 0x170C16 };
            for (int l = 0; l < 3; l++) {
                float top = dune(ux, l);
                if (uy < top) continue;
                Rgb d = rgb_hex(dune_c[l]);
                float crest = expf(-(uy - top) * .12f) * (l == 0 ? .5f : .35f);
                float sun = expf(-(ux - SUN_X) * (ux - SUN_X) / (2 * 260.f * 260.f));
                c = add(d, rgb(1, .55f, .28f), crest * (.3f + .7f * sun));
            }
            float dither = (hashf((uint32_t)(y * ui_w + x)) - .5f) / 255.f;
            backdrop[y * ui_w + x] = pack(add(c, rgb(1, 1, 1), dither));
        }
    scene_tablet(backdrop, TABLET_X, TABLET_Y, TABLET_W, TABLET_H, .96f);
}

void scene_tablet(uint32_t *dst, float tx, float ty, float tw, float th, float opacity) {
    int x0 = (int)ui_x(tx - 4), x1 = (int)ui_x(tx + tw + 40), y0 = (int)ui_y(ty - 4), y1 = (int)ui_y(ty + th + 48);
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > ui_w ? ui_w : x1; y1 = y1 > ui_h ? ui_h : y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float ux = (x - ui_ox) / ui_s, uy = (y - ui_oy) / ui_s;
            Rgb c = unpack(dst[y * ui_w + x]);
            /* Dark sandstone with a soft shadow under it. */
            float td = ui_rrect_dist(ux, uy, tx, ty, tw, th, 14);
            float shadow = ui_rrect_dist(ux, uy, tx + 8, ty + 16, tw, th, 14);
            c = scale(c, 1 - .55f * (1 - smooth(-4, 30, shadow)));
            if (td < .5f) {
                float grain = hashf((uint32_t)((int)(ux * 1.3f) * 7919 + (int)(uy * 1.3f) * 104729)) - .5f;
                float strata = sinf(uy * .09f + sinf(ux * .013f) * 2.2f) * .5f + .5f;
                Rgb stone = mix(rgb_hex(0x1C1712), rgb_hex(0x2A2219), strata * .6f + .2f);
                stone = add(stone, rgb(1, .9f, .7f), grain * .025f);
                /* Warmer near the top, where the sun's light reaches. */
                stone = add(stone, rgb(.9f, .5f, .2f), .06f * (1 - smooth(ty, ty + 300, uy)));
                float cov = clampf(.5f - td * ui_s, 0, 1);
                c = mix(c, stone, cov * opacity);
                /* Gold trim: an outer line, an inner one, and the bevel between. */
                float line1 = 1 - smooth(.5f, 1.6f, fabsf(td + 4));
                float line2 = 1 - smooth(.3f, 1.1f, fabsf(td + 11));
                float bevel = smooth(-3, 0, td) * .3f;
                Rgb gold = mix(rgb_hex(0x6B4A1A), rgb_hex(0xE9C46A), clampf(1 - (uy - ty) / th * .6f, 0, 1));
                c = mix(c, gold, line1 * .9f + line2 * .6f);
                c = add(c, rgb(1, .8f, .5f), bevel * cov * .2f);
            }
            float dither = (hashf((uint32_t)(y * ui_w + x)) - .5f) / 255.f;
            dst[y * ui_w + x] = pack(add(c, rgb(1, 1, 1), dither));
        }
    /* Ornaments at the corners: small gold diamonds. */
    float corners[4][2] = { { tx + 11, ty + 11 }, { tx + tw - 11, ty + 11 }, { tx + 11, ty + th - 11 }, { tx + tw - 11, ty + th - 11 } };
    for (int k = 0; k < 4; k++) {
        int px = (int)ui_x(corners[k][0]), py = (int)ui_y(corners[k][1]), r = (int)(7 * ui_s);
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++) {
                float d = (fabsf((float)x) + fabsf((float)y)) / r;
                if (d > 1 || px + x < 0 || py + y < 0 || px + x >= ui_w || py + y >= ui_h) continue;
                Rgb g = mix(rgb_hex(0xF6DC8E), rgb_hex(0x8A6224), d);
                uint32_t *p = &dst[(py + y) * ui_w + px + x];
                *p = pack(mix(unpack(*p), g, clampf((1 - d) * 3, 0, 1)));
            }
    }
}

void scene_layout(const Art *armlet, const Art *logo) {
    build_backdrop();
    float arm_h = armlet->w ? armlet->h * (float)ARM_W / armlet->w : 0;
    scale_art(&arm, armlet, ARM_CX, ARM_CY - arm_h / 2, ARM_W, TONE_WARM);
    scale_art(&logo_s, logo, ARM_CX, LOGO_Y, LOGO_W, TONE_GOLD);
    /* The armlet's light on everything around it: a blurred copy of its shape. */
    float *glow = calloc((size_t)ui_w * ui_h, sizeof(float));
    int r = (int)(26 * ui_s);
    for (int y = 0; y < arm.h; y++)
        for (int x = 0; x < arm.w; x++) {
            float a = arm.a[y * arm.w + x] / 255.f * arm.lum[y * arm.w + x] / 255.f;
            if (a < .05f) continue;
            int X = arm.x0 + x, Y = arm.y0 + y;
            if (X >= 0 && Y >= 0 && X < ui_w && Y < ui_h) glow[Y * ui_w + X] = a;
        }
    float *tmp = calloc((size_t)ui_w * ui_h, sizeof(float));
    int x0 = arm.x0 - 3 * r, x1 = arm.x0 + arm.w + 3 * r, y0 = arm.y0 - 3 * r, y1 = arm.y0 + arm.h + 3 * r;
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > ui_w ? ui_w : x1; y1 = y1 > ui_h ? ui_h : y1;
    for (int pass = 0; pass < 3; pass++) {      /* three box blurs: close to a gaussian */
        for (int y = y0; y < y1; y++) {
            float s = 0;
            for (int x = x0; x < x1; x++) {
                s += glow[y * ui_w + x];
                if (x - x0 >= 2 * r + 1) s -= glow[y * ui_w + x - 2 * r - 1];
                if (x - r >= x0) tmp[y * ui_w + x - r] = s / (2 * r + 1);
            }
        }
        for (int x = x0; x < x1; x++) {
            float s = 0;
            for (int y = y0; y < y1; y++) {
                s += tmp[y * ui_w + x];
                if (y - y0 >= 2 * r + 1) s -= tmp[(y - 2 * r - 1) * ui_w + x];
                if (y - r >= y0) glow[(y - r) * ui_w + x] = s / (2 * r + 1);
            }
        }
    }
    free(tmp);
    free(arm_light);
    light_x0 = x0; light_y0 = y0; light_w = x1 > x0 ? x1 - x0 : 0; light_h = y1 > y0 ? y1 - y0 : 0;
    arm_light = calloc((size_t)light_w * light_h + 1, 1);
    for (int y = 0; y < light_h; y++)
        for (int x = 0; x < light_w; x++) arm_light[y * light_w + x] = (uint8_t)(clampf(glow[(y + y0) * ui_w + x + x0], 0, 1) * 255);
    free(glow);
}

/* ---- Animated parts ---------------------------------------------------------------- */
static void draw_stars(float t) {
    for (int i = 0; i < 260; i++) {
        float x = hashf((uint32_t)i * 2654435761u) * UI_BASE_W, y = hashf((uint32_t)i * 40503u + 7) * 330;
        if (x > TABLET_X - 4 && x < TABLET_X + TABLET_W + 4 && y > TABLET_Y - 4) continue;
        float tw = .5f + .5f * sinf(t * (1.2f + 2 * hashf((uint32_t)i * 977u)) + i);
        float b = (.25f + .75f * hashf((uint32_t)i * 31u)) * tw * (1 - y / 380);
        int px = (int)ui_x(x), py = (int)ui_y(y);
        add_px(px, py, rgb(.9f, .92f, 1), b * .9f);
        if (b > .6f && ui_s > 1.2f) {
            add_px(px + 1, py, rgb(.9f, .92f, 1), b * .3f); add_px(px - 1, py, rgb(.9f, .92f, 1), b * .3f);
            add_px(px, py + 1, rgb(.9f, .92f, 1), b * .3f); add_px(px, py - 1, rgb(.9f, .92f, 1), b * .3f);
        }
    }
}

static void dot(float x, float y, float r, Rgb c, float k) {
    if (r * ui_s < 1.2f) { add_px((int)ui_x(x), (int)ui_y(y), c, k); return; }
    scene_glow(x, y, r, c, k);
}

/* Sand carried across by the wind, low over the dunes. */
static void draw_sand(float t) {
    for (int i = 0; i < 180; i++) {
        float speed = 30 + 70 * hashf((uint32_t)i * 7u + 1);
        float x = fmodf(hashf((uint32_t)i * 13u) * 1500 + t * speed, 1500) - 110;
        float y = 470 + hashf((uint32_t)i * 17u + 3) * 250 + sinf(t * .8f + i) * 6;
        if (x > TABLET_X && x < TABLET_X + TABLET_W && y > TABLET_Y && y < TABLET_Y + TABLET_H) continue;
        float k = .12f + .25f * hashf((uint32_t)i * 19u);
        dot(x, y, .9f + hashf((uint32_t)i * 23u), rgb(1, .78f, .5f), k);
    }
}

/* Embers rising from the armlet and fading out. */
static void draw_embers(float t) {
    for (int i = 0; i < 46; i++) {
        float life = 4 + 3 * hashf((uint32_t)i * 29u);
        float age = fmodf(t + hashf((uint32_t)i * 31u) * life, life) / life;
        float x = ARM_CX + (hashf((uint32_t)i * 37u) - .5f) * ARM_W * .9f + sinf(t * 1.3f + i) * 10 * age;
        float y = ARM_CY + 60 - age * 300;
        float k = sinf(age * 3.14159f) * (.35f + .4f * hashf((uint32_t)i * 41u));
        dot(x, y, 2.2f, rgb(1, .62f, .25f), k);
    }
}

/* A sprite drawn over the scene, with a sheen sweeping across it every few seconds. */
static void draw_sprite(const Sprite *s, float t, float sheen_k) {
    if (!s->w) return;
    float phase = fmodf(t * .22f, 1.6f) - .3f;  /* the sheen's position, 0-1 across */
    for (int y = 0; y < s->h; y++) {
        int Y = s->y0 + y;
        if (Y < 0 || Y >= ui_h) continue;
        float uy = (float)y / s->h * .35f - phase;
        for (int x = 0; x < s->w; x++) {
            int i = y * s->w + x, a = s->a[i];
            int X = s->x0 + x;
            if (!a || X < 0 || X >= ui_w) continue;
            uint32_t c = s->px[i];
            float u = (float)x / s->w + uy;
            if (u > -.12f && u < .12f) {
                float q = 1 - u * u * 70;   /* a soft band, close to exp(-u^2 * 260) */
                int sh = (int)(q * q * sheen_k * (.3f + s->lum[i] / 255.f) * 255);
                c = (uint32_t)(clamp255((int)((c >> 16) & 255) + sh) << 16 | clamp255((int)((c >> 8) & 255) + sh * 242 / 255) << 8 |
                               clamp255((int)(c & 255) + sh * 204 / 255));
            }
            over_packed(&ui_px[Y * ui_w + X], c, a + (a >> 7));
        }
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

/* The four spirits on a tilted circle around the armlet; `focus` turns to the front. */
static void orb_at(int k, float focus, float t, float *x, float *y, float *depth) {
    float ang = (k - focus) * 1.5708f + 1.5708f + sinf(t * .4f) * .05f;  /* front = bottom of the circle */
    *x = ARM_CX + cosf(ang) * 262;
    *y = ARM_CY + 18 + sinf(ang) * 84 + sinf(t * 1.1f + k * 1.7f) * 6;
    *depth = sinf(ang);                 /* -1 behind the armlet, 1 in front */
}

static void draw_orbs(float t, float focus, float glow, bool front) {
    int order[4] = { 0, 1, 2, 3 };
    float depth[4], ox[4], oy[4];
    for (int k = 0; k < 4; k++) orb_at(k, focus, t, &ox[k], &oy[k], &depth[k]);
    for (int a = 1; a < 4; a++)          /* back to front */
        for (int b = a; b > 0 && depth[order[b]] < depth[order[b - 1]]; b--) { int s = order[b]; order[b] = order[b - 1]; order[b - 1] = s; }
    for (int n = 0; n < 4; n++) {
        int k = order[n];
        if ((depth[k] >= 0) != front) continue;
        float near = (depth[k] + 1) / 2;
        float dist = fabsf(remainderf(k - focus, 4));
        float chosen = clampf(1 - dist, 0, 1);
        float r = 16 + 10 * near + 8 * chosen;
        float bright = .45f + .35f * near + .3f * chosen * glow;
        /* A trail of sparks behind each spirit as it circles. */
        for (int s = 1; s <= 10; s++) {
            float tx, ty, td;
            orb_at(k, focus, t - s * .05f, &tx, &ty, &td);
            float jx = sinf(t * 7 + s * 2.1f + k) * 5, jy = cosf(t * 6 + s * 1.7f + k) * 5;
            dot(tx + jx - (ox[k] - tx) * s * .9f, ty + jy - (oy[k] - ty) * s * .9f, 1.6f, spirits[k].light, .35f * (1 - s / 11.f) * bright);
        }
        scene_orb(k, ox[k], oy[k], r, t, bright);
    }
}

void scene_draw(float t, float focus, float glow) {
    memcpy(ui_px, backdrop, (size_t)ui_w * ui_h * 4);
    draw_stars(t);
    draw_sand(t);
    /* The armlet's light breathes slowly, tinted by the spirit in front. */
    int fi = (int)lroundf(focus) & 3;
    Rgb tint = mix(rgb(1, .72f, .32f), spirits[fi].light, .35f);
    float breathe = .55f + .2f * sinf(t * 1.3f);
    int tr = (int)(tint.r * breathe * 256), tg = (int)(tint.g * breathe * 256), tb = (int)(tint.b * breathe * 256);
    for (int y = 0; y < light_h; y++) {
        const uint8_t *l = arm_light + (size_t)y * light_w;
        uint32_t *row = ui_px + (size_t)(y + light_y0) * ui_w + light_x0;
        for (int x = 0; x < light_w; x++) {
            int v = l[x];
            if (!v) continue;
            uint32_t d = row[x];
            row[x] = (uint32_t)(clamp255((int)((d >> 16) & 255) + ((v * tr) >> 8)) << 16 |
                                clamp255((int)((d >> 8) & 255) + ((v * tg) >> 8)) << 8 | clamp255((int)(d & 255) + ((v * tb) >> 8)));
        }
    }
   
    draw_orbs(t, focus, glow, false);
    draw_sprite(&arm, t, .55f);
    draw_embers(t);
    draw_orbs(t, focus, glow, true);
    draw_sprite(&logo_s, t + 2.2f, .5f);
}
