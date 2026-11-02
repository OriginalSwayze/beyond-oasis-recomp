/* The finishing effects on the graphics card (gpu.c): lighting, the HDR look and sharpening
 * in one shader pass, with SDL's OpenGL renderer. */
#ifndef GPU_H
#define GPU_H

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "hd.h"

/* Readies the shader; false if the renderer is not OpenGL or the shader does not build
 * (the CPU then does the effects). */
bool gpu_fx_init(SDL_Renderer *ren);

/* The picture px (w x h, row pitch in pixels) with p's lighting, HDR look and sharpening,
 * in a texture to draw (valid until the next call); NULL if it could not be done. `smooth`
 * is the texture's scaling to the window (linear above native resolution). */
SDL_Texture *gpu_fx_finish(SDL_Renderer *ren, const uint32_t *px, int w, int h, int pitch, const HdPost *p, bool smooth);

/* The Direct3D 11 version (gpu_d3d11.c), which gpu_fx_init/finish use with SDL's direct3d11 renderer. */
bool d3d11_fx_init(SDL_Renderer *ren);
SDL_Texture *d3d11_fx_finish(SDL_Renderer *ren, const uint32_t *px, int w, int h, int pitch, const HdPost *p, bool smooth);

extern double gpu_fx_upload_ms;         /* the last call's time sending the picture to the card */
extern double gpu_fx_card_ms;           /* the card's own time for the effects, a picture or two ago (0 unknown) */

#endif
