/* Display filters (filter.c): scanlines, CRT and LCD looks drawn on the GPU with
 * SDL's renderer: a multiply overlay, an additive glow and a curved mesh. */
#ifndef FILTER_H
#define FILTER_H

#include <SDL.h>

enum { FILTER_OFF, FILTER_SCANLINES, FILTER_CRT, FILTER_LCD, FILTER_COUNT };

/* Draws src (NULL = the whole texture) of frame into dst with the filter. src_w x src_h
 * is the game's own resolution there, so lines and pixel grids line up with it. */
void filter_draw(SDL_Renderer *ren, SDL_Texture *frame, const SDL_Rect *src, SDL_Rect dst,
                 int src_w, int src_h, int mode);

/* The overlay as 0x00RRGGBB multipliers (w x h), for headless screenshots. */
const uint32_t *filter_overlay(int w, int h, int src_w, int src_h, int mode);

#endif
