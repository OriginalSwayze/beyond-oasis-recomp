/* In-game menu: pauses the game over a blurred, darkened copy of the last frame, on the
 * launcher's dark holofoil steel, with a tab for each group of settings. The card on the
 * right follows the selected row: the save slot's thumbnail, a live before/after preview of
 * a picture option on the paused frame (preview.c), or what the option does. It moves like
 * the launcher (springs, switches, rows cascading in), dissolves in from the game and back
 * out to it, and every choice makes the game's own menu sound (played by main.c through
 * menu_sound). */
#include "menu.h"
#include "controls.h"
#include "outfit.h"
#include "preview.h"
#include "launcher_scene.h"
#include "foil.h"
#include "combat.h"
#include "automap.h"
#include "genesis.h"
#include "savestate.h"
#include "settings.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ITEM_ACTION, ITEM_SLOT, ITEM_OPTION } ItemKind;
enum { ACT_RESUME, ACT_SAVE, ACT_LOAD, ACT_RESET, ACT_QUIT, ACT_CONTROLS, ACT_OUTFIT, ACT_MAP, ACT_STATS };
typedef struct { ItemKind kind; const char *label; int id; } Item;

#define TABS 6
#define MAX_ROWS 10
static const char *const tab_names[TABS] = { "GAME", "DISPLAY", "EFFECTS", "CHEATS", "MORE", "SPEED" };
static const Item tab_items[TABS][MAX_ROWS] = {
    { { ITEM_ACTION, "Resume", ACT_RESUME }, { ITEM_SLOT, "Save state", ACT_SAVE }, { ITEM_SLOT, "Load state", ACT_LOAD },
      { ITEM_ACTION, "Map", ACT_MAP }, { ITEM_ACTION, "Stats", ACT_STATS },
      { ITEM_OPTION, "Rewind", OPT_REWIND }, { ITEM_OPTION, "Fast-forward", OPT_FAST },
      { ITEM_ACTION, "Reset game", ACT_RESET }, { ITEM_ACTION, "Quit game", ACT_QUIT } },
    { { ITEM_OPTION, "Widescreen", OPT_WIDESCREEN }, { ITEM_OPTION, "Aspect ratio", OPT_ASPECT },
      { ITEM_OPTION, "Integer scaling", OPT_INTEGER }, { ITEM_OPTION, "Internal resolution", OPT_INTERNAL },
      { ITEM_OPTION, "Scaler", OPT_SCALER }, { ITEM_OPTION, "Filter", OPT_FILTER },
      { ITEM_OPTION, "Sharpening", OPT_SHARPEN }, { ITEM_OPTION, "Smooth motion", OPT_SMOOTH },
      { ITEM_OPTION, "Sprite flicker fix", OPT_FLICKER }, { ITEM_OPTION, "Slowdown fix", OPT_OVERCLOCK } },
    { { ITEM_OPTION, "Lighting", OPT_LIGHTING }, { ITEM_OPTION, "HDR look", OPT_HDR },
      { ITEM_OPTION, "Colour tone", OPT_TONE }, { ITEM_OPTION, "Colour-blind help", OPT_BLIND }, { ITEM_OPTION, "Spirit effects", OPT_ELEMENTS },
      { ITEM_OPTION, "Water", OPT_WATER }, { ITEM_OPTION, "Footsteps", OPT_FOOTSTEPS },
      { ITEM_OPTION, "Combat info", OPT_COMBAT } },
    { { ITEM_OPTION, "Infinite health", OPT_CHEAT_HEALTH }, { ITEM_OPTION, "Infinite spirit", OPT_CHEAT_SPIRIT },
      { ITEM_OPTION, "Unbreakable weapons", OPT_CHEAT_WEAPONS }, { ITEM_OPTION, "Attack power", OPT_CHEAT_ATTACK },
      { ITEM_OPTION, "Move speed", OPT_CHEAT_SPEED }, { ITEM_OPTION, "Randomizer", OPT_RANDOMIZER } },
    { { ITEM_OPTION, "Language", OPT_LANGUAGE }, { ITEM_OPTION, "Volume", OPT_VOLUME }, { ITEM_OPTION, "Audio", OPT_AUDIO },
      { ITEM_ACTION, "Controls", ACT_CONTROLS }, { ITEM_ACTION, "Ali's outfit", ACT_OUTFIT },
      { ITEM_OPTION, "Rumble", OPT_RUMBLE }, { ITEM_OPTION, "Play timer", OPT_TIMER },
      { ITEM_OPTION, "Texture pack", OPT_TEXPACK }, { ITEM_OPTION, "Dump tiles", OPT_DUMP } },
    { { ITEM_OPTION, "Performance stats", OPT_PERF_STATS }, { ITEM_OPTION, "Auto quality", OPT_AUTO_QUALITY },
      { ITEM_OPTION, "GPU effects", OPT_GPU }, { ITEM_OPTION, "Merged finishing pass", OPT_MERGED },
      { ITEM_OPTION, "AVX2 pixel loops", OPT_AVX2 }, { ITEM_OPTION, "Slowdown fix", OPT_OVERCLOCK } },
};
static const int tab_count[TABS] = { 9, 10, 8, 6, 9, 6 };

