/* PNG files (png.c). */
#ifndef PNG_H
#define PNG_H

#include <stdint.h>

/* Writes 0x00RRGGBB pixels (pitch in pixels) as an RGB PNG. Returns 0 on success. */
int png_write(const char *path, const uint32_t *px, int w, int h, int pitch);

#endif
