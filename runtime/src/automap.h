/* The auto-map (automap.c): a picture of each area, filled in from what has been on screen.
 *
 * Every few game frames the scenery under the camera (the two planes, without sprites or
 * the HUD) is copied at a quarter of its size into the current area's picture. Areas are
 * told apart by the game's map number and tile sets ($FF16F6-$FF16FD) and the map's size
 * ($FF177A/$FF177C). Kept in automap.bin next to the game. */
#ifndef AUTOMAP_H
#define AUTOMAP_H

#include <stdbool.h>

#include "hd.h"

void automap_frame(const HdFrame *f, bool world, float cam_x, float cam_y);
void automap_load(const char *base_dir);
void automap_save(const char *base_dir);

/* Draws the current area's map into the ui canvas, fitted into (x, y, w, h) layout units,
 * with Ali's place blinking. False when nothing of this area has been seen. */
bool automap_draw(float x, float y, float w, float h, float t, float alpha);

#endif
