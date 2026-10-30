/* Live before/after previews of the picture options (preview.c), on a captured game
 * frame: the pause menu uses the paused one, the launcher the one the game saved last. */
#ifndef PREVIEW_H
#define PREVIEW_H

#include <stdbool.h>
#include <stdint.h>

/* Where the frame was in the game world (dust and weather stay put in it). */
void preview_set_world(bool world, float cam_x, float cam_y);

/* The game saves its frame (the renderer's layers, hd_frame) next to the executable for
 * the launcher; the launcher loads it. */
void preview_save(const char *dir);
bool preview_load(const char *dir);
bool preview_ready(void);               /* a frame to preview on */

/* True for options shown as a picture: the frame without the option and with it. */
bool preview_supported(int option);

/* Draws the preview of `option` into the ui canvas at (x, y, w, h) layout units: the frame
 * without the option on the left of a dividing line that sweeps slowly across, with it on
 * the right. `t` is the time in seconds. The pictures are made again only when the option
 * or the settings change. */
void preview_draw(int option, float x, float y, float w, float h, float t);

#endif
