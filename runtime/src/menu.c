/* In-game menu: pauses the game over a blurred, darkened copy of the last frame, with
 * save/load slots (each with a thumbnail of when it was saved), display options,
 * rewind, reset and quit. Drawn with the launcher's UI style. */
#include "menu.h"
#include "genesis.h"
#include "savestate.h"
#include "settings.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ITEM_HEADER, ITEM_ACTION, ITEM_SLOT, ITEM_OPTION } ItemKind;
enum { ACT_RESUME, ACT_SAVE, ACT_LOAD, ACT_RESET, ACT_QUIT };
typedef struct { ItemKind kind; const char *label; int id; } Item;

static const Item items[] = {
    { ITEM_ACTION, "Resume", ACT_RESUME },
    { ITEM_SLOT, "Save state", ACT_SAVE },
    { ITEM_SLOT, "Load state", ACT_LOAD },
    { ITEM_HEADER, "DISPLAY", 0 },
    { ITEM_OPTION, "Widescreen", OPT_WIDESCREEN },
    { ITEM_OPTION, "Aspect ratio", OPT_ASPECT },
    { ITEM_OPTION, "Fullscreen", OPT_FULLSCREEN },
    { ITEM_OPTION, "Integer scaling", OPT_INTEGER },
    { ITEM_OPTION, "Internal resolution", OPT_INTERNAL },
    { ITEM_OPTION, "Smooth motion", OPT_SMOOTH },
    { ITEM_OPTION, "Filter", OPT_FILTER },
    { ITEM_OPTION, "Sprite flicker fix", OPT_FLICKER },
    { ITEM_HEADER, "CHEATS", 0 },
    { ITEM_OPTION, "Infinite health", OPT_CHEAT_HEALTH },
    { ITEM_OPTION, "Infinite spirit", OPT_CHEAT_SPIRIT },
    { ITEM_OPTION, "Unbreakable weapons", OPT_CHEAT_WEAPONS },
    { ITEM_HEADER, "GAME", 0 },
    { ITEM_OPTION, "Rewind", OPT_REWIND },
    { ITEM_ACTION, "Reset game", ACT_RESET },
    { ITEM_ACTION, "Quit game", ACT_QUIT },
};
#define ITEM_COUNT ((int)(sizeof items / sizeof *items))

/* Layout, in 1280x720 units. */
#define PANEL_X 86
#define PANEL_Y 36
#define PANEL_W 520
#define PANEL_H 648
#define LIST_X  (PANEL_X + 40)
#define LIST_R  (PANEL_X + PANEL_W - 36)
#define LIST_Y  (PANEL_Y + 90)
#define ROW_H   26
#define HEAD_H  24
#define CARD_X  662
#define CARD_Y  166
#define CARD_W  532
#define CARD_H  388

static bool open_;
static int selected;
static uint32_t *backdrop;              /* blurred game frame at canvas size */
static uint32_t *frame;                 /* the paused frame, for save thumbnails */
static int frame_w, frame_h;
static SlotInfo slot_info;
static int slot_info_for = -1;
static float mouse_scale = 1;
static float lx, ly, tx, ty;
static double last_mouse;
static char toast[96];
static double toast_until, now;
static bool confirm_pending;            /* reset/quit asked once, waiting for a second press */
static int confirm_item;

bool menu_is_open(void) { return open_; }

static float item_y(int i) {
    float y = LIST_Y;
    for (int k = 0; k < i; k++) y += items[k].kind == ITEM_HEADER ? HEAD_H : ROW_H;
    return y;
}

