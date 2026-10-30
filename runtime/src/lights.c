/* Dynamic lighting with ray-traced soft shadows.
 *
 * Lights: after each game frame, the sprites of the spirits and their magic (the same ones
 * that give off elemental particles: fx.c tells them apart from Ali, the people, items and
 * the HUD by the object that drew them) become lights where they have bright, vivid pixels,
 * one per object, in the colour of those pixels; they move with their object. In dark scenes
 * the background's bright fire colours (torches, braziers, lava) become flickering lights
 * too; in daylight they are not looked for, as the village's straw roofs are fire-coloured.
 *
 * Shadows: the characters' sprites make an occluder map at half the game's resolution. From
 * each light 256 rays are marched outward over it, cell by cell, each keeping how much light
 * gets through; the rays are then blurred into their neighbours, so the shadows behind the
 * characters have soft edges, and each cell around the light reads its ray between the two
 * nearest. Where a light is blocked the scene darkens a little, so shadows show in daylight
 * too.
 *
 * Darkness: the background's mean brightness tells a cave or a night from daylight; a dark
 * scene keeps less of its own light (eased over a second), so the lights stand out.
 *
 * The map is made once per game frame; hd_post lays it over each picture, interpolating
 * between cells. */
#include "lights.h"

#include <math.h>
#include <string.h>

#include "fx.h"
#include "motion.h"
#include "settings.h"
#include "genesis.h"

#define MAX_LIGHTS 12
#define RAYS 256
#define MAX_STEPS 56                    /* cells along a ray: the largest radius, 112 frame pixels */

typedef struct { float x, y, r, g, b, radius, strength; } Light;

static float map[LIGHT_MAP_H][LIGHT_MAP_W][4];
static uint8_t occ[LIGHT_MAP_H][LIGHT_MAP_W];       /* opaque character pixels in each cell, 0-4 */
static float trans[RAYS][MAX_STEPS];
static int map_w, map_h, nlights;
static bool ready;
static float ambient = 1;
static Light test_light;

void lights_clear(void) { ambient = 1; ready = false; }

void lights_test(float x, float y, float r, float g, float b, float radius) {
    test_light = (Light){ x, y, r, g, b, radius, 1 };
}

const float *lights_map(int *w, int *h) {
    if (!ready) return NULL;
    *w = map_w; *h = map_h;
    return &map[0][0][0];
}

int lights_count(void) { return nlights; }

static inline void rgb_of(uint32_t layer_px, float *r, float *g, float *b) {
    uint32_t c = vdp_color_rgb((uint16_t)layer_px);
    *r = ((c >> 16) & 255) / 255.f; *g = ((c >> 8) & 255) / 255.f; *b = (c & 255) / 255.f;
}

/* The background's top pixel at (x, y): plane A over plane B by priority, else the backdrop. */
static uint32_t back_at(const HdFrame *f, int x, int y) {
    uint32_t a = f->a[y][x], b = f->b[y][x];
    bool ao = (a >> 16) & 0x0F, ah = (a >> 16) & 0x40, bo = (b >> 16) & 0x0F, bh = (b >> 16) & 0x40;
    return ao && (ah || !(bo && bh)) ? a : bo ? b : f->backdrop[y];
}

/* How dark the scene is: 0 daylight, 1 a dark cave or night. */
static float darkness(const HdFrame *f) {
    float sum = 0;
    int n = 0;
    for (int y = 2; y < f->h; y += 4)
        for (int x = 2; x < f->w; x += 4) {
            float r, g, b;
            rgb_of(back_at(f, x, y), &r, &g, &b);
            sum += r * .3f + g * .55f + b * .15f;
            n++;
        }
    float lum = n ? sum / n : 1;
    float d = (.32f - lum) / .2f;           /* mean brightness .32 and up: daylight; .12 and down: dark */
    return d < 0 ? 0 : d > 1 ? 1 : d;
}

/* A sprite pixel that gives off light: vivid and bright, or near white. */
static inline bool glowing(float r, float g, float b) {
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
    return mx > .6f && ((mx - mn) * mx > .35f || r * .3f + g * .55f + b * .15f > .85f);
}

static void add_light(Light *lights, Light l) {
    if (nlights < MAX_LIGHTS) lights[nlights++] = l;
}

