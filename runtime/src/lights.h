/* Dynamic lighting with ray-traced soft shadows (lights.c), applied by hd_post. */
#ifndef LIGHTS_H
#define LIGHTS_H

#include <stdbool.h>
#include <stdint.h>

#include "hd.h"

/* Cells of the light map: LIGHT_CELL x LIGHT_CELL frame pixels. */
#define LIGHT_CELL 2
#define LIGHT_MAP_W (HD_MAX_W / LIGHT_CELL)
#define LIGHT_MAP_H (HD_MAX_H / LIGHT_CELL)

/* After each game frame (with settings.dynlight): finds the lights (the spirits and their
 * magic, and in dark scenes the torches and fires of the background), traces their light
 * past the characters onto the light map and works out how dark the scene is. t: seconds. */
void lights_game_frame(const HdFrame *f, bool world, float t);
void lights_clear(void);                /* after a load, reset or scene change */

/* The light map of the newest game frame, or NULL when it would change nothing. Each cell
 * holds 4 floats: the light's colour and strength (r, g, b, 0 and up) and how much of the
 * picture's own light the cell keeps (ambient times the shadows). *w, *h: cells. */
const float *lights_map(int *w, int *h);
int lights_count(void);                 /* lights in the newest map (for tests) */

/* For the settings' preview: the map with one fire spirit's light at (x, y) frame pixels
 * (the next game frame makes its own map again). */
void lights_preview(const HdFrame *f, float x, float y);

/* For tests:a light of this colour (0-1) at (x, y) frame pixels, in every frame (radius 0: none). */
void lights_test(float x, float y, float r, float g, float b, float radius);

#endif