/* Box blur in place, three passes per axis (close to a Gaussian). */
static void blur_rgb(uint32_t *px, int w, int h, int r) {
    if (r < 1) return;
    uint32_t *tmp = malloc((size_t)w * h * 4);
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int x = -r; x <= r; x++) {
                if (x < 0 || x >= w) continue;
                uint32_t p = px[y * w + x]; sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255; n++;
            }
            for (int x = 0; x < w; x++) {
                tmp[y * w + x] = (uint32_t)(sr / n) << 16 | (uint32_t)(sg / n) << 8 | (uint32_t)(sb / n);
                int out = x - r, in = x + r + 1;
                if (out >= 0) { uint32_t p = px[y * w + out]; sr -= (p >> 16) & 255; sg -= (p >> 8) & 255; sb -= p & 255; n--; }
                if (in < w) { uint32_t p = px[y * w + in]; sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255; n++; }
            }
        }
        for (int x = 0; x < w; x++) {
            int sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = -r; y <= r; y++) {
                if (y < 0 || y >= h) continue;
                uint32_t p = tmp[y * w + x]; sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255; n++;
            }
            for (int y = 0; y < h; y++) {
                px[y * w + x] = (uint32_t)(sr / n) << 16 | (uint32_t)(sg / n) << 8 | (uint32_t)(sb / n);
                int out = y - r, in = y + r + 1;
                if (out >= 0) { uint32_t p = tmp[out * w + x]; sr -= (p >> 16) & 255; sg -= (p >> 8) & 255; sb -= p & 255; n--; }
                if (in < h) { uint32_t p = tmp[in * w + x]; sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255; n++; }
            }
        }
    }
    free(tmp);
}

void menu_open(const uint32_t *fb, int fb_w, int fb_h, int fb_pitch, int game_x, int game_y, int game_w, int game_h,
               int out_w, int out_h, float window_to_pixels) {
    open_ = true;
    selected = 0;
    confirm_pending = false;
    slot_info_for = -1;
    mouse_scale = window_to_pixels;
    ui_begin(out_w, out_h);

    free(frame);
    frame_w = fb_w; frame_h = fb_h;
    frame = malloc((size_t)fb_w * fb_h * 4);
    for (int y = 0; y < fb_h; y++) memcpy(frame + (size_t)y * fb_w, fb + (size_t)y * fb_pitch, (size_t)fb_w * 4);

    /* Backdrop: the frame where the game shows it, blurred and darkened toward the panel. */
    free(backdrop);
    backdrop = calloc((size_t)out_w * out_h, 4);
    for (int y = 0; y < game_h; y++)
        for (int x = 0; x < game_w; x++) {
            int X = game_x + x, Y = game_y + y;
            if (X < 0 || Y < 0 || X >= out_w || Y >= out_h) continue;
            backdrop[Y * out_w + X] = frame[(y * fb_h / game_h) * fb_w + x * fb_w / game_w];
        }
    blur_rgb(backdrop, out_w, out_h, (int)(9 * ui_s));
    for (int y = 0; y < out_h; y++)
        for (int x = 0; x < out_w; x++) {
            float u = (x - ui_ox) / ui_s;
            float dark = .26f + .10f * smooth(PANEL_X + PANEL_W, 1280, u);   /* darker behind the panel */
            Rgb c = scale(unpack(backdrop[y * out_w + x]), dark);
            c = add(c, rgb(.05f, .035f, .015f), .5f);                         /* a touch of warmth */
            c = add(c, rgb(1, 1, 1), (hashf((uint32_t)(y * out_w + x)) - .5f) * 2.5f / 255);   /* dither against banding */
            backdrop[y * out_w + x] = pack(c);
        }
}

void menu_close(void) { open_ = false; savestate_info_free(&slot_info); slot_info_for = -1; }

static void notify(const char *msg) {
    snprintf(toast, sizeof toast, "%s", msg);
    toast_until = now + 2.2;
}

static int hit_test(float mx, float my) {
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (items[i].kind == ITEM_HEADER) continue;
        float y = item_y(i);
        if (mx >= LIST_X - 14 && mx <= LIST_R + 6 && my >= y && my < y + ROW_H) return i;
    }
    return -1;
}

static void move(int dir) {
    do selected = settings_cycle(selected, ITEM_COUNT, dir); while (items[selected].kind == ITEM_HEADER);
    confirm_pending = false;
}

