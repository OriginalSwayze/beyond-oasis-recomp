/* Controls page (controls.c): rebinds keyboard keys and controller buttons. Drawn over
 * the ui canvas by the pause menu and the launcher, in their 1280x720 layout. */
#ifndef CONTROLS_H
#define CONTROLS_H

#include <SDL.h>
#include <stdbool.h>

void controls_open(void);
bool controls_is_open(void);
/* Handles one event while the page is open; (mx, my) is the mouse position in layout
 * units for mouse events. Returns true when a binding or the defaults changed. */
bool controls_event(const SDL_Event *e, float mx, float my);
void controls_render(double time_s);

#endif
