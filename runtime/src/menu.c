/* In-game menu: pauses the game over a blurred, darkened copy of the last frame, on the
 * launcher's stone tablet with one tab per spirit (GAME, DISPLAY, EFFECTS, MORE). The card
 * on the right follows the selected row: the save slot's thumbnail, a live before/after
 * preview of a picture option on the paused frame (preview.c), or what the option does.
 * Rows glide and slide like the launcher's, embers drift over the game, and every choice
 * makes the game's own menu sound (played by main.c through menu_sound). */
#include "menu.h"
#include "controls.h"
#include "outfit.h"
#include "preview.h"
#include "launcher_scene.h"
#include "genesis.h"
#include "savestate.h"
#include "settings.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ITEM_ACTION, ITEM_SLOT, ITEM_OPTION } ItemKind;
enum { ACT_RESUME, ACT_SAVE, ACT_LOAD, ACT_RESET, ACT_QUIT, ACT_CONTROLS, ACT_OUTFIT };
typedef struct { ItemKind kind; const char *label; int id; } Item;

#define TABS 4
#define MAX_ROWS 10
static const char *const tab_names[TABS] = { "GAME", "DISPLAY", "EFFECTS", "MORE" };
static const int tab_spirit[TABS] = { 2, 0, 1, 3 };     /* Bow, Dytto, Efreet, Shade */
static const Item tab_items[TABS][MAX_ROWS] = {
    { { ITEM_ACTION, "Resume", ACT_RESUME }, { ITEM_SLOT, "Save state", ACT_SAVE }, { ITEM_SLOT, "Load state", ACT_LOAD },
      { ITEM_OPTION, "Cheats", OPT_CHEATS }, { ITEM_OPTION, "Rewind", OPT_REWIND },
      { ITEM_ACTION, "Reset game", ACT_RESET }, { ITEM_ACTION, "Quit game", ACT_QUIT } },
    { { ITEM_OPTION, "Widescreen", OPT_WIDESCREEN }, { ITEM_OPTION, "Aspect ratio", OPT_ASPECT },
      { ITEM_OPTION, "Integer scaling", OPT_INTEGER }, { ITEM_OPTION, "Internal resolution", OPT_INTERNAL },
      { ITEM_OPTION, "Scaler", OPT_SCALER }, { ITEM_OPTION, "Filter", OPT_FILTER },
      { ITEM_OPTION, "Sharpening", OPT_SHARPEN }, { ITEM_OPTION, "Smooth motion", OPT_SMOOTH },
      { ITEM_OPTION, "Sprite flicker fix", OPT_FLICKER }, { ITEM_OPTION, "Slowdown fix", OPT_OVERCLOCK } },
    { { ITEM_OPTION, "Lighting", OPT_LIGHTING }, { ITEM_OPTION, "Spirit effects", OPT_ELEMENTS },
      { ITEM_OPTION, "Item animations", OPT_ITEMS }, { ITEM_OPTION, "Water", OPT_WATER },
      { ITEM_OPTION, "Footsteps", OPT_FOOTSTEPS }, { ITEM_OPTION, "Dynamic lights", OPT_DYNLIGHT },
      { ITEM_OPTION, "HD-2D look", OPT_HD2D } },
    { { ITEM_OPTION, "Volume", OPT_VOLUME }, { ITEM_OPTION, "Audio", OPT_AUDIO },
      { ITEM_ACTION, "Controls", ACT_CONTROLS }, { ITEM_ACTION, "Ali's outfit", ACT_OUTFIT },
      { ITEM_OPTION, "Texture pack", OPT_TEXPACK }, { ITEM_OPTION, "Dump tiles", OPT_DUMP } },
};
static const int tab_count[TABS] = { 7, 10, 7, 6 };

