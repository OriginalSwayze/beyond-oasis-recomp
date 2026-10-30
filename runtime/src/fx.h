/* Elemental particles, footsteps and item animations over the game world (fx.c), drawn by hd_post. */
#ifndef FX_H
#define FX_H

#include <stdbool.h>
#include <stdint.h>

#include "hd.h"

/* After each game frame: finds the dropped items (marking their sprites), lets the spirits
 * and their magic give off particles by colour, and moves the particles on. The camera
 * places them in the world. */
void fx_game_frame(HdFrame *f, bool world, float cam_x, float cam_y);
void fx_clear(void);                    /* after a load, reset or scene change */

/* Sprite i of the newest frame belongs to a spirit or its magic (not Ali, the people,
 * items or the HUD): these give off particles, and light (lights.c). */
bool fx_light_sprite(const HdFrame *f, int i);

/* Draws the item glows and the particles onto a finished picture. */
void fx_draw(uint32_t *px, int w, int h, const HdPost *p);

#endif