/* What each option and action does, for the card on the right. */
static const char *about(const Item *it) {
    if (it->kind == ITEM_ACTION)
        switch (it->id) {
        case ACT_RESUME: return "Back to the game.";
        case ACT_RESET: return "Starts the game over from the beginning. Unsaved progress is lost.";
        case ACT_QUIT: return "Closes the game. Unsaved progress is lost; the game's own saves stay.";
        case ACT_CONTROLS: return "The keyboard key and controller button for every action.";
        case ACT_MAP: return "Fills in as you explore: everything that has been on screen in this area, with Ali's place blinking.";
        case ACT_STATS: return NULL;
        case ACT_OUTFIT: return "Pick colours for Ali's clothes, hair, skin and jewellery, and animate them if you like (shimmer, pulse, rainbow, flame), with a preview before you apply them.";
        }
    if (it->kind == ITEM_SLOT) return NULL;
    switch (it->id) {
    case OPT_WIDESCREEN: return "Shows more of the world at the sides, streamed by the game's own map code. Off is the original 4:3 picture.";
    case OPT_ASPECT: return "The shape of the widescreen picture.";
    case OPT_INTEGER: return "Scales by whole numbers only, so every game pixel is the same size (black borders around the picture).";
    case OPT_INTERNAL: return "Draws each layer at a higher resolution, rounding the edges of the pixel art. Native is the original look; 6x (2544 x 1344) is for 1440p and 4K screens and needs a fast CPU.";
    case OPT_SCALER: return "How the layers are scaled for the internal resolution: Edge rounds corners, Smooth (xBR) follows curves and slopes, Sharp keeps every pixel square, exactly as drawn. Edge and Smooth leave dither patterns crisp.";
    case OPT_HDR: return "A high-dynamic-range grade for any screen: deeper shadows, brighter highlights and richer colour, so fire, magic and gems stand out. Strong goes further.";
    case OPT_TONE: return "The colours' tone: Warm is a sunlit gold, Cool a moonlit blue, Vivid richer colour everywhere. It changes the console's own colours, so every effect follows.";
    case OPT_BLIND: return "For red-, green- or blue-blind players: colours that eye would see as the same are moved apart, so enemies, items, gems and magic stand out from what is around them.";
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
    case OPT_CHEAT_HEALTH: return "Ali takes no damage and his health stays full, whatever hits him.";
    case OPT_CHEAT_SPIRIT: return "Spirit power stays full, so the spirits can be called and kept as long as you like.";
    case OPT_CHEAT_WEAPONS: return "Weapons never wear out: a weapon keeps its uses however often it strikes.";
    case OPT_CHEAT_ATTACK: return "Enemies take double or four times the damage from every hit, or fall to a single one. Bosses too.";
    case OPT_CHEAT_SPEED: return "Ali walks and runs one and a half or two times as fast. Walls still stop him. If a scripted scene stalls, set it back to Normal.";
    case OPT_REWIND: return "Hold the rewind key to step back in time.";
    case OPT_RANDOMIZER: return "Chests, pots, enemies and the items lying about give other restoratives (and, with weapons, other weapons) than they used to. Each spot keeps its prize for the seed; turning it on picks a new seed. Keys, upgrades, spirit stones and the Bomb never change, so the game can always be finished.";
    case OPT_FAST: return "How fast the game runs while the fast-forward key is held (Tab, or RB on a controller; change it in Controls).";
    case OPT_COMBAT: return "The damage of every hit rises from the enemy as a number, gold for a one-hit kill; with health bars, a bar over an enemy shows what it has left for a few seconds after each hit.";
    case OPT_LANGUAGE: return settings_language_count() > 1
        ? "The version of the game played: each country's cartridge has its own language. Changing it starts that version from its title screen, with its own saves; unsaved progress here is lost."
        : "The version of the game played: each country's cartridge has its own language. Add another country's ROM in the launcher (Game, ROM) to choose it here.";
    case OPT_RUMBLE: return "The controller rumbles when Ali lands a hit, harder for a kill and when he is hit, and softly when a spirit is called.";
    case OPT_PERF_STATS: return "Shows in the top-left corner how many pictures a second reach the screen, the internal resolution, and how long each picture takes: putting the layers together, the effects (on the CPU or the graphics card) and sending it to the graphics card.";
    case OPT_AUTO_QUALITY: return "When pictures cannot keep up with your display, the internal resolution steps down (6x, 4x, 3x, 2x) until they do, and back up when there is room again. It never goes above the Internal resolution you picked.";
    case OPT_GPU: return "Lighting, the HDR look and sharpening on the graphics card instead of the CPU, in one pass. It uses Direct3D 11 (or OpenGL where that is missing), picked as the window opens, so turning it on or off takes effect the next time the game starts; without either the CPU does them as before.";
    case OPT_MERGED: return "On the CPU, lighting, the HDR look and sharpening in one pass over each band of the picture instead of a pass each over the whole picture. It saves a trip through memory but works a little more per band: try it on a PC with slow memory. Particles, damage numbers and the timer are then drawn after sharpening.";
    case OPT_AVX2: return "The CPU's effects loops work on eight pixels at a time with AVX2 instructions, on processors that have them (almost every PC since 2013). The result is the same.";
    case OPT_TIMER: return "This session's play time in the top corner of the picture, for speedruns and keeping track. The total is under Stats.";
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
static float hl_y, hl_v;               /* the highlight, on a spring to the selected row */
static float ind_x, ind_v;              /* the tabs' underline, on a spring to the open tab */
static float tab_on[TABS];              /* each tab's brightness, easing to the open one */
static float knob[MAX_ROWS];            /* each switch's knob, 0 off .. 1 on */
static int changed_dir = 1;             /* which way the last value changed (it slides in from there) */
static bool snap;                       /* the switches go straight to their side (a tab just opened) */
static double selected_at;              /* when the selection last moved (the card rises in) */
static double closing_at = -1;          /* when closing began (it dissolves back to the game), or -1 */
static uint32_t *sharp;                 /* the game frame where it shows, unblurred: the menu dissolves from it */
static bool open_pending;               /* the opening starts at the next frame drawn (now is old until then) */
static bool confirm_pending;            /* reset/quit asked once, waiting for a second press */
static int confirm_item;
static float mouse_x, mouse_y, mouse_t = -1;   /* for the light on the steel (layout units) */
static FoilSurface panel_surf, card_surf;

#define CLOSE_AFTER .2                   /* seconds the closing takes (CLOSE_S) */

/* Open until the closing has dissolved back to the game. */
bool menu_is_open(void) {
    if (open_ && closing_at >= 0 && now - closing_at >= CLOSE_AFTER) {
        open_ = false;
        closing_at = -1;
        savestate_info_free(&slot_info);
        slot_info_for = -1;
    }
    return open_;
}

static void tab_state(void) {
    ind_x = TABS_X + tab * TABW + TABW / 2.f;
    for (int k = 0; k < TABS; k++) tab_on[k] = k == tab;
    snap = true;
}

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
    bool reopen = open_;                /* the window changed size while open, or it was closing: keep the place */
    open_ = true;
    closing_at = -1;
    if (!reopen) {
        tab = tab_from = 0;
        selected = 0;
        confirm_pending = false;
        open_pending = true;
        tab_at = -10;
        hl_y = row_y(0);
        hl_v = 0;
        tab_state();
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

    /* Backdrop: the frame where the game shows it, blurred, darkened into the night, with
     * the shadows of the steel panel and the card. */
    free(backdrop);
    backdrop = calloc((size_t)out_w * out_h, 4);
    for (int y = 0; y < game_h; y++)
        for (int x = 0; x < game_w; x++) {
            int X = game_x + x, Y = game_y + y;
            if (X < 0 || Y < 0 || X >= out_w || Y >= out_h) continue;
            backdrop[Y * out_w + X] = frame[(y * fb_h / game_h) * fb_w + x * fb_w / game_w];
        }
    free(sharp);
    sharp = malloc((size_t)out_w * out_h * 4);
    if (sharp) memcpy(sharp, backdrop, (size_t)out_w * out_h * 4);
    blur_rgb(backdrop, out_w, out_h, (int)(9 * ui_s));
    for (int y = 0; y < out_h; y++)
        for (int x = 0; x < out_w; x++) {
            float u = (x - ui_ox) / ui_s, v = (y - ui_oy) / ui_s;
            float vig = 1 - .35f * smooth(.3f, 1.1f, sqrtf((u - 640) * (u - 640) / (640.f * 640.f) + (v - 360) * (v - 360) / (360.f * 360.f)));
            Rgb c = scale(unpack(backdrop[y * out_w + x]), .36f * vig);
            c = mix(c, scale(rgb(.05f, .04f, .09f), vig), .35f);              /* the night's violet */
            c = add(c, rgb(1, 1, 1), (hashf((uint32_t)(y * out_w + x)) - .5f) * 2.5f / 255);   /* dither against banding */
            backdrop[y * out_w + x] = pack(c);
        }
    foil_shadow_into(backdrop, TAB_X0 + 10, TAB_Y0 + 16, TAB_W0, TAB_H0, 20);
    foil_shadow_into(backdrop, CARD_X + 10, CARD_Y + 16, CARD_W, CARD_H, 20);
}

void menu_select(int k, int row) {
    tab = tab_from = (k % TABS + TABS) % TABS;
    selected = row < -1 ? -1 : row >= tab_count[tab] ? tab_count[tab] - 1 : row;
    hl_y = row_y(selected < 0 ? 0 : selected);
    opened_at = tab_at = selected_at = -10;
    open_pending = false;
    tab_state();
}

/* Starts the closing; menu_is_open() stays true until it has dissolved back to the game. */
void menu_close(void) {
    if (!open_ || closing_at >= 0) return;
    sound(2);
    closing_at = now;
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
    selected_at = now;
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
    selected_at = now;
    snap = true;
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
        changed_dir = dir ? dir : 1;
        changed_at = now;
        sound(1);
        return MENU_SETTINGS;
    case ITEM_SLOT:
        if (dir) {
            settings.state_slot = settings_cycle(settings.state_slot - 1, 9, dir) + 1;
            changed_row = selected;
            changed_dir = dir;
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
        if (it->id == ACT_MAP || it->id == ACT_STATS) return MENU_NONE;     /* shown on the card */
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
    if (closing_at >= 0) return MENU_NONE;  /* dissolving back to the game */
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
        mouse_x = mx; mouse_y = my; mouse_t = (float)now;
        int h = hit_test(mx, my);
        if (h >= 0 && h < HIT_TAB0) select_row(h);
        return MENU_NONE;
    }
    case SDL_MOUSEBUTTONDOWN: {
        float mx = (e->button.x * mouse_scale - ui_ox) / ui_s, my = (e->button.y * mouse_scale - ui_oy) / ui_s;
        int h = hit_test(mx, my);
        if (h >= HIT_TAB0) { open_tab(h - HIT_TAB0); selected = -1; selected_at = now; return MENU_NONE; }
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

/* ---- Drawing ----------------------------------------------------------------------------- *
 * The launcher's look: one iridescent holofoil accent (ui_holo), line icons on the tabs with
 * an underline on a spring, a highlight on a spring, switches for what is only on or off,
 * values sliding in from the side they changed to, rows cascading in on a new tab, and the
 * menu dissolving in from the game and back out to it. */
static float ease_out(float x) { x = clampf(x, 0, 1); return 1 - (1 - x) * (1 - x) * (1 - x); }

#define C_LABEL rgb_hex(0xDAD4C6)
#define C_GOLD  rgb_hex(0xF3D58E)
#define C_DIM   rgb_hex(0x8A8476)
#define OPEN_S  .35f                    /* the opening's length, seconds */
#define CLOSE_S .2f                     /* and the closing's */

static const int tab_icons[TABS] = { UI_ICON_GAME, UI_ICON_DISPLAY, UI_ICON_EFFECTS, UI_ICON_CHEATS, UI_ICON_MORE, UI_ICON_SPEED };

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
                    c = mix(rgb_hex(0x111318), rgb_hex(0x1A1D24), (v - py) / ph);
                    c = add(c, rgb(1, 1, 1), n);
                }
                ui_blend_px(xx, yy, c, alpha);
            } else {
                float a = clampf(1 - fabsf(d - .5f) / (1.6f * ui_s / 1.5f + .6f), 0, 1);
                ui_blend_px(xx, yy, ui_holo((u - px) / pw * .8f + (v - py) / ph * .5f, t), a * .9f * alpha);
            }
        }
    if (!slot_info.exists) {
        const char *empty = "Empty slot";
        float ew = ui_text_width(empty, "Segoe UI", 18, 400, 2);
        ui_draw_text(empty, "Segoe UI", 18, 400, 2, px + (pw - ew) / 2, py + ph / 2 - 12, C_DIM, C_DIM, alpha);
    }
    char when[96];
    if (slot_info.exists) {
        struct tm *tm = localtime(&slot_info.saved_at);
        strftime(when, sizeof when, "Saved %d %b %Y, %H:%M", tm);
    } else {
        snprintf(when, sizeof when, "Nothing saved here yet");
    }
    ui_draw_text(when, "Segoe UI", 15, 400, 0, px, py + ph + 18, C_LABEL, C_LABEL, alpha);
    ui_keycap(px, py + ph + 44, "\xE2\x86\x90\xE2\x86\x92", "change slot", alpha);
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

/* A tab's icon large, glowing, standing in for a picture. */
static void big_icon(float cx, float cy, float size, float t, float alpha) {
    ui_glow(cx, cy, 30 * size, ui_holo(.2f, t), .2f * alpha);
    ui_capsule(cx, cy, cx, cy, 15.5f * size, 1.2f, ui_holo(.35f, t), .45f * alpha);
    ui_icon(tab_icons[tab], cx, cy, size, mix(rgb(1, 1, 1), ui_holo(.1f, t), .35f), alpha);
}

/* The card on the right, for the selected row; its content rises in when that changes. */
static void draw_card(float t, float alpha) {
    if (foil_begin(&card_surf, CARD_X, CARD_Y, CARD_W, CARD_H, 16, rgb(.86f, .88f, 1))) {
        foil_frame(&card_surf, 8, 10);
        foil_recess(&card_surf, CARD_X + 18, CARD_Y + 18, CARD_W - 36, CARD_H - 36, 8, .3f);
        foil_finish(&card_surf);
    }
    float lx, ly;
    foil_light(t, mouse_x, mouse_y, mouse_t, &lx, &ly);
    foil_draw(&card_surf, lx, ly, t);
    float in = ease_out((float)(now - selected_at) / .28f);
    alpha *= in;
    const Item *it = item_at(selected);
    float x = CARD_X + 28, w = CARD_W - 56, y = CARD_Y + 24 + (1 - in) * 8;
    Rgb acc = ui_holo(.25f, t);
    if (!it) {                          /* on the tab bar: the tab itself */
        big_icon(CARD_X + CARD_W / 2.f, CARD_Y + 190 + (1 - in) * 8, 3.4f, t, alpha);
        float lw = ui_text_width(tab_names[tab], "Palatino Linotype", 26, 700, 8);
        ui_draw_text(tab_names[tab], "Palatino Linotype", 26, 700, 8, CARD_X + (CARD_W - lw) / 2, CARD_Y + 300, rgb_hex(0xFFF1D0), rgb_hex(0xD9B567), alpha);
        float kx = CARD_X + CARD_W / 2.f - 96;
        kx = ui_keycap(kx, CARD_Y + 350, "\xE2\x86\x90\xE2\x86\x92", "or", alpha);
        ui_keycap(kx - 8, CARD_Y + 350, "Q  E", "change tabs", alpha);
        return;
    }
    char head[64];
    if (it->kind == ITEM_SLOT) snprintf(head, sizeof head, "SLOT %d", settings.state_slot);
    else {
        size_t k = 0;
        for (const char *p = it->label; *p && k < sizeof head - 1; p++) head[k++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
        head[k] = 0;
    }
    ui_draw_text(head, "Segoe UI Semibold", 12.5f, 600, 4, x, y, acc, acc, alpha);
    ui_holo_bar(x, y + 24, 40 * in, 2, t, .85f * alpha);
    y += 44;
    if (it->kind == ITEM_SLOT) { draw_slot(x, y, w, t, alpha); return; }
    if (it->kind == ITEM_ACTION && it->id == ACT_MAP) {
        float mh = CARD_H - 44 - 24 - 110;
        ui_fill_rect(x, y, w, mh, rgb_hex(0x07080B), .55f * alpha);
        if (!automap_draw(x + 6, y + 6, w - 12, mh - 12, t, alpha)) {
            const char *none = "Nothing of this area seen yet.";
            float nw = ui_text_width(none, "Segoe UI", 15, 400, 0);
            ui_draw_text(none, "Segoe UI", 15, 400, 0, x + (w - nw) / 2, y + mh / 2 - 10, C_DIM, C_DIM, alpha);
        }
        ui_panel(x - 1, y - 1, w + 2, mh + 2, 4, rgb(0, 0, 0), rgb(0, 0, 0), 0, rgb_hex(0xD9B567), .5f * alpha);
        draw_wrapped(about(it), x, y + mh + 18, w, 15.5f, C_LABEL, alpha);
        return;
    }
    if (it->kind == ITEM_ACTION && it->id == ACT_STATS) {
        char lines[12][64];
        int n = combat_stats_lines(lines, 12);
        for (int i = 0; i < n; i++) {
            char *tab_at = strchr(lines[i], '\t');
            if (!tab_at) continue;
            *tab_at = 0;
            float ly = y + i * 38 + (i >= 2 ? 22 : 0);   /* this session, then all sessions */
            if (i == 2) ui_holo_bar(x, ly - 18, w, 1, t, .35f * alpha);
            ui_draw_text(lines[i], "Segoe UI", 16, 400, 0, x, ly, C_LABEL, C_LABEL, alpha);
            float vw = ui_text_width(tab_at + 1, "Segoe UI Semibold", 17, 600, 0);
            ui_draw_text(tab_at + 1, "Segoe UI Semibold", 17, 600, 0, x + w - vw, ly - 1, rgb_hex(0xFFE4A0), rgb_hex(0xFFE4A0), alpha);
        }
        return;
    }
    if (it->kind == ITEM_OPTION && preview_supported(it->id) && preview_ready()) {
        float ph = w * 9 / 16.f;
        preview_draw(it->id, x, y, w, ph, t);
        ui_panel(x - 1, y - 1, w + 2, ph + 2, 4, rgb(0, 0, 0), rgb(0, 0, 0), 0, rgb_hex(0xD9B567), .5f * alpha);
        y += ph + 18;
    } else {
        /* No picture: the tab's icon stands in for it. */
        big_icon(CARD_X + CARD_W / 2.f, y + 110, 2.8f, t, alpha);
        y += 220;
    }
    const char *text = about(it);
    if (text) y = draw_wrapped(text, x, y, w, 15.5f, C_LABEL, alpha);
    if (it->kind == ITEM_OPTION) {
        char value[64];
        settings_option_text(it->id, value, sizeof value);
        char line[96];
        snprintf(line, sizeof line, "Now: %s", value);
        ui_draw_text(line, "Segoe UI Semibold", 15, 600, 0, x, y + 8, rgb_hex(0xFFE4A0), rgb_hex(0xFFE4A0), alpha);
    }
}

/* The steel panel: the title stamped in gold, a holographic frame and rule, the rows'
 * recess. */
static void build_panel(void) {
    if (!foil_begin(&panel_surf, TAB_X0, TAB_Y0, TAB_W0, TAB_H0, 18, rgb(.84f, .86f, 1))) return;
    foil_frame(&panel_surf, 12, 10);
    float tw = ui_text_width("PAUSED", "Palatino Linotype", 40, 700, 8);
    foil_text(&panel_surf, "PAUSED", "Palatino Linotype", 40, 700, 8, LIST_X - 2 + tw / 2, TAB_Y0 + 56, -.9f, FOIL_GOLD);
    foil_rule(&panel_surf, TAB_X0 + 36, TAB_X0 + TAB_W0 - 36, TABS_Y + TABS_H + 9, 1);
    foil_recess(&panel_surf, LIST_X - 22, LIST_Y - 8, LIST_R - LIST_X + 36, TAB_Y0 + TAB_H0 - 64 - (LIST_Y - 8), 8, .35f);
    foil_finish(&panel_surf);
}

/* An option shown as a switch: only on or off, and reading so now. */
static bool row_switch(const Item *it, char *value, size_t n) {
    if (it->kind != ITEM_OPTION || !settings_option_is_switch(it->id)) return false;
    settings_option_text(it->id, value, n);
    return !strcmp(value, "On") || !strcmp(value, "Off");
}

/* The springs and eases, by dt seconds. */
static void animate(float dt) {
    if (selected >= 0) ui_spring(&hl_y, &hl_v, row_y(selected), dt, 320);
    ui_spring(&ind_x, &ind_v, TABS_X + tab * TABW + TABW / 2.f, dt, 220);
    float k = 1 - expf(-dt * 12);
    for (int i = 0; i < TABS; i++) tab_on[i] += ((i == tab ? 1.f : 0.f) - tab_on[i]) * k;
    for (int i = 0; i < tab_count[tab] && i < MAX_ROWS; i++) {
        char value[64];
        float target = row_switch(&tab_items[tab][i], value, sizeof value) && !strcmp(value, "On") ? 1.f : 0.f;
        knob[i] = snap ? target : knob[i] + (target - knob[i]) * (1 - expf(-dt * 14));
    }
    snap = false;
}

static void draw_row(int i, float dx, float alpha, float t) {
    const Item *it = &tab_items[tab][i];
    float y = row_y(i), cy = y + ROW_H / 2.f;
    bool focus = selected == i;
    bool danger = it->kind == ITEM_ACTION && (it->id == ACT_RESET || it->id == ACT_QUIT);
    Rgb lc = focus ? (danger && confirm_pending ? rgb_hex(0xF2A08F) : rgb_hex(0xFFFFFF)) : C_LABEL;
    ui_draw_text(it->label, "Segoe UI", 17, 400, 0, LIST_X + dx, y + 9, lc, lc, alpha);
    char value[64] = "";
    if (row_switch(it, value, sizeof value)) {
        ui_switch(LIST_R + dx, cy, knob[i], t, alpha);
        return;
    }
    if (it->kind == ITEM_OPTION) settings_option_text(it->id, value, sizeof value);
    else if (it->kind == ITEM_SLOT) snprintf(value, sizeof value, "Slot %d", settings.state_slot);
    if (!value[0]) {
        if (it->kind == ITEM_ACTION && focus) {          /* an action: a chevron to go */
            Rgb ac = danger ? rgb_hex(0xF2A08F) : ui_holo(.2f, t);
            float ax = LIST_R - 4 + dx;
            ui_capsule(ax - 4, cy - 5, ax, cy, .85f, 0, ac, alpha);
            ui_capsule(ax, cy, ax - 4, cy + 5, .85f, 0, ac, alpha);
        }
        return;
    }
    /* A new value slides in from the side it changed towards. */
    float s = changed_row == i ? ease_out((float)(now - changed_at) / .26f) : 1;
    float slide = (1 - s) * 16 * changed_dir, va = alpha * (.25f + .75f * s);
    Rgb vc = focus ? rgb_hex(0xFFE7AE) : C_GOLD;
    float vw = ui_text_width(value, "Segoe UI Semibold", 17, 600, 0);
    float vx = LIST_R - 20 - vw;
    if (it->kind == ITEM_OPTION && it->id == OPT_VOLUME) {
        vx = LIST_R - 20 - ui_text_width("100%", "Segoe UI Semibold", 17, 600, 0);
        ui_slider(VOLUME_X + dx, cy, VOLUME_W, settings.volume / 100.f, focus);
    }
    Rgb ac = focus ? ui_holo(.2f, t) : rgb_hex(0x5E5A52);
    float nudge = (1 - s) * 3, lx = vx - 12 - nudge * (changed_dir < 0) + dx, rx = LIST_R - 4 + nudge * (changed_dir > 0) + dx;
    ui_capsule(lx + 3, cy - 5, lx - 1, cy, .85f, 0, ac, alpha); ui_capsule(lx - 1, cy, lx + 3, cy + 5, .85f, 0, ac, alpha);
    ui_capsule(rx - 3, cy - 5, rx + 1, cy, .85f, 0, ac, alpha); ui_capsule(rx + 1, cy, rx - 3, cy + 5, .85f, 0, ac, alpha);
    ui_draw_text(value, "Segoe UI Semibold", 17, 600, 0, vx + dx + slide, y + 9, vc, vc, va);
}

void menu_render(double time_s) {
    float dt = (float)(time_s - last_now);
    if (dt < 0 || dt > .25f) dt = 1 / 60.f;
    last_now = now = time_s;
    if (open_pending) { opened_at = selected_at = now; open_pending = false; }
    float t = (float)time_s;
    animate(dt);
    float in = ease_out((float)(now - opened_at) / OPEN_S);
    if (closing_at >= 0) in *= 1 - ease_out((float)(now - closing_at) / CLOSE_S);

    memcpy(ui_px, backdrop, (size_t)ui_w * ui_h * 4);
    foil_atmosphere(t, .5f);

    build_panel();
    float lx, ly;
    foil_light(t, mouse_x, mouse_y, mouse_t, &lx, &ly);
    foil_draw(&panel_surf, lx, ly, t);

    /* The tabs: icons over their names, the open one lit, the underline on a spring. */
    for (int k = 0; k < TABS; k++) {
        float cx = TABS_X + k * TABW + TABW / 2.f, on = tab_on[k];
        Rgb ic = mix(rgb_hex(0x6E6A62), mix(rgb(1, 1, 1), ui_holo(k * .17f, t), .4f), on);
        if (on > .02f) ui_glow(cx, TABS_Y + 22, 30, ui_holo(k * .17f, t), .10f * on);
        ui_icon(tab_icons[k], cx, TABS_Y + 22 - on * 2, 1, ic, .6f + .4f * on);
        Rgb tc = mix(rgb_hex(0x7C776C), rgb_hex(0xF4EFE4), on);
        float tw = ui_text_width(tab_names[k], "Segoe UI Semibold", 12, 600, 2.5f);
        ui_draw_text(tab_names[k], "Segoe UI Semibold", 12, 600, 2.5f, cx - tw / 2, TABS_Y + 42, tc, tc, 1);
    }
    float uw = TABW - 36;
    ui_glow(ind_x, TABS_Y + TABS_H - 6, 40, ui_holo(.3f, t), selected < 0 ? .3f : .16f);
    ui_holo_bar(ind_x - uw / 2, TABS_Y + TABS_H - 7, uw, 2.5f, t, .95f);

    /* The highlight on a spring, with a living accent edge; the rows cascade in. */
    if (selected >= 0) {
        float a = ease_out((float)(now - tab_at - .1f) / .3f);
        ui_panel(LIST_X - 14, hl_y + 3, LIST_R - LIST_X + 22, ROW_H - 6, 8, rgb_hex(0x262A36), rgb_hex(0x1C1F28), .55f * a,
                 ui_holo(.1f, t), .22f * a);
        ui_capsule(LIST_X - 8, hl_y + 11, LIST_X - 8, hl_y + ROW_H - 11, 1.6f, 0, ui_holo(t * .1f, t), a);
        ui_glow(LIST_X - 8, hl_y + ROW_H / 2.f, 22, ui_holo(t * .1f, t), .28f * a);
    }
    int dir = tab > tab_from ? 1 : -1;
    for (int i = 0; i < tab_count[tab]; i++) {
        float e = ease_out((float)(now - fmax(tab_at, opened_at) - i * .03f) / .28f);
        if (e > 0) draw_row(i, (1 - e) * 24 * dir, e, t);
    }
    float kx = LIST_X, ky = TAB_Y0 + TAB_H0 - 46;
    kx = ui_keycap(kx, ky, "\xE2\x86\x91\xE2\x86\x93", "select", 1);
    kx = ui_keycap(kx, ky, "\xE2\x86\x90\xE2\x86\x92", "change", 1);
    kx = ui_keycap(kx, ky, "Q  E", "tabs", 1);
    kx = ui_keycap(kx, ky, "Enter", "choose", 1);
    ui_keycap(kx, ky, "Esc", "resume", 1);

    draw_card(t, 1);

    if (now < toast_until) {
        float a = clampf((float)(toast_until - now) / .4f, 0, 1) * ease_out((float)(2.2 - (toast_until - now)) / .2f);
        float w = ui_text_width(toast, "Segoe UI Semibold", 16, 600, 1) + 48;
        float x = CARD_X + (CARD_W - w) / 2, y = CARD_Y + CARD_H + 24 + (1 - a) * 8;
        ui_panel(x, y, w, 40, 20, rgb_hex(0x22252E), rgb_hex(0x15171D), .94f * a, ui_holo(.2f, t), .55f * a);
        ui_draw_text(toast, "Segoe UI Semibold", 16, 600, 1, x + 24, y + 9, rgb_hex(0xF3DDA4), rgb_hex(0xF3DDA4), a);
    }
    controls_render(time_s);
    outfit_render(time_s);

    /* Opening and closing: the menu dissolves in from the game itself, and back out to it. */
    if (in < 1 && sharp) {
        int a = (int)(clampf(in, 0, 1) * 256);
        for (size_t i = 0, n = (size_t)ui_w * ui_h; i < n; i++) {
            uint32_t m = ui_px[i], g = sharp[i];
            int r = (int)((g >> 16) & 255) + ((((int)((m >> 16) & 255) - (int)((g >> 16) & 255)) * a) >> 8);
            int gg = (int)((g >> 8) & 255) + ((((int)((m >> 8) & 255) - (int)((g >> 8) & 255)) * a) >> 8);
            int b = (int)(g & 255) + ((((int)(m & 255) - (int)(g & 255)) * a) >> 8);
            ui_px[i] = (uint32_t)(r << 16 | gg << 8 | b);
        }
    }
}