/* Runs the selected item (activate) or steps its value (dir = -1 / +1). */
static MenuAction act(int dir, bool activate) {
    const Item *it = &items[selected];
    char msg[96];
    switch (it->kind) {
    case ITEM_OPTION:
        settings_option_change(it->id, dir ? dir : 1);
        return MENU_SETTINGS;
    case ITEM_SLOT:
        if (dir) {
            settings.state_slot = settings_cycle(settings.state_slot - 1, 9, dir) + 1;
            return MENU_SETTINGS;
        }
        if (!activate) return MENU_NONE;
        if (it->id == ACT_SAVE) {
            bool ok = savestate_save(settings.state_slot, frame, frame_w, frame_h, frame_w);
            snprintf(msg, sizeof msg, ok ? "Saved to slot %d" : "Could not save slot %d", settings.state_slot);
            notify(msg);
            slot_info_for = -1;
            return MENU_NONE;
        }
        if (savestate_load(settings.state_slot)) return MENU_LOADED;
        snprintf(msg, sizeof msg, "Slot %d is empty or from another version", settings.state_slot);
        notify(msg);
        return MENU_NONE;
    case ITEM_ACTION:
        if (!activate) return MENU_NONE;
        if (it->id == ACT_RESUME) return MENU_CLOSE;
        /* Reset and quit lose progress: ask for a second press. */
        if (!confirm_pending || confirm_item != selected) {
            confirm_pending = true;
            confirm_item = selected;
            notify(it->id == ACT_RESET ? "Press again to reset the game" : "Press again to quit");
            return MENU_NONE;
        }
        confirm_pending = false;
        return it->id == ACT_RESET ? MENU_RESET : MENU_QUIT;
    case ITEM_HEADER:
        break;
    }
    return MENU_NONE;
}

MenuAction menu_event(const SDL_Event *e) {
    int dir = 0;
    bool activate = false;
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.sym) {
        case SDLK_ESCAPE: case SDLK_BACKSPACE: return MENU_CLOSE;
        case SDLK_UP: move(-1); break;
        case SDLK_DOWN: case SDLK_TAB: move(1); break;
        case SDLK_LEFT: dir = -1; break;
        case SDLK_RIGHT: dir = 1; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_z: activate = true; break;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_START: return MENU_CLOSE;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: move(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move(1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: dir = -1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dir = 1; break;
        case SDL_CONTROLLER_BUTTON_A: activate = true; break;
        }
        break;
    case SDL_MOUSEMOTION: {
        float mx = (e->motion.x * mouse_scale - ui_ox) / ui_s, my = (e->motion.y * mouse_scale - ui_oy) / ui_s;
        tx = clampf((mx - 640) / 520, -1.2f, 1.2f);
        ty = clampf((my - 360) / 320, -1.2f, 1.2f);
        last_mouse = now;
        int h = hit_test(mx, my);
        if (h >= 0 && h != selected) { selected = h; confirm_pending = false; }
        return MENU_NONE;
    }
    case SDL_MOUSEBUTTONDOWN: {
        float mx = (e->button.x * mouse_scale - ui_ox) / ui_s, my = (e->button.y * mouse_scale - ui_oy) / ui_s;
        int h = hit_test(mx, my);
        if (h < 0) return MENU_NONE;
        selected = h;
        if (e->button.button == SDL_BUTTON_RIGHT) dir = -1;
        else if (items[h].kind == ITEM_SLOT && mx > LIST_R - 150) dir = 1;   /* click the slot number to step it */
        else activate = true;
        break;
    }
    default:
        return MENU_NONE;
    }
    if (!dir && !activate) return MENU_NONE;
    return act(dir, activate);
}