/* What each option and action does, for the card on the right. */
static const char *about(const Item *it) {
    if (it->kind == ITEM_ACTION)
        switch (it->id) {
        case ACT_RESUME: return "Back to the game.";
        case ACT_RESET: return "Starts the game over from the beginning. Unsaved progress is lost.";
        case ACT_QUIT: return "Closes the game. Unsaved progress is lost; the game's own saves stay.";
        case ACT_CONTROLS: return "The keyboard key and controller button for every action.";
        case ACT_OUTFIT: return "Pick colours for Ali's clothes, hair, skin and jewellery, and animate them if you like (shimmer, pulse, rainbow, flame), with a preview before you apply them.";
        }
    if (it->kind == ITEM_SLOT) return NULL;
    switch (it->id) {
    case OPT_WIDESCREEN: return "Shows more of the world at the sides, streamed by the game's own map code. Off is the original 4:3 picture.";
    case OPT_ASPECT: return "The shape of the widescreen picture.";
    case OPT_INTEGER: return "Scales by whole numbers only, so every game pixel is the same size (black borders around the picture).";
    case OPT_INTERNAL: return "Draws each layer at a higher resolution, rounding the edges of the pixel art. Native is the original look; 6x (2544 x 1344) is for 1440p and 4K screens and needs a fast CPU.";
    case OPT_SCALER: return "How the layers are scaled for the internal resolution: Edge rounds corners, Smooth (xBR) follows curves and slopes.";
    case OPT_FILTER: return "Scanlines, a curved CRT tube or an LCD grid laid over the picture, like the screens of the time.";
    case OPT_SHARPEN: return "Crisper edges after scaling, without halos: soft edges are sharpened, hard pixel edges are left alone.";
    case OPT_SMOOTH: return "The game moves things 30 times a second; this draws each sprite between its moves, so motion is fluid on fast screens.";
    case OPT_FLICKER: return "Lifts the hardware's limit of sprites on one line, so crowded scenes no longer flicker.";
    case OPT_OVERCLOCK: return "Gives the console's processor more time each frame, so busy scenes no longer slow down. Music and speed stay the same.";
    case OPT_LIGHTING: return "Fire, magic, gems and bright metal glow softly into their surroundings. Daylight scenes are left as they are.";
    case OPT_TEXPACK: return "High-resolution replacements for the game's tiles, from a folder in texture_packs next to the game. None is the original art.";
    case OPT_DUMP: return "Writes every tile shown on screen once to texture_packs/dump, named by its key: the starting point for making a pack.";
    case OPT_FOOTSTEPS: return "Ali kicks up what is under his feet as he walks: puffs of dust on sand and stone, bits of grass, splashes and rings in water.";
    case OPT_ELEMENTS: return "The spirits and their magic give off particles by element: embers from fire, drops from water, wisps from shadow, leaves from plants. Ali, the people of Oasis, enemies, items and the HUD give none.";
    case OPT_WATER: return "The sea, rivers and pools roll with slow swells, foam where they meet the shore and anyone standing in them, glint along the crests and reflect the people and things above them.";
    case OPT_HD2D: return "A diorama look in the style of modern HD-2D games: the top and bottom of the screen soften like a tilt-shift photo while Ali stays sharp, warm sunbeams drift across daylight scenes, colours get warm highlights and cool shadows with a soft vignette, and on Full, dust motes float in the light. The HUD stays crisp.";
    case OPT_DYNLIGHT: return "The spirits, their magic and, in caves and at night, torches and fires light up their surroundings in their own colours, and Ali, the people and creatures cast soft shadows away from them. Dark scenes dim a little so the lights stand out; daylight stays as it is.";
    case OPT_ITEMS: return "Dropped items float gently above the ground on a soft glow, with a glint now and then, so they are easy to spot. The game itself is unchanged.";
    case OPT_CHEATS: return "Infinite health, spirit power, and weapons that never break.";
    case OPT_REWIND: return "Hold the rewind key to step back in time.";
    case OPT_VOLUME: return "The game's volume.";
    case OPT_AUDIO: return "Original is the console's sound as emulated. Clean removes aliasing, the hum and clipping; Rich adds wider stereo and a light room.";
    }
    return NULL;
}

