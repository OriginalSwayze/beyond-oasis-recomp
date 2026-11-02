/* Software UI drawing shared by the launcher and the in-game menu (ui.c).
 * Layouts are written for a 1280x720 space; the canvas maps it to pixels with a scale
 * and a centering offset, so any window size or aspect works. */
#ifndef UI_H
#define UI_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "text.h"

#define UI_BASE_W 1280
#define UI_BASE_H 720

/* ---- Colors and small math ---------------------------------------------------- */
typedef struct { float r, g, b; } Rgb;

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float smooth(float e0, float e1, float x) { float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }
static inline uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }
static inline float hashf(uint32_t x) { return (hash32(x) & 0xFFFFFF) / 16777216.0f; }

static inline Rgb rgb(float r, float g, float b) { return (Rgb){ r, g, b }; }
static inline Rgb rgb_hex(uint32_t c) { return rgb(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f); }
static inline Rgb mix(Rgb a, Rgb b, float t) { return rgb(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t); }
static inline Rgb add(Rgb a, Rgb b, float k) { return rgb(a.r + b.r * k, a.g + b.g * k, a.b + b.b * k); }
static inline Rgb scale(Rgb a, float k) { return rgb(a.r * k, a.g * k, a.b * k); }
static inline uint32_t pack(Rgb c) {
    int r = (int)(clampf(c.r, 0, 1) * 255 + .5f), g = (int)(clampf(c.g, 0, 1) * 255 + .5f), b = (int)(clampf(c.b, 0, 1) * 255 + .5f);
    return (uint32_t)(r << 16 | g << 8 | b);
}
static inline Rgb unpack(uint32_t c) { return rgb_hex(c); }
static inline Rgb hue(float h) {        /* saturated rainbow */
    h = h - floorf(h);
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
    return rgb(clampf(r, 0, 1), clampf(g, 0, 1), clampf(b, 0, 1));
}

/* ---- Canvas -------------------------------------------------------------------- */
extern uint32_t *ui_px;                 /* 0x00RRGGBB, ui_w * ui_h */
extern int ui_w, ui_h;
extern float ui_s, ui_ox, ui_oy;        /* pixel = offset + layout * scale */

/* Sizes the canvas to w x h pixels and fits the 1280x720 layout inside, centered. */
void  ui_begin(int w, int h);
float ui_x(float layout_x);             /* layout -> pixel */
float ui_y(float layout_y);
void  ui_blend_px(int x, int y, Rgb c, float a);   /* pixel coordinates */
void  ui_fill_rect(float x, float y, float w, float h, Rgb c, float a);
/* Signed distance to a rounded rectangle (negative inside), in the units given. */
float ui_rrect_dist(float px, float py, float x, float y, float w, float h, float r);
/* Fills a rounded rectangle with a vertical gradient and an optional 1-unit border. */
void  ui_panel(float x, float y, float w, float h, float r, Rgb top, Rgb bottom, float alpha, Rgb border, float border_alpha);

/* ---- Text ---------------------------------------------------------------------- */
const TextMask *ui_text_get(const char *s, const char *face, float px, int weight, float tracking);
float ui_text_width(const char *s, const char *face, float px, int weight, float tracking);
/* Draws text with its top-left at (x, y), blending vertically from `top` to `bottom`. */
void  ui_draw_text(const char *s, const char *face, float px, int weight, float tracking,
                   float x, float y, Rgb top, Rgb bottom, float alpha);

/* ---- Widgets ------------------------------------------------------------------- */
/* A volume-style slider: a track from x to x + w centred on y, filled to value (0-1)
 * in gold with a round knob. ui_slider_value() turns a layout x into a value. */
void  ui_slider(float x, float y, float w, float value, bool focus);
float ui_slider_value(float x, float w, float mouse_x);

/* A button; `primary` ones are gold foil with a sheen that follows the light (lx, ly). */
void  ui_button(float x, float y, float w, float h, const char *label, float text_px,
                bool primary, bool focus, bool enabled, float lx, float ly, float t);

/* ---- The holofoil look (the launcher and the in-game menu) ------------------------- */
/* The accent: iridescent holofoil, a pastel drifting through cyan, violet and gold along
 * s and in time t. */
static inline Rgb ui_holo(float s, float t) { return mix(rgb(1, 1, 1), hue(s + t * .05f), .45f); }
/* A critically damped spring: x moves to target with velocity v (stiffness k). */
void  ui_spring(float *x, float *v, float target, float dt, float k);
/* Soft additive light of radius r centred at (cx, cy) (layout units). */
void  ui_glow(float cx, float cy, float r, Rgb c, float k);
/* A filled triangle, antialiased. */
void  ui_tri(float ax, float ay, float bx, float by, float cx, float cy, Rgb c, float a);
/* A capsule from (x0, y0) to (x1, y1) of radius r: filled, or only its outline that thick
 * (outline > 0). A single point makes a disc or a ring. */
void  ui_capsule(float x0, float y0, float x1, float y1, float r, float outline, Rgb c, float a);
/* A line icon about 20 units across (times size), centred at (cx, cy). */
enum { UI_ICON_DISPLAY, UI_ICON_EFFECTS, UI_ICON_GAME, UI_ICON_MUSIC, UI_ICON_CHEATS, UI_ICON_SPEED, UI_ICON_MORE };
void  ui_icon(int icon, float cx, float cy, float size, Rgb c, float a);
/* A bar filled with the accent along its length. */
void  ui_holo_bar(float x, float y, float w, float h, float t, float a);
/* An on/off switch right-aligned to x1 and centred on cy, its knob at k (0 off .. 1 on). */
void  ui_switch(float x1, float cy, float k, float t, float alpha);
/* A key hint at (x, y): the key on a small keycap, what it does after it. Returns where
 * the next one goes. */
float ui_keycap(float x, float y, const char *key, const char *what, float alpha);

#endif
