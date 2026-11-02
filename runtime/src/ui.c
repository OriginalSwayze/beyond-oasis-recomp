/* Software UI drawing shared by the launcher and the in-game menu. */
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t *ui_px;
int ui_w, ui_h;
float ui_s = 1, ui_ox, ui_oy;

/* ---- Text cache ------------------------------------------------------------------ */
typedef struct { char key[160]; TextMask m; } TextEntry;
static TextEntry text_cache[128];
static int text_count;

static void text_cache_clear(void) {
    for (int i = 0; i < text_count; i++) text_free(&text_cache[i].m);
    text_count = 0;
}

void ui_begin(int w, int h) {
    float s = fminf(w / (float)UI_BASE_W, h / (float)UI_BASE_H);
    if (s != ui_s) text_cache_clear();
    if (w != ui_w || h != ui_h || !ui_px) {
        free(ui_px);
        ui_px = calloc((size_t)w * h, 4);
    }
    ui_w = w; ui_h = h; ui_s = s;
    ui_ox = (w - UI_BASE_W * s) / 2;
    ui_oy = (h - UI_BASE_H * s) / 2;
}

float ui_x(float x) { return ui_ox + x * ui_s; }
float ui_y(float y) { return ui_oy + y * ui_s; }

void ui_blend_px(int x, int y, Rgb c, float a) {
    if (x < 0 || y < 0 || x >= ui_w || y >= ui_h || a <= 0) return;
    uint32_t *p = &ui_px[y * ui_w + x];
    *p = a >= 1 ? pack(c) : pack(mix(unpack(*p), c, a));
}

void ui_fill_rect(float x, float y, float w, float h, Rgb c, float a) {
    int x0 = (int)ui_x(x), y0 = (int)ui_y(y), x1 = (int)ui_x(x + w), y1 = (int)ui_y(y + h);
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) ui_blend_px(xx, yy, c, a);
}

float ui_rrect_dist(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w / 2, cy = y + h / 2;
    float qx = fabsf(px - cx) - (w / 2 - r), qy = fabsf(py - cy) - (h / 2 - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
}

void ui_slider(float x, float y, float w, float value, bool focus) {
    value = clampf(value, 0, 1);
    const float th = 4, kr = focus ? 7 : 6;   /* track height, knob radius */
    float fx = x + w * value;
    int px0 = (int)ui_x(x - kr), px1 = (int)ui_x(x + w + kr) + 1;
    int py0 = (int)ui_y(y - kr), py1 = (int)ui_y(y + kr) + 1;
    Rgb track = rgb_hex(0x2A2F3B), fill = rgb_hex(0xD9B567), knob = focus ? rgb_hex(0xFBEBB8) : rgb_hex(0xE6E9EF);
    for (int py = py0; py < py1; py++)
        for (int px = px0; px < px1; px++) {
            float lx = (px + .5f - ui_ox) / ui_s, ly = (py + .5f - ui_oy) / ui_s;
            /* The track, rounded at both ends; gold up to the knob. */
            float d = ui_rrect_dist(lx, ly, x, y - th / 2, w, th, th / 2);
            float a = clampf(.5f - d * ui_s, 0, 1);
            if (a > 0) ui_blend_px(px, py, lx <= fx ? fill : track, a * (lx <= fx ? .95f : .9f));
            float dk = sqrtf((lx - fx) * (lx - fx) + (ly - y) * (ly - y)) - kr;
            float ak = clampf(.5f - dk * ui_s, 0, 1);
            if (ak > 0) ui_blend_px(px, py, knob, ak);
        }
}

float ui_slider_value(float x, float w, float mouse_x) { return clampf((mouse_x - x) / w, 0, 1); }

void ui_panel(float x, float y, float w, float h, float r, Rgb top, Rgb bottom, float alpha, Rgb border, float border_alpha) {
    int x0 = (int)ui_x(x) - 1, y0 = (int)ui_y(y) - 1, x1 = (int)ui_x(x + w) + 1, y1 = (int)ui_y(y + h) + 1;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float ux = (px + .5f - ui_ox) / ui_s, uy = (py + .5f - ui_oy) / ui_s;
            float d = ui_rrect_dist(ux, uy, x, y, w, h, r) * ui_s;
            float a = clampf(.5f - d, 0, 1);
            if (a <= 0) continue;
            Rgb c = mix(top, bottom, clampf((uy - y) / h, 0, 1));
            ui_blend_px(px, py, c, a * alpha);
            if (border_alpha > 0) {
                float edge = clampf(1 - fabsf(d + ui_s * .5f) / (ui_s * .9f), 0, 1);
                ui_blend_px(px, py, border, edge * border_alpha);
            }
        }
}

