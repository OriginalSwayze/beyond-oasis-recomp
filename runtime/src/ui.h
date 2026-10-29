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

#endif
