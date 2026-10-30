/* PNG files (png.c). */
#ifndef PNG_H
#define PNG_H

#include <stdbool.h>
#include <stdint.h>

/* Writes 0x00RRGGBB pixels (pitch in pixels) as an RGB PNG. Returns 0 on success. */
int png_write(const char *path, const uint32_t *px, int w, int h, int pitch);
/* Writes 0xAARRGGBB pixels (no padding) as an RGBA PNG. Returns 0 on success. */
int png_write_rgba(const char *path, const uint32_t *px, int w, int h);
/* Reads an 8-bit PNG (grey, RGB, palette, grey + alpha or RGBA; not interlaced) into
 * 0xAARRGGBB pixels; NULL if it cannot. Free the result with free(). */
uint32_t *png_read(const char *path, int *w, int *h);

#endif