/* ---- Text ------------------------------------------------------------------------ */
const TextMask *ui_text_get(const char *s, const char *face, float px, int weight, float tracking) {
    char key[160];
    snprintf(key, sizeof key, "%s|%s|%d|%d|%d", s, face, (int)(px * ui_s), weight, (int)(tracking * ui_s));
    for (int i = 0; i < text_count; i++) if (!strcmp(text_cache[i].key, key)) return &text_cache[i].m;
    if (text_count == (int)(sizeof text_cache / sizeof *text_cache)) text_cache_clear();
    TextEntry *e = &text_cache[text_count++];
    snprintf(e->key, sizeof e->key, "%s", key);
    e->m = text_render(s, face, (int)(px * ui_s + .5f), weight, (int)(tracking * ui_s + .5f));
    return &e->m;
}

float ui_text_width(const char *s, const char *face, float px, int weight, float tracking) {
    return ui_text_get(s, face, px, weight, tracking)->w / ui_s;
}

void ui_draw_text(const char *s, const char *face, float px, int weight, float tracking,
                  float x, float y, Rgb top, Rgb bottom, float alpha) {
    const TextMask *m = ui_text_get(s, face, px, weight, tracking);
    if (!m->a) return;
    int ox = (int)ui_x(x), oy = (int)ui_y(y);
    for (int yy = 0; yy < m->h; yy++) {
        Rgb c = mix(top, bottom, m->h > 1 ? (float)yy / (m->h - 1) : 0);
        for (int xx = 0; xx < m->w; xx++) {
            uint8_t a = m->a[yy * m->w + xx];
            if (a) ui_blend_px(ox + xx, oy + yy, c, a / 255.f * alpha);
        }
    }
}

/* ---- Widgets --------------------------------------------------------------------- */
void ui_button(float x, float y, float w, float h, const char *label, float text_px,
               bool primary, bool focus, bool enabled, float lx, float ly, float t) {
    int x0 = (int)ui_x(x), y0 = (int)ui_y(y), x1 = (int)ui_x(x + w), y1 = (int)ui_y(y + h);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float ux = (px - ui_ox) / ui_s, uy = (py - ui_oy) / ui_s;
            float d = ui_rrect_dist(ux, uy, x, y, w, h, 10);
            float a = clampf(.5f - d * ui_s, 0, 1);
            if (a <= 0) continue;
            float u = (ux - x) / w, v = (uy - y) / h;
            Rgb c;
            if (primary && enabled) {
                /* Foil: a gold base with a holographic sheen that follows the light. */
                Rgb gold = mix(rgb_hex(0xF6DE9C), rgb_hex(0xB8862E), v);
                Rgb holo = mix(rgb(1, 1, 1), hue(u * .8f + v * .4f + lx * .6f + ly * .3f + t * .02f), .5f);
                float gd = u - (.5f + .7f * lx);
                c = mix(gold, holo, .22f + .06f * sinf(u * 5 + t * .8f));
                c = add(c, rgb(1, 1, 1), expf(-gd * gd * 30) * .45f);
                if (!focus) c = scale(c, .86f);
            } else {
                float edge = 1 - smooth(.5f, 1.6f, fabsf(d));
                c = mix(rgb_hex(0x141821), rgb_hex(0x1C2230), v);
                c = mix(c, focus ? rgb_hex(0xD9B567) : rgb_hex(0x4A5265), edge);
            }
            ui_blend_px(px, py, c, a);
        }
    float tw = ui_text_width(label, "Segoe UI Semibold", text_px, 600, text_px * .3f);
    const TextMask *m = ui_text_get(label, "Segoe UI Semibold", text_px, 600, text_px * .3f);
    Rgb tc = primary && enabled ? rgb_hex(0x2A1B05) : enabled ? (focus ? rgb_hex(0xF3DDA4) : rgb_hex(0xC9CED8)) : rgb_hex(0x5A6070);
    ui_draw_text(label, "Segoe UI Semibold", text_px, 600, text_px * .3f, x + (w - tw) / 2, y + (h - m->h / ui_s) / 2, tc, tc, 1);
}

