/* Weather and elemental particles over the game world (fx.c), drawn by hd_post. */
#ifndef FX_H
#define FX_H

#include <stdbool.h>
#include <stdint.h>

#include "hd.h"

/* After each game frame: bright, vivid pixels of the sprites that are neither the player
 * nor the HUD (spirits, magic, fire) give off particles by colour; the particles move on.
 * The camera places them in the world. */
void fx_game_frame(const HdFrame *f, bool world, float cam_x, float cam_y);
void fx_clear(void);                    /* after a load, reset or scene change */

/* Draws the weather (settings.weather) and the particles onto a finished picture. */
void fx_draw(uint32_t *px, int w, int h, const HdPost *p);

#endif