/* ---- Layout, in 1280x720 units ---------------------------------------------------------- */
#define TAB_X0  56
#define TAB_Y0  22
#define TAB_W0  596
#define TAB_H0  676
#define LIST_X  (TAB_X0 + 44)
#define LIST_R  (TAB_X0 + TAB_W0 - 44)
#define TABS_Y  (TAB_Y0 + 92)
#define TABS_H  66
#define TABS_X  (TAB_X0 + 26)
#define TABW    ((TAB_W0 - 52) / TABS)
#define LIST_Y  (TABS_Y + TABS_H + 22)
#define ROW_H   40
#define VOLUME_W 140                    /* the volume slider's track, left of its value */
#define VOLUME_X (LIST_R - 20 - 46 - 26 - VOLUME_W)
#define CARD_X  690
#define CARD_Y  96
#define CARD_W  548
#define CARD_H  520

void (*menu_sound)(int kind);
static void sound(int kind) { if (menu_sound) menu_sound(kind); }
static void outfit_page_sound(int kind) { sound(kind); }

static bool open_;
static int tab, tab_from, selected;     /* selected: a row of the tab, or -1 for the tab bar */
static uint32_t *backdrop;              /* blurred game frame and the tablet, at canvas size */
static uint32_t *frame;                 /* the paused frame, for save thumbnails */
static int frame_w, frame_h;
static SlotInfo slot_info;
static int slot_info_for = -1;
static float mouse_scale = 1;
static char toast[96];
static double toast_until, now, last_now, opened_at, tab_at, changed_at;
static int changed_row = -1;
static float hl_y;
static bool confirm_pending;            /* reset/quit asked once, waiting for a second press */
static int confirm_item;

bool menu_is_open(void) { return open_; }

static float row_y(int i) { return LIST_Y + i * ROW_H; }
static const Item *item_at(int i) { return i >= 0 && i < tab_count[tab] ? &tab_items[tab][i] : NULL; }

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
    bool reopen = open_;                /* the window changed size while open: keep the place */
    open_ = true;
    if (!reopen) {
        tab = tab_from = 0;
        selected = 0;
        confirm_pending = false;
        opened_at = now;
        tab_at = -10;
        hl_y = row_y(0);
        sound(1);
    }
    slot_info_for = -1;
    mouse_scale = window_to_pixels;
    outfit_sound = outfit_page_sound;
    ui_begin(out_w, out_h);

    free(frame);
    frame_w = fb_w; frame_h = fb_h;
    frame = malloc((size_t)fb_w * fb_h * 4);
    for (int y = 0; y < fb_h; y++) memcpy(frame + (size_t)y * fb_w, fb + (size_t)y * fb_pitch, (size_t)fb_w * 4);

    /* Backdrop: the frame where the game shows it, blurred, darkened and warmed, with the
     * stone tablet on the left. */
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
            float u = (x - ui_ox) / ui_s, v = (y - ui_oy) / ui_s;
            float vig = 1 - .35f * smooth(.3f, 1.1f, sqrtf((u - 640) * (u - 640) / (640.f * 640.f) + (v - 360) * (v - 360) / (360.f * 360.f)));
            Rgb c = scale(unpack(backdrop[y * out_w + x]), .34f * vig);
            c = add(c, rgb(.09f, .05f, .02f), .5f);                          /* the desert's warmth */
            c = add(c, rgb(1, 1, 1), (hashf((uint32_t)(y * out_w + x)) - .5f) * 2.5f / 255);   /* dither against banding */
            backdrop[y * out_w + x] = pack(c);
        }
    scene_tablet(backdrop, TAB_X0, TAB_Y0, TAB_W0, TAB_H0, .94f);
}

void menu_select(int k, int row) {
    tab = tab_from = (k % TABS + TABS) % TABS;
    selected = row < -1 ? -1 : row >= tab_count[tab] ? tab_count[tab] - 1 : row;
    hl_y = row_y(selected < 0 ? 0 : selected);
    opened_at = tab_at = -10;
}

void menu_close(void) {
    if (open_) sound(2);
    open_ = false;
    savestate_info_free(&slot_info);
    slot_info_for = -1;
}

static void notify(const char *msg) {
    snprintf(toast, sizeof toast, "%s", msg);
    toast_until = now + 2.2;
}

enum { HIT_TAB0 = 100 };

