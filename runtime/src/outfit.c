/* Ali's outfit colours.
 *
 * Every sprite in the game uses palette line 0, so a colour cannot simply be changed in
 * CRAM: the townspeople would change with him. Instead the renderer recolours only the
 * sprites his object drew (vdp.c, with motion.c telling which those are). Each part of
 * him is a ramp of colour indices, brightest first, on his head, body or feet (his hair
 * and the Gold Armlet share their colours, as do his headband and boots, so height tells
 * them apart); one colour is picked per part, and each index gets it scaled to the
 * brightness the game gives that index, so folds and shading stay and the game's fades
 * still darken him. A part can also be animated: a shimmer sweeping up him, a slow pulse,
 * running rainbow hues or flickering flame (vdp_outfit_color).
 *
 * The page edits a copy of the settings: the preview shows it at once, and nothing in the
 * game changes until Apply. */
#include "outfit.h"

#include <stdlib.h>
#include <string.h>

#include "genesis.h"
#include "settings.h"
#include "ui.h"

/* The parts, from his sprites in the game: colour indices in palette 0, brightest first,
 * and that brightest colour (the picks stand for it). */
typedef struct { const char *name, *about; int n; uint8_t idx[3]; uint16_t top; uint8_t bands; } Part;
#define HEAD (1 << OUTFIT_HEAD)
#define BELOW (1 << OUTFIT_BODY | 1 << OUTFIT_FEET)
#define EVERYWHERE (HEAD | BELOW)
static const Part parts[OUTFIT_PARTS] = {
    { "Trousers", "His baggy trousers and their folds.", 2, { 15, 13 }, 0x0EEE, EVERYWHERE },
    { "Sash", "The sash around his waist (his sword's grip shares it).", 2, { 2, 1 }, 0x000C, EVERYWHERE },
    { "Boots", "His boots.", 3, { 7, 6, 5 }, 0x0EE4, BELOW },
    { "Hair", "His hair.", 2, { 4, 3 }, 0x02EE, HEAD },
    { "Headband", "The band across his brow.", 3, { 7, 6, 5 }, 0x0EE4, HEAD },
    { "Gold Armlet", "The Gold Armlet and the gold bands on his wrists and ankles.", 2, { 4, 3 }, 0x02EE, BELOW },
    { "Skin", "His skin, from pale to deep.", 3, { 10, 9, 8 }, 0x068C, EVERYWHERE },
    { "Jewel", "The small green jewel set in his sword's guard.", 1, { 12 }, 0x0284, EVERYWHERE },
};

typedef struct { const char *name; uint8_t r, g, b; } Preset;   /* 3-bit channels, as the Genesis has */
static const Preset presets[] = {
    { "White", 7, 7, 7 }, { "Ivory", 7, 7, 5 }, { "Sand", 7, 6, 4 }, { "Gold", 7, 5, 1 }, { "Amber", 7, 4, 1 },
    { "Orange", 7, 4, 0 }, { "Rust", 5, 2, 0 }, { "Crimson", 6, 0, 0 }, { "Scarlet", 7, 1, 1 }, { "Rose", 7, 3, 5 },
    { "Pink", 7, 5, 6 }, { "Magenta", 7, 0, 6 }, { "Violet", 5, 2, 7 }, { "Purple", 4, 0, 5 }, { "Indigo", 2, 1, 5 },
    { "Royal blue", 1, 2, 7 }, { "Navy", 0, 1, 4 }, { "Sky", 3, 6, 7 }, { "Cyan", 0, 7, 7 }, { "Teal", 0, 5, 5 },
    { "Jade", 2, 6, 4 }, { "Emerald", 1, 6, 2 }, { "Lime", 5, 7, 1 }, { "Olive", 4, 5, 1 }, { "Khaki", 6, 6, 3 },
    { "Brown", 4, 2, 1 }, { "Chocolate", 3, 1, 0 }, { "Silver", 5, 5, 6 }, { "Steel", 4, 5, 6 }, { "Slate", 3, 3, 4 },
    { "Charcoal", 2, 2, 2 }, { "Onyx", 1, 1, 1 },
};
static const char *const anim_names[OUTFIT_ANIMS] = { "Still", "Shimmer", "Pulse", "Rainbow", "Flame" };
#define PRESETS ((int)(sizeof presets / sizeof *presets))

