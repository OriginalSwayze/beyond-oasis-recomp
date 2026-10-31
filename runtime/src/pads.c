/* Controllers: see pads.h.
 *
 * SDL announces the controllers connected at start only once, so whoever opens them keeps
 * them: the launcher and the game share this list instead of each opening its own. A
 * controller that disconnects (a wireless one sleeping, Bluetooth dropping) is closed by
 * its instance id, and opened again when it comes back. */
#include "pads.h"

#define MAX_PADS 8

static SDL_GameController *pads[MAX_PADS];
static SDL_JoystickID ids[MAX_PADS];
static int count;
static int active = -1;                 /* index of the one used last */

static int find(SDL_JoystickID id) {
    for (int i = 0; i < count; i++) if (ids[i] == id) return i;
    return -1;
}

static void open_device(int device) {
    if (!SDL_IsGameController(device)) return;
    if (find(SDL_JoystickGetDeviceInstanceID(device)) >= 0 || count >= MAX_PADS) return;
    SDL_GameController *c = SDL_GameControllerOpen(device);
    if (!c) return;
    pads[count] = c;
    ids[count] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    active = count++;                   /* the one just connected is the one to use */
}

static void close_id(SDL_JoystickID id) {
    int i = find(id);
    if (i < 0) return;
    SDL_GameControllerClose(pads[i]);
    pads[i] = pads[count - 1];
    ids[i] = ids[count - 1];
    count--;
    if (active == count) active = i;    /* the last one moved into the gap */
    if (active >= count) active = count - 1;
}

void pads_init(void) {
    SDL_GameControllerEventState(SDL_ENABLE);
    for (int d = 0; d < SDL_NumJoysticks(); d++) open_device(d);
}

void pads_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED: open_device(e->cdevice.which); break;
    case SDL_CONTROLLERDEVICEREMOVED: close_id(e->cdevice.which); break;
    case SDL_CONTROLLERBUTTONDOWN: { int i = find(e->cbutton.which); if (i >= 0) active = i; break; }
    case SDL_CONTROLLERAXISMOTION: {
        int i = find(e->caxis.which);
        if (i >= 0 && (e->caxis.value > 16000 || e->caxis.value < -16000)) active = i;
        break;
    }
    default: break;
    }
}

SDL_GameController *pads_active(void) {
    if (active >= 0 && active < count && SDL_GameControllerGetAttached(pads[active])) return pads[active];
    for (int i = 0; i < count; i++) if (SDL_GameControllerGetAttached(pads[i])) return pads[i];
    return NULL;
}

void pads_rumble(float low, float high, int ms) {
    SDL_GameController *c = pads_active();
    if (!c) return;
    Uint16 lo = (Uint16)(low < 0 ? 0 : low > 1 ? 65535 : low * 65535), hi = (Uint16)(high < 0 ? 0 : high > 1 ? 65535 : high * 65535);
    SDL_GameControllerRumble(c, lo, hi, (Uint32)ms);
}