/* One light per object among the spirits' and their magic's sprites. */
static void sprite_lights(const HdFrame *f, Light *lights) {
    struct { uint32_t obj; float x, y, r, g, b; int n; } groups[MAX_LIGHTS];
    int ngroups = 0;
    for (int i = 0; i < 80; i++) {
        const HdSprite *s = &f->sprites[i];
        if (!fx_light_sprite(f, i)) continue;
        uint32_t obj;
        uint16_t kind;
        if (!motion_entry_owner(i, &obj, &kind)) continue;
        int g = 0;
        while (g < ngroups && groups[g].obj != obj) g++;
        if (g == ngroups) {
            if (ngroups == MAX_LIGHTS) continue;
            memset(&groups[g], 0, sizeof groups[g]);
            groups[g].obj = obj;
            ngroups++;
        }
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++) {
                uint32_t v = s->px[y * 32 + x];
                if (!v) continue;
                float r, gg, b;
                rgb_of(v, &r, &gg, &b);
                if (!glowing(r, gg, b)) continue;
                groups[g].x += s->x + f->extra + x + .5f; groups[g].y += s->y + y + .5f;
                groups[g].r += r; groups[g].g += gg; groups[g].b += b;
                groups[g].n++;
            }
    }
    for (int g = 0; g < ngroups; g++) {
        int n = groups[g].n;
        if (n < 4) continue;                /* a glint, not a light */
        float r = groups[g].r / n, gg = groups[g].g / n, b = groups[g].b / n;
        float mx = fmaxf(r, fmaxf(gg, fmaxf(b, .01f)));
        /* Its colour at full brightness, a little towards white. */
        r = r / mx * .75f + .25f; gg = gg / mx * .75f + .25f; b = b / mx * .75f + .25f;
        float radius = 36 + sqrtf((float)n) * 5;
        add_light(lights, (Light){ groups[g].x / n, groups[g].y / n, r, gg, b,
                                   radius > 100 ? 100 : radius, fminf(.55f + n / 90.f, 1) });
    }
}

/* In a dark scene: torches and fires of the background, as flickering lights. */
static void fire_lights(const HdFrame *f, Light *lights, float dark, float t) {
    enum { C = 8 };
    int cw = f->w / C, ch = f->h / C;
    static uint8_t fire[HD_MAX_H / C][HD_MAX_W / C];
    for (int cy = 0; cy < ch; cy++)
        for (int cx = 0; cx < cw; cx++) {
            int n = 0;
            for (int y = cy * C; y < cy * C + C; y += 2)
                for (int x = cx * C; x < cx * C + C; x += 2) {
                    float r, g, b;
                    rgb_of(back_at(f, x, y), &r, &g, &b);
                    n += r > .75f && g > .25f && g < .85f && b < .45f && r - b > .45f;
                }
            fire[cy][cx] = (uint8_t)n;
        }
    /* The cells with the most fire around them, at least 24 pixels apart. */
    for (int cy = 0; cy < ch; cy++)
        for (int cx = 0; cx < cw; cx++) {
            int n = fire[cy][cx];
            if (n < 4) continue;
            bool best = true;
            for (int dy = -1; dy <= 1 && best; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int x = cx + dx, y = cy + dy;
                    if ((dx || dy) && x >= 0 && y >= 0 && x < cw && y < ch &&
                        (fire[y][x] > n || (fire[y][x] == n && (dy < 0 || (dy == 0 && dx < 0))))) { best = false; break; }
                }
            if (!best) continue;
            float x = cx * C + C / 2.f, y = cy * C + C / 2.f;
            bool near = false;
            for (int k = 0; k < nlights; k++)
                if (fabsf(lights[k].x - x) < 24 && fabsf(lights[k].y - y) < 24) near = true;
            if (near) continue;
            /* Each fire flickers on its own: two slow waves and a quick one. */
            float ph = x * .13f + y * .29f;
            float flicker = .82f + .08f * sinf(t * 7.1f + ph) + .06f * sinf(t * 11.3f + ph * 2) + .04f * sinf(t * 23.f + ph * 3);
            add_light(lights, (Light){ x, y, 1, .62f, .3f, 52 + n * 2.f, dark * flicker * .85f });
        }
}

/* The characters' sprites (not the lights' own, not the HUD) as occluders. */
static void make_occluders(const HdFrame *f) {
    memset(occ, 0, sizeof occ);
    for (int i = 0; i < 80; i++) {
        const HdSprite *s = &f->sprites[i];
        if (!s->drawn || (s->attr & 0x8000) || fx_light_sprite(f, i)) continue;
        for (int y = 0; y < s->h; y++) {
            int fy = s->y + y;
            if (fy < 0 || fy >= f->h) continue;
            for (int x = 0; x < s->w; x++) {
                int fx = s->x + f->extra + x;
                if (fx < 0 || fx >= f->w || !s->px[y * 32 + x]) continue;
                uint8_t *o = &occ[fy / LIGHT_CELL][fx / LIGHT_CELL];
                if (*o < 4) (*o)++;
            }
        }
    }
}

