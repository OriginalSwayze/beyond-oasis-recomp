/* Soft additive light for the launcher, the pause menu and the foil surfaces: the glow of
 * the tabs, the highlight, the Play button, the stars and the embers. */
#include "launcher_scene.h"


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