static int hit_test(float mx, float my) {
    for (int k = 0; k < TABS; k++)
        if (mx >= TABS_X + k * TABW && mx < TABS_X + (k + 1) * TABW && my >= TABS_Y && my < TABS_Y + TABS_H) return HIT_TAB0 + k;
    for (int i = 0; i < tab_count[tab]; i++) {
        float y = row_y(i);
        if (mx >= LIST_X - 14 && mx <= LIST_R + 8 && my >= y && my < y + ROW_H) return i;
    }
    return -2;
}

static void select_row(int i) {
    if (i == selected) return;
    selected = i;
    confirm_pending = false;
    sound(0);
}

static void move(int dir) {
    int n = tab_count[tab] + 1;         /* the tab bar, then the rows */
    select_row(settings_cycle(selected + 1, n, dir) - 1);
}

static void open_tab(int k) {
    k = (k % TABS + TABS) % TABS;
    if (k == tab) return;
    tab_from = tab;
    tab = k;
    tab_at = now;
    if (selected >= tab_count[tab]) selected = tab_count[tab] - 1;
    confirm_pending = false;
    sound(0);
}

/* Runs the selected item (activate) or steps its value (dir = -1 / +1). */
static MenuAction act(int dir, bool activate) {
    if (selected < 0) {                 /* on the tab bar: left and right switch tabs */
        if (dir) open_tab(tab + dir);
        return MENU_NONE;
    }
    const Item *it = item_at(selected);
    if (!it) return MENU_NONE;
    char msg[96];
    switch (it->kind) {
    case ITEM_OPTION:
        if (it->id == OPT_VOLUME && !dir) return MENU_NONE;
        settings_option_change(it->id, dir ? dir : 1);
        changed_row = selected;
        changed_at = now;
        sound(1);
        return MENU_SETTINGS;
    case ITEM_SLOT:
        if (dir) {
            settings.state_slot = settings_cycle(settings.state_slot - 1, 9, dir) + 1;
            changed_row = selected;
            changed_at = now;
            sound(0);
            return MENU_SETTINGS;
        }
        if (!activate) return MENU_NONE;
        if (it->id == ACT_SAVE) {
            bool ok = savestate_save(settings.state_slot, frame, frame_w, frame_h, frame_w);
            snprintf(msg, sizeof msg, ok ? "Saved to slot %d" : "Could not save slot %d", settings.state_slot);
            notify(msg);
            sound(ok ? 1 : 3);
            slot_info_for = -1;
            return MENU_NONE;
        }
        if (savestate_load(settings.state_slot)) { sound(1); return MENU_LOADED; }
        snprintf(msg, sizeof msg, "Slot %d is empty or from another version", settings.state_slot);
        notify(msg);
        sound(3);
        return MENU_NONE;
    case ITEM_ACTION:
        if (!activate) return MENU_NONE;
        if (it->id == ACT_RESUME) return MENU_CLOSE;
        if (it->id == ACT_CONTROLS) { sound(1); controls_open(); return MENU_NONE; }
        if (it->id == ACT_OUTFIT) { sound(1); outfit_open(); return MENU_NONE; }
        /* Reset and quit lose progress: ask for a second press. */
        if (!confirm_pending || confirm_item != selected) {
            confirm_pending = true;
            confirm_item = selected;
            notify(it->id == ACT_RESET ? "Press again to reset the game" : "Press again to quit");
            sound(3);
            return MENU_NONE;
        }
        confirm_pending = false;
        return it->id == ACT_RESET ? MENU_RESET : MENU_QUIT;
    }
    return MENU_NONE;
}

