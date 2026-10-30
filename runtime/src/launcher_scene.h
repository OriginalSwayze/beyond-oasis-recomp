/* The spirits (launcher_scene.c): their colours, their glass orbs with emblems, and soft
 * light, used by the launcher and the pause menu. */
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
/* The four spirits, and the Gold Armlet (SPIRIT_ARMLET) for pages of its own, like the cheats. */
#define SPIRIT_ARMLET 4
extern const Spirit spirits[5];

/* A spirit's orb with its emblem, centred at (cx, cy) with radius r (layout units). */
void scene_orb(int spirit, float cx, float cy, float r, float t, float bright);

/* Soft additive light, radius r (layout units). */
void scene_glow(float cx, float cy, float r, Rgb c, float k);

#endif
