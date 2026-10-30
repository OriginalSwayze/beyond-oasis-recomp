/* Controls page: one row per action with its keyboard key and controller button.
 * Enter (or A, or a click) on a row waits for the next key or controller button, which
 * becomes that action's key or button; Esc cancels. Navigation itself always uses the
 * arrows / D-pad, Enter / A and Esc / B, so a binding can never lock you out. */
#include "controls.h"

#include <stdio.h>

#include "input.h"
#include "settings.h"
#include "ui.h"

#define ROW_RESET IN_COUNT
#define ROW_BACK  (IN_COUNT + 1)
#define ROWS      (IN_COUNT + 2)

#define PANEL_X 280
#define PANEL_Y 52
#define PANEL_W 720
#define PANEL_H 616
#define LIST_X  (PANEL_X + 44)
#define LIST_R  (PANEL_X + PANEL_W - 44)
#define KEY_X   (PANEL_X + 330)
#define PAD_X   (PANEL_X + 510)
#define LIST_Y  (PANEL_Y + 128)
#define ROW_H   36

static bool open_, capturing;
static int selected;
static double now, captured_until;      /* "Saved" flash on the row just bound */

void controls_open(void) { open_ = true; capturing = false; selected = 0; }
bool controls_is_open(void) { return open_; }

static float row_y(int i) { return LIST_Y + i * ROW_H + (i >= ROW_RESET ? 14 : 0); }

static int hit_test(float mx, float my) {
    for (int i = 0; i < ROWS; i++) {
        float y = row_y(i);
        if (mx >= LIST_X - 14 && mx <= LIST_R + 10 && my >= y && my < y + ROW_H) return i;
    }
    return -1;
}

static bool activate(void) {
    if (selected == ROW_BACK) { open_ = false; return false; }
    if (selected == ROW_RESET) { input_defaults(); captured_until = now + 1; return true; }
    capturing = true;
    return false;
}

bool controls_event(const SDL_Event *e, float mx, float my) {
    if (!open_) return false;
    if (capturing) {
        /* The next key or button is the new binding; Esc (or the Guide button) cancels. */
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            capturing = false;
            if (e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) return false;
            input_bind_key(selected, e->key.keysym.scancode);
            captured_until = now + 1;
            return true;
        }
        if (e->type == SDL_CONTROLLERBUTTONDOWN) {
            capturing = false;
            if (e->cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) return false;
            input_bind_button(selected, (SDL_GameControllerButton)e->cbutton.button);
            captured_until = now + 1;
            return true;
        }
        if (e->type == SDL_MOUSEBUTTONDOWN) capturing = false;
        return false;
    }
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.sym) {
        case SDLK_ESCAPE: case SDLK_BACKSPACE: open_ = false; break;
        case SDLK_UP: selected = (selected + ROWS - 1) % ROWS; break;
        case SDLK_DOWN: case SDLK_TAB: selected = (selected + 1) % ROWS; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return activate();
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_BACK: open_ = false; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: selected = (selected + ROWS - 1) % ROWS; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: selected = (selected + 1) % ROWS; break;
        case SDL_CONTROLLER_BUTTON_A: return activate();
        }
        break;
    case SDL_MOUSEMOTION: {
        int h = hit_test(mx, my);
        if (h >= 0) selected = h;
        break;
    }
    case SDL_MOUSEBUTTONDOWN: {
        int h = hit_test(mx, my);
        if (h >= 0 && e->button.button == SDL_BUTTON_LEFT) { selected = h; return activate(); }
        if (e->button.button == SDL_BUTTON_RIGHT) open_ = false;
        break;
    }
    }
    return false;
}

void controls_render(double time_s) {
    now = time_s;
    if (!open_) return;
    ui_fill_rect(0, 0, UI_BASE_W, UI_BASE_H, rgb(0, 0, 0), .55f);
    ui_panel(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 18, rgb_hex(0x171B26), rgb_hex(0x0B0D12), 1, rgb_hex(0xD9B567), .35f);
    ui_draw_text("CONTROLS", "Palatino Linotype", 34, 700, 7, LIST_X - 2, PANEL_Y + 26, rgb_hex(0xFBEBB8), rgb_hex(0xC08A2C), 1);
    Rgb head = rgb_hex(0xC9A45C);
    ui_draw_text("KEYBOARD", "Segoe UI Semibold", 11.5f, 600, 3.5f, KEY_X, LIST_Y - 26, head, head, 1);
    ui_draw_text("CONTROLLER", "Segoe UI Semibold", 11.5f, 600, 3.5f, PAD_X, LIST_Y - 26, head, head, 1);

    for (int i = 0; i < ROWS; i++) {
        float y = row_y(i);
        bool focus = selected == i;
        if (focus) {
            ui_fill_rect(LIST_X - 14, y + 1, LIST_R - LIST_X + 24, ROW_H - 2, rgb_hex(0xD9B567), .08f);
            ui_fill_rect(LIST_X - 14, y + 1, 3, ROW_H - 2, rgb_hex(0xE8C77A), .95f);
        }
        Rgb lc = focus ? rgb_hex(0xFFFFFF) : rgb_hex(0xC5CAD5);
        if (i == ROW_RESET) { ui_draw_text("Reset to defaults", "Segoe UI", 17, 400, 0, LIST_X, y + 7, lc, lc, 1); continue; }
        if (i == ROW_BACK) { ui_draw_text("Back", "Segoe UI", 17, 400, 0, LIST_X, y + 7, lc, lc, 1); continue; }
        ui_draw_text(input_labels[i], "Segoe UI", 17, 400, 0, LIST_X, y + 7, lc, lc, 1);
        if (focus && capturing) {
            /* Blinking prompt in place of the two bindings. */
            float a = .55f + .45f * (float)((int)(now * 2.5) & 1);
            ui_draw_text("Press a key or a controller button\xE2\x80\xA6   Esc cancels", "Segoe UI Semibold", 15, 600, 0,
                         KEY_X, y + 9, rgb_hex(0xF3DDA4), rgb_hex(0xF3DDA4), a);
            continue;
        }
        Rgb vc = focus ? rgb_hex(0xF3DDA4) : rgb_hex(0xE6E9EF);
        if (focus && now < captured_until) vc = rgb_hex(0x8FD19E);
        ui_draw_text(input_key_name(i), "Segoe UI Semibold", 16.5f, 600, 0, KEY_X, y + 7, vc, vc, 1);
        ui_draw_text(input_button_name(i), "Segoe UI Semibold", 16.5f, 600, 0, PAD_X, y + 7, vc, vc, 1);
    }
    const char *keys = "\xE2\x86\x91\xE2\x86\x93 select     Enter  change     Esc  back     "
                       "Esc, F-keys and the menus' own keys always work";
    ui_draw_text(keys, "Segoe UI", 12, 400, 1, LIST_X, PANEL_Y + PANEL_H - 38, rgb_hex(0x6C7385), rgb_hex(0x6C7385), 1);
}
