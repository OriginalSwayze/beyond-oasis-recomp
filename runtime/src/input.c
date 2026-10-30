#include "input.h"

#include <string.h>

#include "genesis.h"
#include "settings.h"

_Static_assert(sizeof settings.bind_key / sizeof *settings.bind_key == IN_COUNT &&
               sizeof settings.bind_pad / sizeof *settings.bind_pad == IN_COUNT, "settings.h binding arrays");

const char *const input_labels[IN_COUNT] = {
    "Up", "Down", "Left", "Right", "A button", "B button", "C button", "Start", "Rewind (hold)", "Pause menu",
};

/* settings.ini names. */
static const char *const ini_names[IN_COUNT] = {
    "up", "down", "left", "right", "a", "b", "c", "start", "rewind", "menu",
};

static const uint16_t pad_bit[IN_COUNT] = {
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_A, PAD_B, PAD_C, PAD_START, 0, 0,
};

void input_defaults(void) {
    static const SDL_Scancode keys[IN_COUNT] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_RETURN,
        SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_ESCAPE,
    };
    /* Xbox layout: the Genesis A, B, C row sits on X, A, B. */
    static const SDL_GameControllerButton buttons[IN_COUNT] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_BACK,
    };
    for (int i = 0; i < IN_COUNT; i++) { settings.bind_key[i] = keys[i]; settings.bind_pad[i] = buttons[i]; }
}

bool input_ini_load(const char *key, const char *value) {
    for (int i = 0; i < IN_COUNT; i++) {
        if (!strncmp(key, "key_", 4) && !strcmp(key + 4, ini_names[i])) {
            SDL_Scancode s = SDL_GetScancodeFromName(value);
            if (s != SDL_SCANCODE_UNKNOWN) settings.bind_key[i] = s;
            return true;
        }
        if (!strncmp(key, "pad_", 4) && !strcmp(key + 4, ini_names[i])) {
            SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(value);
            if (b != SDL_CONTROLLER_BUTTON_INVALID) settings.bind_pad[i] = b;
            return true;
        }
    }
    return false;
}

void input_ini_save(FILE *f) {
    fprintf(f, "# Controls: keyboard key and controller button for each action\n");
    for (int i = 0; i < IN_COUNT; i++) {
        const char *b = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)settings.bind_pad[i]);
        fprintf(f, "key_%s=%s\n", ini_names[i], SDL_GetScancodeName((SDL_Scancode)settings.bind_key[i]));
        fprintf(f, "pad_%s=%s\n", ini_names[i], b ? b : "");
    }
}

bool input_held(int action, const Uint8 *keys, SDL_GameController *pad) {
    int k = settings.bind_key[action];
    if (keys && k > 0 && k < SDL_NUM_SCANCODES && keys[k]) return true;
    return pad && SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)settings.bind_pad[action]);
}

bool input_event_is(int action, const SDL_Event *e) {
    if (e->type == SDL_KEYDOWN) return !e->key.repeat && (int)e->key.keysym.scancode == settings.bind_key[action];
    if (e->type == SDL_CONTROLLERBUTTONDOWN) return e->cbutton.button == settings.bind_pad[action];
    return false;
}

uint16_t input_pad_bits(const Uint8 *keys, SDL_GameController *pad) {
    uint16_t p = 0;
    for (int i = 0; i < IN_COUNT; i++) if (pad_bit[i] && input_held(i, keys, pad)) p |= pad_bit[i];
    if (pad) {                          /* the left stick moves too, whatever the bindings */
        const int dead = 12000;
        int lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (ly < -dead) p |= PAD_UP;
        if (ly > dead) p |= PAD_DOWN;
        if (lx < -dead) p |= PAD_LEFT;
        if (lx > dead) p |= PAD_RIGHT;
    }
    return p;
}

const char *input_key_name(int action) {
    const char *n = SDL_GetScancodeName((SDL_Scancode)settings.bind_key[action]);
    if (!n || !*n) return "(none)";
    if (!strcmp(n, "Return")) return "Enter";
    return n;
}

/* Xbox-style names for the buttons. */
const char *input_button_name(int action) {
    static const char *const names[SDL_CONTROLLER_BUTTON_MAX] = {
        "A", "B", "X", "Y", "Back", "Guide", "Start", "Left stick", "Right stick", "LB", "RB",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Share", "Paddle 1", "Paddle 2", "Paddle 3",
        "Paddle 4", "Touchpad",
    };
    int b = settings.bind_pad[action];
    return b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX && names[b] ? names[b] : "(none)";
}

void input_bind_key(int action, SDL_Scancode key) {
    for (int i = 0; i < IN_COUNT; i++)
        if (i != action && settings.bind_key[i] == (int)key) settings.bind_key[i] = settings.bind_key[action];
    settings.bind_key[action] = key;
}

void input_bind_button(int action, SDL_GameControllerButton button) {
    for (int i = 0; i < IN_COUNT; i++)
        if (i != action && settings.bind_pad[i] == (int)button) settings.bind_pad[i] = settings.bind_pad[action];
    settings.bind_pad[action] = button;
}