/* atan2 to within a few hundredths of a degree's ray: plenty for 256 rays. */
static inline float fast_atan2(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y);
    float a = fminf(ax, ay) / (fmaxf(ax, ay) + 1e-9f), s = a * a;
    float r = ((-.0464964749f * s + .15931422f) * s - .327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

/* Marches the rays of one light over the occluder map, softens them, and adds the light
 * and its shadows to the map (light in rgb, shadow in [3], turned into a multiplier later). */
static void trace(const Light *l) {
    float cx = l->x / LIGHT_CELL, cy = l->y / LIGHT_CELL;    /* in cells */
    int steps = (int)(l->radius / LIGHT_CELL) + 1;
    if (steps > MAX_STEPS) steps = MAX_STEPS;
    for (int ray = 0; ray < RAYS; ray++) {
        float a = ray * (6.2831853f / RAYS), dx = cosf(a), dy = sinf(a);
        float through = 1;
        for (int d = 0; d < steps; d++) {
            trans[ray][d] = through;          /* the cell itself is lit: its own pixels face the light */
            int x = (int)(cx + dx * (d + .5f)), y = (int)(cy + dy * (d + .5f));
            if (d >= 2 && x >= 0 && y >= 0 && x < map_w && y < map_h && occ[y][x])
                through *= 1 - occ[y][x] * .11f;   /* a cell full of character lets 56% through */
        }
    }
    /* Soft edges: each ray takes in its neighbours (a box of 5, twice), more so far out,
     * where a light of some size leaves a wider half-shadow. */
    static float tmp[RAYS];
    for (int d = 1; d < steps; d++)
        for (int pass = 0; pass < 2; pass++) {
            int r = d < steps / 3 ? 1 : 2;
            for (int ray = 0; ray < RAYS; ray++) {
                float s = 0;
                for (int k = -r; k <= r; k++) s += trans[(ray + k) & (RAYS - 1)][d];
                tmp[ray] = s / (2 * r + 1);
            }
            for (int ray = 0; ray < RAYS; ray++) trans[ray][d] = tmp[ray];
        }
    float R = l->radius / LIGHT_CELL;
    int x0 = (int)(cx - R), x1 = (int)(cx + R) + 1, y0 = (int)(cy - R), y1 = (int)(cy + R) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > map_w) x1 = map_w;
    if (y1 > map_h) y1 = map_h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float ox = x + .5f - cx, oy = y + .5f - cy, d = sqrtf(ox * ox + oy * oy);
            if (d >= R) continue;
            float fall = 1 - d * d / (R * R);     /* round at the middle, soft at the edge */
            fall *= fall;
            float ra = fast_atan2(oy, ox) * (RAYS / 6.2831853f);
            if (ra < 0) ra += RAYS;
            int r0 = (int)ra, d0 = (int)d;
            float tr = ra - r0, td = d - d0;
            int r1 = (r0 + 1) & (RAYS - 1), d1 = d0 + 1 < steps ? d0 + 1 : d0;
            r0 &= RAYS - 1;
            float near = trans[r0][d0] + (trans[r1][d0] - trans[r0][d0]) * tr;
            float far = trans[r0][d1] + (trans[r1][d1] - trans[r0][d1]) * tr;
            float lit = near + (far - near) * td;
            float k = l->strength * fall;
            float *m = map[y][x];
            m[0] += l->r * k * lit; m[1] += l->g * k * lit; m[2] += l->b * k * lit;
            m[3] += k * (1 - lit);
        }
}

/* The map from these lights: their light, their shadows and the scene's ambient. */
static void build(const HdFrame *f, const Light *lights, float amb) {
    for (int y = 0; y < map_h; y++) memset(map[y], 0, sizeof(float) * 4 * (size_t)map_w);
    if (nlights) make_occluders(f);
    for (int k = 0; k < nlights; k++) trace(&lights[k]);
    /* The shadow amounts become what each cell keeps of its own light. */
    for (int y = 0; y < map_h; y++)
        for (int x = 0; x < map_w; x++) {
            float *m = map[y][x];
            m[3] = amb * (1 - fminf(m[3] * .5f, .45f));
        }
    ready = true;
}

void lights_game_frame(const HdFrame *f, bool world, float t) {
    if (!settings.dynlight || !world) { ready = false; ambient = 1; nlights = 0; return; }
    map_w = (f->w + LIGHT_CELL - 1) / LIGHT_CELL;
    map_h = (f->h + LIGHT_CELL - 1) / LIGHT_CELL;
    float dark = darkness(f);
    float target = 1 - dark * .3f;
    ambient += (target - ambient) * .06f;   /* eased over about a second */

    Light lights[MAX_LIGHTS];
    nlights = 0;
    if (test_light.radius > 0) add_light(lights, test_light);
    sprite_lights(f, lights);
    if (dark > .3f) fire_lights(f, lights, dark, t);
    /* In daylight the sun outshines them: a light shows a little, in the dark fully. */
    for (int k = 0; k < nlights; k++) lights[k].strength *= .3f + .7f * dark;

    if (!nlights && ambient > .995f) { ready = false; return; }
    build(f, lights, ambient);
}

void lights_preview(const HdFrame *f, float x, float y) {
    map_w = (f->w + LIGHT_CELL - 1) / LIGHT_CELL;
    map_h = (f->h + LIGHT_CELL - 1) / LIGHT_CELL;
    Light l = { x, y, 1, .66f, .34f, 90, .7f };
    nlights = 1;
    build(f, &l, 1);
}