/* ---- The holofoil look ------------------------------------------------------------- */
void ui_spring(float *x, float *v, float target, float dt, float k) {
    float c = 2 * sqrtf(k);
    for (float left = dt; left > 0; left -= 1 / 240.f) {   /* small steps: stable at any dt */
        float h = left < 1 / 240.f ? left : 1 / 240.f;
        *v += (k * (target - *x) - c * *v) * h;
        *x += *v * h;
    }
}

void ui_glow(float cx, float cy, float r, Rgb c, float k) {
    int px = (int)ui_x(cx), py = (int)ui_y(cy), pr = (int)(r * ui_s) + 1;
    float inv = 1.f / (r * ui_s);
    for (int y = py - pr; y <= py + pr; y++) {
        if (y < 0 || y >= ui_h) continue;
        for (int x = px - pr; x <= px + pr; x++) {
            if (x < 0 || x >= ui_w) continue;
            float dx = (x - px) * inv, dy = (y - py) * inv, d2 = dx * dx + dy * dy;
            if (d2 >= 1) continue;
            float f = 1 - d2, kk = k * f * f * f * 255;
            uint32_t *p = &ui_px[y * ui_w + x], d = *p;
            int r8 = (int)((d >> 16) & 255) + (int)(c.r * kk), g8 = (int)((d >> 8) & 255) + (int)(c.g * kk), b8 = (int)(d & 255) + (int)(c.b * kk);
            *p = (uint32_t)((r8 > 255 ? 255 : r8) << 16 | (g8 > 255 ? 255 : g8) << 8 | (b8 > 255 ? 255 : b8));
        }
    }
}

