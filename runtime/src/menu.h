/* In-game menu (menu.c): drawn into the ui canvas while the game is paused. */
#ifndef MENU_H
#define MENU_H

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MENU_NONE,
    MENU_CLOSE,                         /* resume */
    MENU_SETTINGS,                      /* a setting changed: apply it */
    MENU_LOADED,                        /* a save state was loaded: resume */
    MENU_RESET,
    MENU_QUIT,
} MenuAction;

/* Opens the menu over the paused frame fb (fb_w x fb_h, pitch in pixels), which the
 * window shows in the rectangle game_x/y/w/h of an out_w x out_h output.
 * window_to_pixels converts mouse coordinates to output pixels. */
void menu_open(const uint32_t *fb, int fb_w, int fb_h, int fb_pitch, int game_x, int game_y, int game_w, int game_h,
               int out_w, int out_h, float window_to_pixels);
void menu_close(void);
bool menu_is_open(void);
MenuAction menu_event(const SDL_Event *e);
void menu_render(double time_s);        /* into ui_px (ui_w x ui_h) */

#endif