MenuAction menu_event(const SDL_Event *e) {
    int dir = 0;
    bool activate = false;
    if (controls_is_open() || outfit_is_open()) {   /* a page takes all input while it is up */
        float mx = 0, my = 0;
        if (e->type == SDL_MOUSEMOTION) { mx = e->motion.x; my = e->motion.y; }
        if (e->type == SDL_MOUSEBUTTONDOWN) { mx = e->button.x; my = e->button.y; }
        mx = (mx * mouse_scale - ui_ox) / ui_s;
        my = (my * mouse_scale - ui_oy) / ui_s;
        if (outfit_is_open()) return outfit_event(e, mx, my) ? MENU_SETTINGS : MENU_NONE;
        bool changed = controls_event(e, mx, my);
        if (!controls_is_open()) sound(2);
        return changed ? MENU_SETTINGS : MENU_NONE;
    }
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.sym) {
        case SDLK_ESCAPE: case SDLK_BACKSPACE: return MENU_CLOSE;
        case SDLK_UP: move(-1); break;
        case SDLK_DOWN: case SDLK_TAB: move(1); break;
        case SDLK_LEFT: dir = -1; break;
        case SDLK_RIGHT: dir = 1; break;
        case SDLK_q: case SDLK_PAGEUP: open_tab(tab - 1); break;
        case SDLK_e: case SDLK_PAGEDOWN: open_tab(tab + 1); break;
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
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: open_tab(tab - 1); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: open_tab(tab + 1); break;
        case SDL_CONTROLLER_BUTTON_A: activate = true; break;
        }
        break;
    case SDL_MOUSEMOTION: {
        float mx = (e->motion.x * mouse_scale - ui_ox) / ui_s, my = (e->motion.y * mouse_scale - ui_oy) / ui_s;
        int h = hit_test(mx, my);
        if (h >= 0 && h < HIT_TAB0) select_row(h);
        return MENU_NONE;
    }
    case SDL_MOUSEBUTTONDOWN: {
        float mx = (e->button.x * mouse_scale - ui_ox) / ui_s, my = (e->button.y * mouse_scale - ui_oy) / ui_s;
        int h = hit_test(mx, my);
        if (h >= HIT_TAB0) { open_tab(h - HIT_TAB0); selected = -1; return MENU_NONE; }
        if (h < 0) return MENU_NONE;
        selected = h;
        const Item *it = item_at(h);
        if (e->button.button == SDL_BUTTON_LEFT && it->kind == ITEM_OPTION && it->id == OPT_VOLUME &&
            mx >= VOLUME_X - 8 && mx <= VOLUME_X + VOLUME_W + 8) {
            settings_set_volume_fraction(ui_slider_value(VOLUME_X, VOLUME_W, mx));   /* click on the track */
            sound(0);
            return MENU_SETTINGS;
        }
        if (e->button.button == SDL_BUTTON_RIGHT) dir = -1;
        else if (it->kind == ITEM_SLOT && mx > LIST_R - 150) dir = 1;   /* click the slot number to step it */
        else if (it->kind == ITEM_OPTION) dir = 1;
        else activate = true;
        break;
    }
    default:
        return MENU_NONE;
    }
    if (!dir && !activate) return MENU_NONE;
    return act(dir, activate);
}

/* ---- Drawing ----------------------------------------------------------------------------- */
static float ease_out(float x) { x = clampf(x, 0, 1); return 1 - (1 - x) * (1 - x) * (1 - x); }

static void draw_slot(float x, float y, float w, float t, float alpha) {
    if (slot_info_for != settings.state_slot) {
        savestate_info_free(&slot_info);
        savestate_info(settings.state_slot, &slot_info);
        slot_info_for = settings.state_slot;
    }
    /* The picture: thumbnail with a thin holographic foil frame. */
    float px = x, py = y, pw = w, ph = pw * 224 / 424.f;
    int x0 = (int)ui_x(px), y0 = (int)ui_y(py), x1 = (int)ui_x(px + pw), y1 = (int)ui_y(py + ph);
    for (int yy = y0 - 3; yy < y1 + 3; yy++)
        for (int xx = x0 - 3; xx < x1 + 3; xx++) {
            float u = (xx - ui_ox) / ui_s, v = (yy - ui_oy) / ui_s;
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
                    float n = hashf((uint32_t)(yy * 977 + xx)) * .02f;
                    c = mix(rgb_hex(0x14100C), rgb_hex(0x1E1812), (v - py) / ph);
                    c = add(c, rgb(1, 1, 1), n);
                }
                ui_blend_px(xx, yy, c, alpha);
            } else {
                float a = clampf(1 - fabsf(d - .5f) / (1.6f * ui_s / 1.5f + .6f), 0, 1);
                Rgb holo = mix(rgb(1, .9f, .6f), hue((u - px) / pw * .8f + (v - py) / ph * .5f + t * .03f), .45f);
                ui_blend_px(xx, yy, holo, a * .9f * alpha);
            }
        }
    if (!slot_info.exists) {
        const char *empty = "Empty slot";
        float ew = ui_text_width(empty, "Segoe UI", 18, 400, 2);
        ui_draw_text(empty, "Segoe UI", 18, 400, 2, px + (pw - ew) / 2, py + ph / 2 - 12, rgb_hex(0x7A6E5A), rgb_hex(0x7A6E5A), alpha);
    }
    char when[96];
    if (slot_info.exists) {
        struct tm *tm = localtime(&slot_info.saved_at);
        strftime(when, sizeof when, "Saved %d %b %Y, %H:%M", tm);
    } else {
        snprintf(when, sizeof when, "Nothing saved here yet");
    }
    ui_draw_text(when, "Segoe UI", 15, 400, 0, px, py + ph + 18, rgb_hex(0xD8CDB6), rgb_hex(0xD8CDB6), alpha);
    ui_draw_text("\xE2\x86\x90 \xE2\x86\x92  change slot", "Segoe UI", 12.5f, 400, 1, px, py + ph + 44,
                 rgb_hex(0x8F8673), rgb_hex(0x8F8673), alpha);
}

