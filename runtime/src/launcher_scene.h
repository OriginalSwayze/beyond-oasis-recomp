/* The launcher's shared pieces (launcher_scene.c): the captured artwork, the tablet's place
 * and soft light, used by the launcher and the pause menu. */
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

/* Soft additive light, radius r (layout units). */
void scene_glow(float cx, float cy, float r, Rgb c, float k);

#endif
