/* Live before/after previews. The renderer keeps every frame's layers (hd_frame), so a
 * frame can be drawn again with any picture options: once without the focused option and
 * once with it (its current value, or a typical one when it is off). Options that change
 * detail (internal resolution, scaler, sharpening) are shown 1:1 around the middle of the
 * screen; options that change the whole scene (lighting) show the whole frame. */
#include "preview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis.h"
#include "hd.h"
#include "settings.h"
#include "ui.h"

typedef struct { int scale, scaler, sharpen, lighting, hdr, tone, blind; bool pack, water; } Look;

static struct { bool world; float cam_x, cam_y, focus_x, focus_y; } where;

void preview_set_world(bool world, float cam_x, float cam_y, float focus_x, float focus_y) {
    where.world = world; where.cam_x = cam_x; where.cam_y = cam_y;
    where.focus_x = focus_x; where.focus_y = focus_y;
}

/* ---- The saved frame ------------------------------------------------------------------- */
#define FRAME_MAGIC 0x32455250u          /* "PRE2" */

static void frame_path(char *out, size_t n, const char *dir) { snprintf(out, n, "%spreview_frame.cache", dir ? dir : ""); }

void preview_save(const char *dir) {
    if (hd_frame.h <= 0 || hd_frame.w <= 0) return;
    char path[1100];
    frame_path(path, sizeof path, dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t head[2] = { FRAME_MAGIC, (uint32_t)sizeof hd_frame };
    fwrite(head, 4, 2, f);
    fwrite(&where, sizeof where, 1, f);
    fwrite(&hd_frame, sizeof hd_frame, 1, f);
    fclose(f);
}

bool preview_load(const char *dir) {
    char path[1100];
    frame_path(path, sizeof path, dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t head[2] = { 0, 0 };
    bool ok = fread(head, 4, 2, f) == 2 && head[0] == FRAME_MAGIC && head[1] == sizeof hd_frame &&
              fread(&where, sizeof where, 1, f) == 1 && fread(&hd_frame, sizeof hd_frame, 1, f) == 1;
    fclose(f);
    if (!ok) hd_frame.h = 0;
    hd_serial++;                        /* the layers changed: scaled copies are stale */
    return ok;
}

bool preview_ready(void) { return hd_frame.h > 0 && hd_frame.w > 0; }

/* ---- Which looks to compare ------------------------------------------------------------ */
static Look look_now(void) {
    return (Look){ settings.internal_scale, settings.scaler, settings.sharpen, settings.lighting, settings.hdr, settings.tone,
                   settings.color_blind, hd_texpack, settings.water };
}

/* The look without the option and with it, whether to show it 1:1, and the label of the
 * option's value on the right-hand side. */
static bool looks_for(int option, Look *off, Look *on, bool *zoom, char *label, size_t n) {
    *off = *on = look_now();
    Settings keep = settings;
    *zoom = false;
    switch (option) {
    case OPT_INTERNAL:
        off->scale = 1;
        on->scale = settings.internal_scale > 1 ? settings.internal_scale : 3;
        settings.internal_scale = on->scale;
        *zoom = true;
        break;
    case OPT_SCALER:
        if (on->scale < 2) on->scale = off->scale = 3;
        off->scaler = 0; on->scaler = 1;
        settings.scaler = 1;
        *zoom = true;
        break;
    case OPT_SHARPEN:
        off->sharpen = 0;
        on->sharpen = settings.sharpen ? settings.sharpen : 2;
        settings.sharpen = on->sharpen;
        *zoom = true;
        break;
    case OPT_LIGHTING:
        off->lighting = 0;
        on->lighting = settings.lighting ? settings.lighting : 2;
        settings.lighting = on->lighting;
        break;
    case OPT_HDR:
        off->hdr = 0;
        on->hdr = settings.hdr ? settings.hdr : 1;
        settings.hdr = on->hdr;
        break;
    case OPT_TONE:
        off->tone = VDP_TONE_ORIGINAL;
        on->tone = settings.tone ? settings.tone : VDP_TONE_WARM;
        settings.tone = on->tone;
        break;
    case OPT_BLIND:
        off->blind = VDP_BLIND_OFF;
        on->blind = settings.color_blind ? settings.color_blind : VDP_BLIND_DEUTAN;
        settings.color_blind = on->blind;
        break;
    case OPT_WATER:
        off->water = false; on->water = true;
        settings.water = true;
        break;
    case OPT_TEXPACK:
        if (!hd_texpack) { settings = keep; return false; }     /* the frame has no tile references without a pack */
        off->pack = false; on->pack = true;
        if (on->scale < 2) on->scale = off->scale = 3;
        *zoom = true;
        break;
    default:
        return false;
    }
    settings_option_text(option, label, n);
    settings = keep;
    return true;
}

bool preview_supported(int option) {
    Look a, b;
    bool z;
    char l[64];
    return looks_for(option, &a, &b, &z, l, sizeof l);
}

/* ---- Pictures --------------------------------------------------------------------------- */
typedef struct { uint32_t *px; int w, h; } Picture;

static void render_look(const Look *l, Picture *out) {
    int keep_scaler = hd_scaler;
    bool keep_pack = hd_texpack, keep_water = hd_water;
    float keep_time = hd_time;
    int keep_tone = vdp_opts.tone, keep_blind = vdp_opts.color_blind;
    vdp_opts.tone = l->tone;
    vdp_opts.color_blind = l->blind;
    hd_scaler = l->scaler;
    hd_texpack = l->pack;
    hd_water = l->water;
    hd_time = 2.0f;
    hd_compose(l->scale);
    hd_scaler = keep_scaler;
    hd_texpack = keep_pack;
    hd_water = keep_water;
    hd_time = keep_time;
    vdp_opts.tone = keep_tone;
    vdp_opts.color_blind = keep_blind;
    int scale = hd_w / (hd_frame.w > 0 ? hd_frame.w : 1);
    HdPost post = { .sharpen = l->sharpen, .lighting = l->lighting, .world = where.world, .cam_x = where.cam_x, .cam_y = where.cam_y,
                    .t = 2.0f, .scale = scale < 1 ? 1 : scale, .extra = hd_frame.extra, .hdr = l->hdr };
    const uint32_t *img = hd_post(hd_px, hd_w, hd_h, hd_w, &post);
    free(out->px);
    out->px = malloc((size_t)hd_w * hd_h * 4);
    out->w = hd_w; out->h = hd_h;
    if (out->px) memcpy(out->px, img, (size_t)hd_w * hd_h * 4);
}

static Picture before, after;
static struct { int option; Look off, on; unsigned serial; } made = { .option = -1 };
static char on_label[64];
static bool zoomed;

void preview_draw(int option, float x, float y, float w, float h, float t) {
    Look off, on;
    if (!preview_ready() || !looks_for(option, &off, &on, &zoomed, on_label, sizeof on_label)) return;
    int x0 = (int)ui_x(x), y0 = (int)ui_y(y), x1 = (int)ui_x(x + w), y1 = (int)ui_y(y + h);
    int pw = x1 - x0, ph = y1 - y0;
    if (pw <= 0 || ph <= 0) return;
    if (made.option != option || memcmp(&made.off, &off, sizeof off) || memcmp(&made.on, &on, sizeof on) || made.serial != hd_serial) {
        render_look(&off, &before);
        render_look(&on, &after);
        made.option = option; made.off = off; made.on = on;
        made.serial = hd_serial;
    }
    if (!before.px || !after.px) return;

    /* The window onto the frame, in 0-1 units of the frame: 1:1 pixels of the sharper
     * picture around the middle, or the whole frame fitted (cropped to the card's shape). */
    float fw = (float)hd_frame.w, fh = (float)hd_frame.h;
    float uw, vh, u0, v0;
    if (zoomed) {
        uw = (float)pw / after.w; vh = (float)ph / after.h;
        if (uw > 1) { vh /= uw; uw = 1; }
        if (vh > 1) { uw /= vh; vh = 1; }
    } else {
        float frame_aspect = fw * (4.f / 3.f) / (320.f / 224.f) / fh, card = (float)pw / ph;
        uw = frame_aspect > card ? card / frame_aspect : 1;
        vh = frame_aspect > card ? 1 : frame_aspect / card;
    }
    float cu = .5f, cv = .52f;
    if (zoomed && where.focus_x >= 0) { cu = where.focus_x / fw; cv = where.focus_y / fh; }   /* on Ali */
    u0 = cu - uw / 2; v0 = cv - vh / 2;
    if (u0 < 0) u0 = 0;
    if (u0 + uw > 1) u0 = 1 - uw;
    if (v0 < 0) v0 = 0;
    if (v0 + vh > 1) v0 = 1 - vh;
    /* Skip the widescreen sides when they are masked. */
    if (hd_frame.mask_sides && !zoomed && hd_frame.extra) {
        float side = hd_frame.extra / fw;
        if (uw > 1 - 2 * side) { float k = (1 - 2 * side) / uw; uw *= k; u0 = side; }
    }

    float split = .5f + .32f * sinf(t * .7f);
    int sx = x0 + (int)(pw * split);
    for (int py = y0; py < y1; py++) {
        if (py < 0 || py >= ui_h) continue;
        float v = v0 + (py - y0 + .5f) / ph * vh;
        uint32_t *row = ui_px + (size_t)py * ui_w;
        for (int px = x0; px < x1; px++) {
            if (px < 0 || px >= ui_w) continue;
            float u = u0 + (px - x0 + .5f) / pw * uw;
            const Picture *p = px < sx ? &before : &after;
            int ix = (int)(u * p->w), iy = (int)(v * p->h);
            ix = ix < 0 ? 0 : ix >= p->w ? p->w - 1 : ix;
            iy = iy < 0 ? 0 : iy >= p->h ? p->h - 1 : iy;
            row[px] = p->px[(size_t)iy * p->w + ix];
        }
    }
    /* The dividing line, and what each side is. */
    float lx = x + w * split;
    ui_fill_rect(lx - 1, y, 2, h, rgb_hex(0xF3D27A), .95f);
    for (int k = 1; k <= 4; k++) ui_fill_rect(lx - 1 - k, y, 1, h, rgb_hex(0xF3D27A), .18f / k);
    for (int k = 1; k <= 4; k++) ui_fill_rect(lx + k, y, 1, h, rgb_hex(0xF3D27A), .18f / k);
    float ow = ui_text_width("OFF", "Segoe UI Semibold", 12, 600, 2) + 22;
    ui_panel(x + 10, y + 10, ow, 24, 12, rgb_hex(0x15120E), rgb_hex(0x15120E), .8f, rgb_hex(0xC9A45C), .4f);
    ui_draw_text("OFF", "Segoe UI Semibold", 12, 600, 2, x + 21, y + 15, rgb_hex(0xD8CDB6), rgb_hex(0xD8CDB6), 1);
    float nw = ui_text_width(on_label, "Segoe UI Semibold", 12, 600, 1) + 22;
    ui_panel(x + w - 10 - nw, y + 10, nw, 24, 12, rgb_hex(0x2A2210), rgb_hex(0x2A2210), .85f, rgb_hex(0xF3D27A), .6f);
    ui_draw_text(on_label, "Segoe UI Semibold", 12, 600, 1, x + w - nw + 1, y + 15, rgb_hex(0xFFE4A0), rgb_hex(0xFFE4A0), 1);
}