static void draw_slot_card(float t) {
    if (slot_info_for != settings.state_slot) {
        savestate_info_free(&slot_info);
        savestate_info(settings.state_slot, &slot_info);
        slot_info_for = settings.state_slot;
    }
    ui_panel(CARD_X, CARD_Y, CARD_W, CARD_H, 14, rgb_hex(0x161A24), rgb_hex(0x0C0E14), .82f, rgb_hex(0xD9B567), .28f);
    char head[32];
    snprintf(head, sizeof head, "SLOT %d", settings.state_slot);
    ui_draw_text(head, "Segoe UI Semibold", 12, 600, 4, CARD_X + 28, CARD_Y + 22, rgb_hex(0xC9A45C), rgb_hex(0xC9A45C), 1);

    /* The picture: thumbnail with a thin holographic foil frame. */
    float px = CARD_X + 28, py = CARD_Y + 52, pw = CARD_W - 56, ph = pw * 224 / 424.f;
    int x0 = (int)ui_x(px), y0 = (int)ui_y(py), x1 = (int)ui_x(px + pw), y1 = (int)ui_y(py + ph);
    for (int y = y0 - 3; y < y1 + 3; y++)
        for (int x = x0 - 3; x < x1 + 3; x++) {
            float u = (x - ui_ox) / ui_s, v = (y - ui_oy) / ui_s;
            float d = ui_rrect_dist(u, v, px, py, pw, ph, 6) * ui_s;
            if (d > 2.5f * ui_s / 1.5f + 1) continue;
            Rgb c;
            if (d < -1) {
                if (slot_info.thumb && slot_info.w) {
                    /* Fit the thumbnail by height; a narrower (4:3) one gets black bars. */
                    float tw = ph * slot_info.w / slot_info.h, tx0 = px + (pw - tw) / 2;
                    float fx = (u - tx0) / tw * slot_info.w, fy = (v - py) / ph * slot_info.h;
                    if (fx < 0 || fx >= slot_info.w) c = rgb(0, 0, 0);
                    else c = unpack(slot_info.thumb[(int)clampf(fy, 0, slot_info.h - 1) * slot_info.w + (int)fx]);
                } else {
                    float n = hashf((uint32_t)(y * 977 + x)) * .02f;
                    c = mix(rgb_hex(0x0E1118), rgb_hex(0x151A25), (v - py) / ph);
                    c = add(c, rgb(1, 1, 1), n);
                }
                ui_blend_px(x, y, c, 1);
            } else {
                float a = clampf(1 - fabsf(d - .5f) / (1.6f * ui_s / 1.5f + .6f), 0, 1);
                Rgb holo = mix(rgb(1, 1, 1), hue((u - px) / pw * .8f + (v - py) / ph * .5f + lx * .6f + ly * .4f + t * .03f), .7f);
                ui_blend_px(x, y, holo, a * .9f);
            }
        }
    if (!slot_info.exists) {
        const char *empty = "Empty slot";
        float w = ui_text_width(empty, "Segoe UI", 18, 400, 2);
        ui_draw_text(empty, "Segoe UI", 18, 400, 2, px + (pw - w) / 2, py + ph / 2 - 12, rgb_hex(0x5E6578), rgb_hex(0x5E6578), 1);
    }

    char when[96];
    if (slot_info.exists) {
        struct tm *tm = localtime(&slot_info.saved_at);
        strftime(when, sizeof when, "Saved %d %b %Y, %H:%M", tm);
    } else {
        snprintf(when, sizeof when, "Nothing saved here yet");
    }
    ui_draw_text(when, "Segoe UI", 15, 400, 0, px, py + ph + 18, rgb_hex(0xC5CAD5), rgb_hex(0xC5CAD5), 1);
    ui_draw_text("\xE2\x86\x90 \xE2\x86\x92  change slot", "Segoe UI", 12.5f, 400, 1, px, py + ph + 46,
                 rgb_hex(0x6C7385), rgb_hex(0x6C7385), 1);
}

