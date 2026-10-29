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