void ui_tri(float ax, float ay, float bx, float by, float cx, float cy, Rgb c, float a) {
    float x0 = fminf(ax, fminf(bx, cx)), x1 = fmaxf(ax, fmaxf(bx, cx)), y0 = fminf(ay, fminf(by, cy)), y1 = fmaxf(ay, fmaxf(by, cy));
    int px0 = (int)ui_x(x0) - 1, px1 = (int)ui_x(x1) + 1, py0 = (int)ui_y(y0) - 1, py1 = (int)ui_y(y1) + 1;
    for (int y = py0; y <= py1; y++)
        for (int x = px0; x <= px1; x++) {
            int in = 0;
            for (int s = 0; s < 4; s++) {   /* 2x2 samples a pixel */
                float ux = (x + .25f + (s & 1) * .5f - ui_ox) / ui_s, uy = (y + .25f + (s >> 1) * .5f - ui_oy) / ui_s;
                float e0 = (bx - ax) * (uy - ay) - (by - ay) * (ux - ax);
                float e1 = (cx - bx) * (uy - by) - (cy - by) * (ux - bx);
                float e2 = (ax - cx) * (uy - cy) - (ay - cy) * (ux - cx);
                in += (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
            }
            if (in) ui_blend_px(x, y, c, a * in / 4.f);
        }
}

void ui_capsule(float x0, float y0, float x1, float y1, float r, float outline, Rgb c, float a) {
    float pad = r + outline + 1;
    int px0 = (int)ui_x(fminf(x0, x1) - pad), px1 = (int)ui_x(fmaxf(x0, x1) + pad);
    int py0 = (int)ui_y(fminf(y0, y1) - pad), py1 = (int)ui_y(fmaxf(y0, y1) + pad);
    float dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy;
    for (int y = py0; y <= py1; y++)
        for (int x = px0; x <= px1; x++) {
            float ux = (x + .5f - ui_ox) / ui_s, uy = (y + .5f - ui_oy) / ui_s;
            float h = ll > 0 ? clampf(((ux - x0) * dx + (uy - y0) * dy) / ll, 0, 1) : 0;
            float ex = ux - x0 - dx * h, ey = uy - y0 - dy * h, d = sqrtf(ex * ex + ey * ey) - r;
            if (outline > 0) d = fabsf(d) - outline / 2;
            float cov = clampf(.5f - d * ui_s, 0, 1);  /* from the distance field: antialiased */
            if (cov > 0) ui_blend_px(x, y, c, a * cov);
        }
}

/* The icons' strokes and sparkles, in units of the icon's size. */
static float icon_x, icon_y, icon_s;
static void icon_line(float x0, float y0, float x1, float y1, Rgb c, float a) {
    ui_capsule(icon_x + x0 * icon_s, icon_y + y0 * icon_s, icon_x + x1 * icon_s, icon_y + y1 * icon_s, .85f * icon_s, 0, c, a);
}
static void icon_disc(float x, float y, float r, float outline, Rgb c, float a) {
    ui_capsule(icon_x + x * icon_s, icon_y + y * icon_s, icon_x + x * icon_s, icon_y + y * icon_s, r * icon_s, outline * icon_s, c, a);
}
static void icon_tri(float ax, float ay, float bx, float by, float cx, float cy, Rgb c, float a) {
    ui_tri(icon_x + ax * icon_s, icon_y + ay * icon_s, icon_x + bx * icon_s, icon_y + by * icon_s, icon_x + cx * icon_s, icon_y + cy * icon_s, c, a);
}
static void icon_star(float x, float y, float s, float w, Rgb c, float a) {
    icon_tri(x, y - s, x + w, y, x, y + s, c, a); icon_tri(x, y - s, x - w, y, x, y + s, c, a);
    icon_tri(x - s, y, x, y - w, x + s, y, c, a); icon_tri(x - s, y, x, y + w, x + s, y, c, a);
}

void ui_icon(int icon, float cx, float cy, float size, Rgb c, float a) {
    icon_x = cx; icon_y = cy; icon_s = size;
    switch (icon) {
    case UI_ICON_DISPLAY:               /* a monitor on its stand */
        icon_line(-9, -7, 9, -7, c, a); icon_line(-9, 5, 9, 5, c, a); icon_line(-9, -7, -9, 5, c, a); icon_line(9, -7, 9, 5, c, a);
        icon_line(0, 5, 0, 8.5f, c, a); icon_line(-4.5f, 8.8f, 4.5f, 8.8f, c, a);
        break;
    case UI_ICON_EFFECTS:               /* sparkles */
        icon_star(-2, 1, 8.5f, 2.2f, c, a); icon_star(6.5f, -6.5f, 3.6f, 1.1f, c, a);
        break;
    case UI_ICON_GAME:                  /* a controller */
        ui_capsule(cx - 5.5f * size, cy, cx + 5.5f * size, cy, 5.8f * size, 1.7f * size, c, a);
        icon_line(-6.5f, -2.6f, -6.5f, 2.6f, c, a); icon_line(-9.1f, 0, -3.9f, 0, c, a);
        icon_disc(5, -1.6f, 1.25f, 0, c, a); icon_disc(7.8f, 1.2f, 1.25f, 0, c, a);
        break;
    case UI_ICON_MUSIC:                 /* two beamed notes */
        icon_disc(-6, 5.5f, 3, 0, c, a); icon_disc(4.5f, 3.5f, 3, 0, c, a);
        icon_line(-3.1f, 5.5f, -3.1f, -7.5f, c, a); icon_line(7.4f, 3.5f, 7.4f, -9.5f, c, a);
        ui_capsule(cx - 3.1f * size, cy - 7.5f * size, cx + 7.4f * size, cy - 9.5f * size, 1.4f * size, 0, c, a);
        break;
    case UI_ICON_CHEATS:                /* a bolt */
        icon_tri(3, -10, -6.5f, 1.8f, .5f, 1.8f, c, a); icon_tri(-.5f, -1.8f, 6.5f, -1.8f, -3, 10, c, a);
        break;
    case UI_ICON_SPEED:                 /* fast forward */
        icon_line(-8, -7, -1.5f, 0, c, a); icon_line(-1.5f, 0, -8, 7, c, a);
        icon_line(.5f, -7, 7, 0, c, a); icon_line(7, 0, .5f, 7, c, a);
        break;
    default:                            /* MORE: three sliders */
        for (int k = 0; k < 3; k++) {
            float y = -6 + k * 6, knob = k == 0 ? 3 : k == 1 ? -4 : 1;
            icon_line(-9, y, 9, y, c, a * .7f);
            icon_disc(knob, y, 2.2f, 0, c, a);
        }
    }
}

void ui_holo_bar(float x, float y, float w, float h, float t, float a) {
    int n = (int)(w / 3) + 1;
    for (int i = 0; i < n; i++) ui_fill_rect(x + w * i / n, y, w / n + .4f, h, ui_holo((float)i / n * .6f, t), a);
}

void ui_switch(float x1, float cy, float k, float t, float alpha) {
    float x0 = x1 - 40, r = 10;
    Rgb track = mix(rgb_hex(0x262A35), mix(ui_holo(.15f, t), rgb_hex(0x9A8BC0), .25f), k);
    ui_capsule(x0 + r, cy, x1 - r, cy, r, 0, track, alpha);
    ui_capsule(x0 + r, cy, x1 - r, cy, r, 1, mix(rgb_hex(0x4A5060), rgb(1, 1, 1), k * .6f), alpha * .8f);
    float kx = x0 + r + (40 - 2 * r) * k;
    if (k > .05f) ui_glow(kx, cy, 16, ui_holo(.15f, t), .18f * k * alpha);
    ui_capsule(kx, cy + 1, kx, cy + 1, 7.5f, 0, rgb(0, 0, 0), .35f * alpha);
    ui_capsule(kx, cy, kx, cy, 7.5f, 0, mix(rgb_hex(0xC9C6BE), rgb(1, 1, 1), k), alpha);
}

float ui_keycap(float x, float y, const char *key, const char *what, float alpha) {
    float kw = ui_text_width(key, "Segoe UI Semibold", 11, 600, 0) + 12;
    ui_panel(x, y, kw, 19, 5, rgb_hex(0x2B303D), rgb_hex(0x191C24), alpha, rgb_hex(0x50576A), .9f * alpha);
    ui_fill_rect(x + 3, y + 17, kw - 6, 1.2f, rgb(0, 0, 0), .45f * alpha);
    ui_draw_text(key, "Segoe UI Semibold", 11, 600, 0, x + 6, y + 3, rgb_hex(0xE8E4DA), rgb_hex(0xE8E4DA), alpha);
    float ww = ui_text_width(what, "Segoe UI", 12, 400, 0);
    ui_draw_text(what, "Segoe UI", 12, 400, 0, x + kw + 6, y + 2.5f, rgb_hex(0x8A8476), rgb_hex(0x8A8476), alpha);
    return x + kw + 6 + ww + 16;
}