static uint16_t cram_of(int r, int g, int b) { return (uint16_t)(b << 9 | g << 5 | r << 1); }
static int lum_of(uint16_t c) { return ((c >> 1) & 7) + ((c >> 5) & 7) + ((c >> 9) & 7); }

/* ---- Settings ---------------------------------------------------------------------- */
void outfit_defaults(void) {
    settings.outfit = false;
    for (int k = 0; k < OUTFIT_PARTS; k++) { settings.outfit_color[k] = -1; settings.outfit_anim[k] = 0; }
}

bool outfit_ini_load(const char *key, const char *value) {
    if (!strcmp(key, "outfit")) { settings.outfit = atoi(value) != 0; return true; }
    if (!strcmp(key, "outfit_anims")) {
        const char *p = value;
        for (int k = 0; k < OUTFIT_PARTS && *p; k++) {
            int a = atoi(p);
            settings.outfit_anim[k] = a >= 0 && a < OUTFIT_ANIMS ? a : 0;
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
        return true;
    }
    if (strcmp(key, "outfit_colors")) return false;
    const char *p = value;
    int n = 0;
    for (int k = 0; k < OUTFIT_PARTS && *p; k++) {
        char *end;
        long v = strtol(p, &end, 16);
        settings.outfit_color[k] = end != p && v >= 0 && v <= 0xEEE ? (int)(v & 0xEEE) : -1;
        n = k + 1;
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    /* The older list had four parts: boots with the headband, hair with the armlet. */
    if (n == 4) { settings.outfit_color[4] = settings.outfit_color[2]; settings.outfit_color[5] = settings.outfit_color[3]; }
    return true;
}

void outfit_ini_save(FILE *f) {
    fprintf(f, "# Ali's outfit: trousers, sash, boots, hair, headband, armlet, skin, jewel (Genesis CRAM colours, - = his own)\n");
    fprintf(f, "outfit=%d\noutfit_colors=", settings.outfit);
    for (int k = 0; k < OUTFIT_PARTS; k++) {
        if (settings.outfit_color[k] < 0) fprintf(f, "%s-", k ? "," : "");
        else fprintf(f, "%s%03X", k ? "," : "", settings.outfit_color[k]);
    }
    fprintf(f, "\n# 0 still, 1 shimmer, 2 pulse, 3 rainbow, 4 flame\noutfit_anims=");
    for (int k = 0; k < OUTFIT_PARTS; k++) fprintf(f, "%s%d", k ? "," : "", settings.outfit_anim[k]);
    fprintf(f, "\n");
}

static void tables_for(bool on, const int *colors, const int *anims, uint16_t pick[OUTFIT_BANDS][64],
                       uint8_t anim[OUTFIT_BANDS][64], uint8_t ref[64]) {
    for (int i = 0; i < 64; i++) {
        ref[i] = 21;
        for (int b = 0; b < OUTFIT_BANDS; b++) { pick[b][i] = OUTFIT_KEEP; anim[b][i] = 0; }
    }
    if (!on) return;
    for (int k = 0; k < OUTFIT_PARTS; k++) {
        if (colors[k] < 0) continue;
        for (int j = 0; j < parts[k].n; j++) {
            int i = parts[k].idx[j];
            for (int b = 0; b < OUTFIT_BANDS; b++)
                if (parts[k].bands & 1 << b) { pick[b][i] = (uint16_t)colors[k]; anim[b][i] = (uint8_t)anims[k]; }
            ref[i] = (uint8_t)lum_of(parts[k].top);
        }
    }
}

void outfit_apply(void) {
    tables_for(settings.outfit, settings.outfit_color, settings.outfit_anim, vdp_outfit_pick, vdp_outfit_anim, vdp_outfit_ref);
    bool any = false;
    for (int k = 0; k < OUTFIT_PARTS; k++) any |= settings.outfit_color[k] >= 0;
    vdp_opts.outfit = settings.outfit && any;
}

/* ---- The preview's picture of Ali ---------------------------------------------------- */
#define PREVIEW_MAGIC 0x4F544C41u       /* "ALTO" */
static uint8_t cache_img[PLAYER_IMG * PLAYER_IMG];
static uint16_t cache_cram[64];
static int cache_w, cache_h;

static void preview_path(char *out, size_t n, const char *dir) { snprintf(out, n, "%splayer_preview.cache", dir ? dir : ""); }

void outfit_preview_save(const char *dir) {
    if (!vdp_player_w || !vdp_player_h) return;
    char path[1100];
    preview_path(path, sizeof path, dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t head[3] = { PREVIEW_MAGIC, (uint32_t)vdp_player_w, (uint32_t)vdp_player_h };
    fwrite(head, 4, 3, f);
    fwrite(vdp_player, 1, sizeof vdp_player, f);
    fwrite(vdp_player_cram, 2, 64, f);
    fclose(f);
}

bool outfit_preview_load(const char *dir) {
    char path[1100];
    preview_path(path, sizeof path, dir);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t head[3] = { 0 };
    bool ok = fread(head, 4, 3, f) == 3 && head[0] == PREVIEW_MAGIC && head[1] > 0 && head[1] <= PLAYER_IMG &&
              head[2] > 0 && head[2] <= PLAYER_IMG && fread(cache_img, 1, sizeof cache_img, f) == sizeof cache_img &&
              fread(cache_cram, 2, 64, f) == 64;
    fclose(f);
    cache_w = ok ? (int)head[1] : 0;
    cache_h = ok ? (int)head[2] : 0;
    return ok;
}

/* ---- The page ---------------------------------------------------------------------- */
#define PANEL_X 160
#define PANEL_Y 30
#define PANEL_W 960
#define PANEL_H 660
#define BOX_X   (PANEL_X + 36)
#define BOX_Y   (PANEL_Y + 96)
#define BOX_W   350
#define BOX_H   412
#define LIST_X  (PANEL_X + 430)
#define LIST_R  (PANEL_X + PANEL_W - 44)
#define LIST_Y  (PANEL_Y + 92)
#define ROW_H   44
#define SWATCH_X (LIST_X + 160)
#define BTN_Y   (PANEL_Y + PANEL_H - 104)
#define BTN_H   50

enum { ROW_ON, ROW_PART0, ITEM_APPLY = ROW_PART0 + OUTFIT_PARTS, ITEM_RESET, ITEM_CANCEL, ITEMS };

void (*outfit_sound)(int kind);
static bool open_, mixing;
static int selected, channel;           /* channel being mixed: 0 red, 1 green, 2 blue */
static bool draft_on;
static int draft[OUTFIT_PARTS], draft_anim[OUTFIT_PARTS];
static double now;
static const uint8_t *src_img;
static const uint16_t *src_cram;
static int src_w, src_h;

static void sound(int kind) { if (outfit_sound) outfit_sound(kind); }

void outfit_open(void) {
    open_ = true;
    mixing = false;
    selected = ROW_PART0;
    draft_on = settings.outfit;
    memcpy(draft, settings.outfit_color, sizeof draft);
    memcpy(draft_anim, settings.outfit_anim, sizeof draft_anim);
    /* The game's latest picture of him, or the one it saved for the launcher. */
    if (vdp_player_w) { src_img = vdp_player; src_cram = vdp_player_cram; src_w = vdp_player_w; src_h = vdp_player_h; }
    else { src_img = cache_img; src_cram = cache_cram; src_w = cache_w; src_h = cache_h; }
}

bool outfit_is_open(void) { return open_; }

static float row_y(int r) { return LIST_Y + r * ROW_H; }
static float btn_x(int item) { return LIST_X + (item - ITEM_APPLY) * 164; }
static float btn_w(int item) { return item == ITEM_APPLY ? 150 : 150; }

static int hit_test(float mx, float my) {
    for (int r = 0; r < ITEM_APPLY; r++)
        if (mx >= LIST_X - 14 && mx <= LIST_R + 10 && my >= row_y(r) && my < row_y(r) + ROW_H) return r;
    for (int b = ITEM_APPLY; b < ITEMS; b++)
        if (mx >= btn_x(b) && mx <= btn_x(b) + btn_w(b) && my >= BTN_Y && my <= BTN_Y + BTN_H) return b;
    return -1;
}

static int preset_of(int color) {
    for (int k = 0; k < PRESETS; k++) if (cram_of(presets[k].r, presets[k].g, presets[k].b) == color) return k;
    return -1;
}

/* Steps a part through Original and the presets. */
static void cycle_part(int part, int dir) {
    int at = draft[part] < 0 ? -1 : preset_of(draft[part]);
    if (at < 0 && draft[part] >= 0) at = dir > 0 ? -1 : PRESETS;   /* from a mix: to the first or last preset */
    at += dir;
    if (at < -1) at = PRESETS - 1;
    if (at >= PRESETS) at = -1;
    draft[part] = at < 0 ? -1 : cram_of(presets[at].r, presets[at].g, presets[at].b);
    if (draft[part] >= 0) draft_on = true;
}

/* Steps a part's animation; a part in his own colours takes its usual colour to animate. */
static void cycle_anim(int part, int dir) {
    draft_anim[part] = settings_cycle(draft_anim[part], OUTFIT_ANIMS, dir);
    if (draft_anim[part] && draft[part] < 0) draft[part] = parts[part].top;
    draft_on = true;
    sound(1);
}

static void mix_part(int part, int dir) {
    int c = draft[part] < 0 ? parts[part].top : draft[part];
    int ch[3] = { (c >> 1) & 7, (c >> 5) & 7, (c >> 9) & 7 };
    ch[channel] = ch[channel] + dir < 0 ? 0 : ch[channel] + dir > 7 ? 7 : ch[channel] + dir;
    draft[part] = cram_of(ch[0], ch[1], ch[2]);
    draft_on = true;
}

static bool activate(void) {
    if (selected == ROW_ON) { draft_on = !draft_on; sound(1); return false; }
    if (selected < ITEM_APPLY) { mixing = !mixing; channel = 0; sound(1); return false; }
    if (selected == ITEM_RESET) {
        for (int k = 0; k < OUTFIT_PARTS; k++) { draft[k] = -1; draft_anim[k] = 0; }
        draft_on = false;
        sound(1);
        return false;
    }
    open_ = false;
    mixing = false;
    if (selected == ITEM_CANCEL) { sound(2); return false; }
    settings.outfit = draft_on;
    memcpy(settings.outfit_color, draft, sizeof draft);
    memcpy(settings.outfit_anim, draft_anim, sizeof draft_anim);
    outfit_apply();
    sound(1);
    return true;
}

static void change(int dir) {
    if (selected == ROW_ON) { draft_on = !draft_on; sound(1); }
    else if (selected < ITEM_APPLY) {
        if (mixing) mix_part(selected - ROW_PART0, dir);
        else cycle_part(selected - ROW_PART0, dir);
        sound(1);
    } else {
        int b = selected + dir;
        if (b >= ITEM_APPLY && b < ITEMS) { selected = b; sound(0); }
    }
}

static void move(int dir) {
    if (mixing) { channel = (channel + 3 + dir) % 3; sound(0); return; }
    int to = selected + dir;
    if (selected >= ITEM_APPLY) to = dir < 0 ? ITEM_APPLY - 1 : selected;
    else if (to >= ITEM_APPLY) to = ITEM_APPLY;
    if (to < 0) to = ITEM_CANCEL;
    if (to != selected) { selected = to; sound(0); }
}

bool outfit_event(const SDL_Event *e, float mx, float my) {
    if (!open_) return false;
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.sym) {
        case SDLK_ESCAPE: case SDLK_BACKSPACE:
            if (mixing) { mixing = false; sound(2); }
            else { open_ = false; sound(2); }
            break;
        case SDLK_UP: move(-1); break;
        case SDLK_DOWN: case SDLK_TAB: move(1); break;
        case SDLK_LEFT: change(-1); break;
        case SDLK_RIGHT: change(1); break;
        case SDLK_SPACE:
            if (selected >= ROW_PART0 && selected < ITEM_APPLY && !mixing) { cycle_anim(selected - ROW_PART0, 1); break; }
            return activate();
        case SDLK_RETURN: case SDLK_KP_ENTER: return activate();
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK:
            if (mixing) mixing = false; else open_ = false;
            sound(2);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: change(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: change(1); break;
        case SDL_CONTROLLER_BUTTON_X: case SDL_CONTROLLER_BUTTON_Y:
            if (selected >= ROW_PART0 && selected < ITEM_APPLY && !mixing) cycle_anim(selected - ROW_PART0, 1);
            break;
        case SDL_CONTROLLER_BUTTON_A: return activate();
        }
        break;
    case SDL_MOUSEMOTION: {
        int h = hit_test(mx, my);
        if (h >= 0 && h != selected && !mixing) { selected = h; sound(0); }
        break;
    }
    case SDL_MOUSEBUTTONDOWN: {
        int h = hit_test(mx, my);
        if (e->button.button == SDL_BUTTON_RIGHT) {
            if (h >= 0 && h < ITEM_APPLY) { selected = h; change(-1); }
            else { open_ = false; sound(2); }
        } else if (h >= 0) {
            if (h != selected) mixing = false;
            selected = h;
            if (h >= ROW_PART0 && h < ITEM_APPLY && !mixing && mx >= SWATCH_X + 30 && mx < SWATCH_X + 150) cycle_anim(h - ROW_PART0, 1);
            else if (h >= ROW_PART0 && h < ITEM_APPLY && !mixing) change(1);
            else return activate();
        }
        break;
    }
    }
    return false;
}

/* ---- Drawing ----------------------------------------------------------------------- */
static Rgb color_rgb(uint16_t c) { return unpack(vdp_color_rgb(c)); }

/* Ali at `scale` with the given outfit, his feet centred at (cx, bottom). */
static void draw_ali(float cx, float bottom, float scale, bool on, const int *colors, const int *anims, float alpha) {
    if (!src_w) return;
    uint16_t pick[OUTFIT_BANDS][64];
    uint8_t anim[OUTFIT_BANDS][64], ref[64];
    tables_for(on, colors, anims, pick, anim, ref);
    unsigned frame = (unsigned)(now * 60);
    float x0 = cx - src_w * scale / 2, y0 = bottom - src_h * scale;
    for (int y = 0; y < src_h; y++) {
        int above = src_h - 1 - y, band = outfit_band(above);
        for (int x = 0; x < src_w; x++) {
            int v = src_img[y * PLAYER_IMG + x];
            if (!v) continue;
            uint16_t c = pick[band][v] != OUTFIT_KEEP ? vdp_outfit_color(pick[band][v], anim[band][v], src_cram[v], ref[v], above, frame) : src_cram[v];
            ui_fill_rect(x0 + x * scale, y0 + y * scale, scale, scale, color_rgb(c), alpha);
        }
    }
}

static void draw_preview(void) {
    ui_panel(BOX_X, BOX_Y, BOX_W, BOX_H, 14, rgb_hex(0x2A2131), rgb_hex(0x120E16), 1, rgb_hex(0xD9B567), .3f);
    /* A warm pool of light where he stands. */
    float cx = BOX_X + BOX_W / 2.f, floor_y = BOX_Y + BOX_H - 40;
    for (int k = 0; k < 18; k++) {
        float w = 170 - k * 8, h = 22 - k * 1.1f;
        ui_fill_rect(cx - w / 2, floor_y - h / 2 + 6, w, h, rgb_hex(0xF0B060), .018f);
    }
    ui_draw_text("PREVIEW", "Segoe UI Semibold", 11.5f, 600, 3.5f, BOX_X + 20, BOX_Y + 16, rgb_hex(0xC9A45C), rgb_hex(0xC9A45C), 1);
    if (!src_w) {
        const char *l1 = "Ali appears here once he has", *l2 = "been on screen in the game.";
        float w1 = ui_text_width(l1, "Segoe UI", 15, 400, 0), w2 = ui_text_width(l2, "Segoe UI", 15, 400, 0);
        ui_draw_text(l1, "Segoe UI", 15, 400, 0, cx - w1 / 2, BOX_Y + BOX_H / 2 - 30, rgb_hex(0x9C927E), rgb_hex(0x9C927E), 1);
        ui_draw_text(l2, "Segoe UI", 15, 400, 0, cx - w2 / 2, BOX_Y + BOX_H / 2 - 8, rgb_hex(0x9C927E), rgb_hex(0x9C927E), 1);
        return;
    }
    float scale = floorf(fminf((BOX_W - 60) / (float)src_w, (BOX_H - 100) / (float)src_h));
    scale = clampf(scale, 2, 7);
    draw_ali(cx, floor_y, scale, draft_on, draft, draft_anim, 1);
    /* Below the box, as he is now, for comparison. */
    float small = fminf(1.5f, 70.f / src_h);
    ui_draw_text("NOW", "Segoe UI Semibold", 11.5f, 600, 3.5f, BOX_X + 4, BOX_Y + BOX_H + 26, rgb_hex(0x8F8673), rgb_hex(0x8F8673), 1);
    draw_ali(BOX_X + 70 + src_w * small / 2, BOX_Y + BOX_H + 12 + src_h * small, small, settings.outfit, settings.outfit_color, settings.outfit_anim, 1);
}

static void draw_bars(float x, float y, int color) {
    int ch[3] = { (color >> 1) & 7, (color >> 5) & 7, (color >> 9) & 7 };
    static const char *const names[3] = { "R", "G", "B" };
    static const uint32_t tints[3] = { 0xF06A5A, 0x6AD07A, 0x6A9AF0 };
    for (int k = 0; k < 3; k++) {
        float bx = x + k * 90;
        bool on = channel == k;
        Rgb lc = on ? rgb_hex(0xFFFFFF) : rgb_hex(0x8F8673);
        ui_draw_text(names[k], "Segoe UI Semibold", 14, 600, 0, bx, y - 9, lc, lc, 1);
        for (int s = 0; s < 7; s++)
            ui_fill_rect(bx + 16 + s * 10, y - 6, 7, 12, s < ch[k] ? rgb_hex(tints[k]) : rgb_hex(0x3A3330), on ? 1 : .6f);
        if (on) ui_fill_rect(bx - 4, y + 10, 88, 2, rgb_hex(0xE8C77A), .9f);
    }
}

void outfit_render(double time_s) {
    now = time_s;
    if (!open_) return;
    ui_fill_rect(0, 0, UI_BASE_W, UI_BASE_H, rgb(0, 0, 0), .6f);
    ui_panel(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 18, rgb_hex(0x1C1712), rgb_hex(0x0E0B09), 1, rgb_hex(0xD9B567), .4f);
    ui_draw_text("ALI'S OUTFIT", "Palatino Linotype", 32, 700, 6, BOX_X, PANEL_Y + 30, rgb_hex(0xFBEBB8), rgb_hex(0xC08A2C), 1);
    draw_preview();

    for (int r = 0; r < ITEM_APPLY; r++) {
        float y = row_y(r);
        bool focus = selected == r;
        if (focus) {
            ui_fill_rect(LIST_X - 14, y + 4, LIST_R - LIST_X + 24, ROW_H - 8, rgb_hex(0xD9B567), .08f);
            ui_fill_rect(LIST_X - 14, y + 4, 3, ROW_H - 8, rgb_hex(0xE8C77A), .95f);
        }
        Rgb lc = focus ? rgb_hex(0xFFFFFF) : rgb_hex(0xD8CDB6), vc = focus ? rgb_hex(0xFFE4A0) : rgb_hex(0xEBD9AE);
        float ty = y + ROW_H / 2.f - 11;
        if (r == ROW_ON) {
            ui_draw_text("Outfit colours", "Segoe UI", 17.5f, 400, 0, LIST_X, ty, lc, lc, 1);
            const char *v = draft_on ? "On" : "Off";
            float vw = ui_text_width(v, "Segoe UI Semibold", 17.5f, 600, 0);
            ui_draw_text(v, "Segoe UI Semibold", 17.5f, 600, 0, LIST_R - 22 - vw, ty, vc, vc, 1);
        } else {
            int k = r - ROW_PART0;
            ui_draw_text(parts[k].name, "Segoe UI", 17.5f, 400, 0, LIST_X, ty, lc, lc, 1);
            int c = draft[k] < 0 ? parts[k].top : draft[k];
            /* The swatch: the colour as the game shows it. */
            float sx = SWATCH_X;
            ui_panel(sx, y + ROW_H / 2.f - 12, 24, 24, 5, color_rgb((uint16_t)c), color_rgb((uint16_t)c), 1, rgb_hex(0xF3DDA4), focus ? .8f : .35f);
            if (focus && mixing) {
                draw_bars(sx + 46, y + ROW_H / 2.f, c);
            } else {
                if (draft_anim[k]) {    /* its animation, by the swatch */
                    Rgb tc = focus ? rgb_hex(0xF3C46A) : rgb_hex(0xB08A4A);
                    ui_draw_text(anim_names[draft_anim[k]], "Segoe UI Semibold", 13, 600, 1, sx + 36, y + ROW_H / 2.f - 8, tc, tc, 1);
                }
                int at = draft[k] < 0 ? -2 : preset_of(draft[k]);
                const char *v = at == -2 ? "Original" : at >= 0 ? presets[at].name : "Custom";
                float vw = ui_text_width(v, "Segoe UI Semibold", 17.5f, 600, 0);
                ui_draw_text(v, "Segoe UI Semibold", 17.5f, 600, 0, LIST_R - 22 - vw, ty, vc, vc, 1);
            }
        }
        if (!(focus && mixing && r != ROW_ON)) {
            Rgb ac = focus ? rgb_hex(0xE8C77A) : rgb_hex(0x6A5F4C);
            const char *v = r == ROW_ON ? (draft_on ? "On" : "Off") : NULL;
            float vw;
            if (v) vw = ui_text_width(v, "Segoe UI Semibold", 17.5f, 600, 0);
            else {
                int k = r - ROW_PART0, at = draft[k] < 0 ? -2 : preset_of(draft[k]);
                vw = ui_text_width(at == -2 ? "Original" : at >= 0 ? presets[at].name : "Custom", "Segoe UI Semibold", 17.5f, 600, 0);
            }
            ui_draw_text("\xE2\x80\xB9", "Segoe UI Semibold", 20, 600, 0, LIST_R - 22 - vw - 18, ty - 3, ac, ac, 1);
            ui_draw_text("\xE2\x80\xBA", "Segoe UI Semibold", 20, 600, 0, LIST_R - 10, ty - 3, ac, ac, 1);
        }
    }

    /* What the selected row does, or how to mix. */
    const char *about = selected == ROW_ON ? "Off shows Ali in his own colours, whatever is picked below." :
                        selected < ITEM_APPLY ? (mixing ? "\xE2\x86\x91\xE2\x86\x93 red, green, blue    \xE2\x86\x90\xE2\x86\x92 less / more    Enter  done"
                                                        : parts[selected - ROW_PART0].about) :
                        selected == ITEM_APPLY ? "Dress him in these colours." :
                        selected == ITEM_RESET ? "Back to his own colours (Apply to keep it)." : "Leave without changing anything.";
    ui_draw_text(about, "Segoe UI", 14, 400, 0, LIST_X, row_y(ITEM_APPLY) + 6, rgb_hex(0xB8AC92), rgb_hex(0xB8AC92), 1);
    if (selected >= ROW_PART0 && selected < ITEM_APPLY && !mixing)
        ui_draw_text("\xE2\x86\x90\xE2\x86\x92 colours    Space  animation    Enter  mix your own", "Segoe UI", 13, 400, 1, LIST_X, row_y(ITEM_APPLY) + 28,
                     rgb_hex(0x7C7262), rgb_hex(0x7C7262), 1);

    float lx = .5f * sinf((float)now * .45f), ly = .3f * cosf((float)now * .33f);
    static const char *const labels[3] = { "APPLY", "RESET", "CANCEL" };
    for (int b = ITEM_APPLY; b < ITEMS; b++)
        ui_button(btn_x(b), BTN_Y, btn_w(b), BTN_H, labels[b - ITEM_APPLY], 17, b == ITEM_APPLY, selected == b, true, lx, ly, (float)now);
    ui_draw_text("\xE2\x86\x91\xE2\x86\x93 select    \xE2\x86\x90\xE2\x86\x92 change    Space  animate    Enter  mix / choose    Esc  back", "Segoe UI", 12, 400, 1,
                 LIST_X, PANEL_Y + PANEL_H - 36, rgb_hex(0x6C7385), rgb_hex(0x6C7385), 1);
}
