/* Elemental particles and footsteps over the game world (fx.c), drawn by hd_post. */
#ifndef FX_H
#define FX_H

#include <stdbool.h>
#include <stdint.h>

#include "hd.h"

/* After each game frame: lets the spirits and their magic give off particles by colour
 * (not Ali, the people, dropped items or the HUD), and moves the particles on. The camera
 * places them in the world. */
void fx_game_frame(HdFrame *f, bool world, float cam_x, float cam_y);
void fx_clear(void);                    /* after a load, reset or scene change */

/* Draws the particles onto a finished picture. */
void fx_draw(uint32_t *px, int w, int h, const HdPost *p);

#endif
