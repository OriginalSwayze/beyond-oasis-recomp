/* The launcher's scene (launcher_scene.c): a desert at dusk with the Gold Armlet in the
 * middle and the four spirits circling it, and the stone tablet the settings sit on. */
#ifndef LAUNCHER_SCENE_H
#define LAUNCHER_SCENE_H

#include <stdint.h>

#include "ui.h"

/* Artwork captured from the user's ROM (launcher.c). */
typedef struct { int w, h; uint32_t *rgb; uint8_t *mask; } Art;

/* The tablet, in layout units. */
#define TABLET_X 652
#define TABLET_Y 28
#define TABLET_W 588
#define TABLET_H 664

/* The spirits, in tab order: Dytto (water), Efreet (fire), Bow (plant), Shade (shadow). */
typedef struct { const char *name, *element; Rgb light, deep; } Spirit;
extern const Spirit spirits[4];

/* Builds the parts that only change with the canvas size (sky, dunes, tablet, the art at
 * its size). Call after ui_begin(). */
void scene_layout(const Art *armlet, const Art *logo);

/* Draws the scene for time t; `focus` (0-3, fractional while turning) is the spirit
 * brought to the front, `glow` (0-1) how brightly it shines. */
void scene_draw(float t, float focus, float glow);

/* A spirit's orb with its emblem, centred at (cx, cy) with radius r (layout units). */
void scene_orb(int spirit, float cx, float cy, float r, float t, float bright);

/* Soft additive light, radius r (layout units). */
void scene_glow(float cx, float cy, float r, Rgb c, float k);

#endif