void menu_render(double time_s) {
    now = time_s;
    float t = (float)time_s;
    float idle = (float)(now - last_mouse);
    float w = smooth(2.5f, 5, idle);
    float gx = tx * (1 - w) + .55f * sinf(t * .45f) * w, gy = ty * (1 - w) + .35f * cosf(t * .33f) * w;
    lx += (gx - lx) * .12f; ly += (gy - ly) * .12f;

    memcpy(ui_px, backdrop, (size_t)ui_w * ui_h * 4);
    ui_panel(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 18, rgb_hex(0x171B26), rgb_hex(0x0B0D12), .86f, rgb_hex(0xD9B567), .30f);

    ui_draw_text("PAUSED", "Palatino Linotype", 40, 700, 8, LIST_X - 2, PANEL_Y + 26, rgb_hex(0xFBEBB8), rgb_hex(0xC08A2C), 1);
    for (int px = (int)ui_x(LIST_X); px < (int)ui_x(LIST_R); px++) {
        float u = ((px - ui_ox) / ui_s - LIST_X) / (LIST_R - LIST_X);
        ui_blend_px(px, (int)ui_y(PANEL_Y + 82), mix(rgb_hex(0xD9B567), rgb_hex(0x2A2F3B), u), .9f * (1 - u * .8f));
    }

    for (int i = 0; i < ITEM_COUNT; i++) {
        const Item *it = &items[i];
        float y = item_y(i);
        if (it->kind == ITEM_HEADER) {
            ui_draw_text(it->label, "Segoe UI Semibold", 11.5f, 600, 3.5f, LIST_X, y + 13, rgb_hex(0xC9A45C), rgb_hex(0xC9A45C), 1);
            continue;
        }
        bool focus = selected == i;
        if (focus) {
            ui_fill_rect(LIST_X - 14, y + 1, LIST_R - LIST_X + 20, ROW_H - 2, rgb_hex(0xD9B567), .08f);
            ui_fill_rect(LIST_X - 14, y + 1, 3, ROW_H - 2, rgb_hex(0xE8C77A), .95f);
        }
        bool danger = it->kind == ITEM_ACTION && (it->id == ACT_RESET || it->id == ACT_QUIT);
        Rgb lc = focus ? (danger && confirm_pending ? rgb_hex(0xF2A08F) : rgb_hex(0xFFFFFF)) : rgb_hex(0xC5CAD5);
        ui_draw_text(it->label, "Segoe UI", 17, 400, 0, LIST_X, y + 5, lc, lc, 1);

        char value[48] = "";
        if (it->kind == ITEM_OPTION) settings_option_text(it->id, value, sizeof value);
        else if (it->kind == ITEM_SLOT) snprintf(value, sizeof value, "Slot %d", settings.state_slot);
        if (value[0]) {
            Rgb vc = focus ? rgb_hex(0xF3DDA4) : rgb_hex(0xE6E9EF);
            Rgb ac = focus ? rgb_hex(0xE8C77A) : rgb_hex(0x5B6274);
            float vw = ui_text_width(value, "Segoe UI Semibold", 17, 600, 0);
            float vx = LIST_R - 20 - vw;
            ui_draw_text("\xE2\x80\xB9", "Segoe UI Semibold", 18, 600, 0, vx - 18, y + 3, ac, ac, 1);
            ui_draw_text("\xE2\x80\xBA", "Segoe UI Semibold", 18, 600, 0, LIST_R - 10, y + 3, ac, ac, 1);
            ui_draw_text(value, "Segoe UI Semibold", 17, 600, 0, vx, y + 5, vc, vc, 1);
        }
    }
    const char *keys = "\xE2\x86\x91\xE2\x86\x93 select    \xE2\x86\x90\xE2\x86\x92 change    Enter  select    Esc  resume";
    ui_draw_text(keys, "Segoe UI", 12, 400, 1, LIST_X, PANEL_Y + PANEL_H - 38, rgb_hex(0x6C7385), rgb_hex(0x6C7385), 1);

    draw_slot_card(t);

    if (now < toast_until) {
        float a = clampf((float)(toast_until - now) / .4f, 0, 1);
        float w = ui_text_width(toast, "Segoe UI Semibold", 16, 600, 1) + 48;
        float x = CARD_X + (CARD_W - w) / 2, y = CARD_Y + CARD_H + 28;
        ui_panel(x, y, w, 40, 20, rgb_hex(0x2A2416), rgb_hex(0x1A160D), .92f * a, rgb_hex(0xE8C77A), .6f * a);
        ui_draw_text(toast, "Segoe UI Semibold", 16, 600, 1, x + 24, y + 9, rgb_hex(0xF3DDA4), rgb_hex(0xF3DDA4), a);
    }
}