/* Wraps `text` to `width` and draws it from (x, y); returns the y below it. */
static float draw_wrapped(const char *text, float x, float y, float width, float px, Rgb c, float alpha) {
    char line[256], word[96];
    line[0] = 0;
    const char *p = text;
    while (*p) {
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < 95) word[n++] = *p++;
        word[n] = 0;
        if (!n) break;
        char test[352];
        snprintf(test, sizeof test, "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && ui_text_width(test, "Segoe UI", px, 400, 0) > width) {
            ui_draw_text(line, "Segoe UI", px, 400, 0, x, y, c, c, alpha);
            y += px * 1.45f;
            snprintf(line, sizeof line, "%s", word);
        } else {
            snprintf(line, sizeof line, "%s", test);
        }
    }
    if (line[0]) { ui_draw_text(line, "Segoe UI", px, 400, 0, x, y, c, c, alpha); y += px * 1.45f; }
    return y;
}

/* The card on the right, for the selected row. */
static void draw_card(float t, float alpha) {
    const Spirit *sp = &spirits[tab_spirit[tab]];
    ui_panel(CARD_X, CARD_Y, CARD_W, CARD_H, 16, rgb_hex(0x1C1712), rgb_hex(0x0E0B09), .9f * alpha, rgb_hex(0xD9B567), .35f * alpha);
    const Item *it = item_at(selected);
    float x = CARD_X + 28, w = CARD_W - 56, y = CARD_Y + 24;
    if (!it) {                          /* on the tab bar: the tab's spirit */
        scene_orb(tab_spirit[tab], CARD_X + CARD_W / 2.f, CARD_Y + 190, 52, t, 1);
        char line[64];
        snprintf(line, sizeof line, "%s  \xC2\xB7  %s", sp->name, tab_names[tab]);
        float lw = ui_text_width(line, "Palatino Linotype", 24, 700, 4);
        ui_draw_text(line, "Palatino Linotype", 24, 700, 4, CARD_X + (CARD_W - lw) / 2, CARD_Y + 290, rgb_hex(0xFFF1D0), rgb_hex(0xD9B567), alpha);
        const char *hint = "\xE2\x86\x90 \xE2\x86\x92  or  Q / E  to change tabs";
        float hw = ui_text_width(hint, "Segoe UI", 14, 400, 1);
        ui_draw_text(hint, "Segoe UI", 14, 400, 1, CARD_X + (CARD_W - hw) / 2, CARD_Y + 340, rgb_hex(0x8F8673), rgb_hex(0x8F8673), alpha);
        return;
    }
    char head[64];
    if (it->kind == ITEM_SLOT) snprintf(head, sizeof head, "SLOT %d", settings.state_slot);
    else {
        size_t k = 0;
        for (const char *p = it->label; *p && k < sizeof head - 1; p++) head[k++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
        head[k] = 0;
    }
    ui_draw_text(head, "Segoe UI Semibold", 12.5f, 600, 4, x, y, sp->light, sp->light, alpha);
    ui_fill_rect(x, y + 24, 40, 2, sp->light, .7f * alpha);
    y += 44;
    if (it->kind == ITEM_SLOT) { draw_slot(x, y, w, t, alpha); return; }
    if (it->kind == ITEM_OPTION && preview_supported(it->id) && preview_ready()) {
        float ph = w * 9 / 16.f;
        preview_draw(it->id, x, y, w, ph, t);
        ui_panel(x - 1, y - 1, w + 2, ph + 2, 4, rgb(0, 0, 0), rgb(0, 0, 0), 0, rgb_hex(0xD9B567), .5f * alpha);
        y += ph + 18;
    } else {
        /* No picture: the tab's spirit stands in for it. */
        scene_orb(tab_spirit[tab], CARD_X + CARD_W / 2.f, y + 110, 44, t, .8f);
        y += 220;
    }
    const char *text = about(it);
    if (text) y = draw_wrapped(text, x, y, w, 15.5f, rgb_hex(0xD8CDB6), alpha);
    if (it->kind == ITEM_OPTION) {
        char value[64];
        settings_option_text(it->id, value, sizeof value);
        char line[96];
        snprintf(line, sizeof line, "Now: %s", value);
        ui_draw_text(line, "Segoe UI Semibold", 15, 600, 0, x, y + 8, rgb_hex(0xFFE4A0), rgb_hex(0xFFE4A0), alpha);
    }
}

/* Embers drifting up over the paused game. */
static void draw_embers(float t) {
    for (int i = 0; i < 34; i++) {
        float life = 6 + 4 * hashf((uint32_t)i * 29u);
        float age = fmodf(t + hashf((uint32_t)i * 31u) * life, life) / life;
        float x = 660 + hashf((uint32_t)i * 37u) * 620 + sinf(t * .9f + i) * 14 * age;
        float y = 740 - age * 760;
        float k = sinf(age * 3.14159f) * (.18f + .25f * hashf((uint32_t)i * 41u));
        scene_glow(x, y, 3.2f, rgb(1, .62f, .25f), k);
    }
}

void menu_render(double time_s) {
    float dt = (float)(time_s - last_now);
    if (dt < 0 || dt > .25f) dt = 1 / 60.f;
    last_now = now = time_s;
    float t = (float)time_s;
    float in = ease_out((float)(now - opened_at) / .35f);

    memcpy(ui_px, backdrop, (size_t)ui_w * ui_h * 4);
    draw_embers(t);
    const Spirit *sp = &spirits[tab_spirit[tab]];

    /* Title, with the open tab's spirit beside it. */
    ui_draw_text("PAUSED", "Palatino Linotype", 40, 700, 8, LIST_X - 2, TAB_Y0 + 30, rgb_hex(0xFBEBB8), rgb_hex(0xC08A2C), in);
    scene_orb(tab_spirit[tab], LIST_R - 16, TAB_Y0 + 54, 16, t, .9f);

    /* The tabs. */
    for (int k = 0; k < TABS; k++) {
        float x = TABS_X + k * TABW, cx = x + TABW / 2.f;
        bool on = tab == k;
        const Spirit *s = &spirits[tab_spirit[k]];
        if (on) {
            ui_fill_rect(x + 6, TABS_Y, TABW - 12, TABS_H, s->light, selected < 0 ? .11f : .06f);
            for (int i = 0; i < 3; i++) ui_fill_rect(x + 14 - i * 3, TABS_Y + TABS_H - 3 - i, TABW - 28 + i * 6, 2, s->light, .5f - i * .15f);
        }
        scene_orb(tab_spirit[k], cx, TABS_Y + 22, on ? 14 : 11, t, on ? 1 : .45f);
        Rgb tc = on ? mix(rgb_hex(0xFFF4DA), s->light, .25f) : rgb_hex(0x8A8170);
        float tw = ui_text_width(tab_names[k], "Segoe UI Semibold", 12, 600, 3);
        ui_draw_text(tab_names[k], "Segoe UI Semibold", 12, 600, 3, cx - tw / 2, TABS_Y + 43, tc, tc, 1);
    }

    /* The rows slide in from the side the tab came from; the highlight glides. */
    float k = ease_out((float)(now - tab_at) / .3f);
    float dx = (1 - k) * 32 * (tab > tab_from ? 1 : -1) - (1 - in) * 40, alpha = k * in;
    if (selected >= 0) {
        hl_y += (row_y(selected) - hl_y) * (1 - expf(-dt * 14));
        ui_fill_rect(LIST_X - 14, hl_y + 3, LIST_R - LIST_X + 22, ROW_H - 6, sp->light, .08f);
        ui_fill_rect(LIST_X - 14, hl_y + 3, 3, ROW_H - 6, sp->light, .95f);
        scene_glow(LIST_X - 12, hl_y + ROW_H / 2.f, 14, sp->light, .3f);
    }
    for (int i = 0; i < tab_count[tab]; i++) {
        const Item *it = &tab_items[tab][i];
        float y = row_y(i);
        bool focus = selected == i;
        bool danger = it->kind == ITEM_ACTION && (it->id == ACT_RESET || it->id == ACT_QUIT);
        Rgb lc = focus ? (danger && confirm_pending ? rgb_hex(0xF2A08F) : rgb_hex(0xFFFFFF)) : rgb_hex(0xD8CDB6);
        ui_draw_text(it->label, "Segoe UI", 17.5f, 400, 0, LIST_X + dx, y + 9, lc, lc, alpha);
        char value[48] = "";
        if (it->kind == ITEM_OPTION) settings_option_text(it->id, value, sizeof value);
        else if (it->kind == ITEM_SLOT) snprintf(value, sizeof value, "Slot %d", settings.state_slot);
        if (!value[0]) continue;
        float pulse = changed_row == i ? 1 - ease_out((float)(now - changed_at) / .35f) : 0;
        Rgb vc = focus ? rgb_hex(0xFFE4A0) : rgb_hex(0xEBD9AE);
        if (pulse > 0) vc = mix(vc, rgb(1, 1, 1), pulse * .8f);
        Rgb ac = focus ? sp->light : rgb_hex(0x6A5F4C);
        float px = 17.5f + pulse * 1.5f;
        float vw = ui_text_width(value, "Segoe UI Semibold", px, 600, 0);
        float vx = LIST_R - 20 - vw;
        if (it->kind == ITEM_OPTION && it->id == OPT_VOLUME) {
            vx = LIST_R - 20 - ui_text_width("100%", "Segoe UI Semibold", px, 600, 0);
            ui_slider(VOLUME_X + dx, y + ROW_H / 2.f, VOLUME_W, settings.volume / 100.f, focus);
        }
        ui_draw_text("\xE2\x80\xB9", "Segoe UI Semibold", 20, 600, 0, vx - 18 - pulse * 3 + dx, y + 6, ac, ac, alpha);
        ui_draw_text("\xE2\x80\xBA", "Segoe UI Semibold", 20, 600, 0, LIST_R - 10 + pulse * 3 + dx, y + 6, ac, ac, alpha);
        ui_draw_text(value, "Segoe UI Semibold", px, 600, 0, vx + dx, y + 9 - pulse * .8f, vc, vc, alpha);
    }
    const char *keys = "\xE2\x86\x91\xE2\x86\x93 select    \xE2\x86\x90\xE2\x86\x92 change    Q E  tabs    Enter  select    Esc  resume";
    ui_draw_text(keys, "Segoe UI", 12, 400, 1, LIST_X, TAB_Y0 + TAB_H0 - 44, rgb_hex(0x7C7262), rgb_hex(0x7C7262), in);

    draw_card(t, in);

    if (now < toast_until) {
        float a = clampf((float)(toast_until - now) / .4f, 0, 1);
        float w = ui_text_width(toast, "Segoe UI Semibold", 16, 600, 1) + 48;
        float x = CARD_X + (CARD_W - w) / 2, y = CARD_Y + CARD_H + 24;
        ui_panel(x, y, w, 40, 20, rgb_hex(0x2A2416), rgb_hex(0x1A160D), .92f * a, rgb_hex(0xE8C77A), .6f * a);
        ui_draw_text(toast, "Segoe UI Semibold", 16, 600, 1, x + 24, y + 9, rgb_hex(0xF3DDA4), rgb_hex(0xF3DDA4), a);
    }
    controls_render(time_s);
    outfit_render(time_s);
}
