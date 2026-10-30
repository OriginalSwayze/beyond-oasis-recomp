/* HD texture packs (texpack.c): replacement images for the game's 8x8 tiles.
 *
 * A tile as the game shows it is its 32 bytes of pattern and the 16 colours of the
 * palette line it is drawn with; its key is a 64-bit hash of those, written as 16 hex
 * digits. A pack is a folder of `<key>.png` images, each a square multiple of 8 pixels
 * (8x8 to 128x128), with transparency; the renderer draws them in place of the tiles,
 * flipped as the tiles are. Dumping writes every tile drawn on screen once, as an 8x8
 * PNG with its key as the name, to start a pack from. */
#ifndef TEXPACK_H
#define TEXPACK_H

#include <stdbool.h>
#include <stdint.h>

/* Loads texture_packs/<name>/ under `base` (an empty name turns packs off). Returns the
 * number of images loaded. */
int  texpack_load(const char *base, const char *name);
void texpack_dump(const char *base, bool on);   /* texture_packs/dump/ */
bool texpack_active(void);                      /* replacing or dumping */
int  texpack_count(void);                       /* images loaded */

/* Lists the pack folders (not dump) under base into names, up to max; returns the count. */
int  texpack_list(const char *base, char names[][64], int max);

/* The replacement for tile `tile` (0-2047) drawn with palette line `pal` (0-3), as the
 * current VRAM and CRAM have it: an id (1-65535), or 0 when the pack has none. Dumps the
 * tile when dumping. Results are kept for the frame; texpack_frame() starts a new one. */
uint16_t texpack_tile(int tile, int pal);
void texpack_frame(void);

/* The replacement's pixels, size x size, 0xAARRGGBB. */
const uint32_t *texpack_texels(uint16_t id, int *size);

/* A pixel's tile reference, kept by the renderer: id, flips and the pixel in the tile. */
static inline uint32_t texpack_ref(uint16_t id, bool hflip, bool vflip, int u, int v) {
    return id ? (uint32_t)id << 8 | (hflip ? 0x80u : 0) | (vflip ? 0x40u : 0) | (uint32_t)(v & 7) << 3 | (uint32_t)(u & 7) : 0;
}

/* The replacement colour for sub-position (sx, sy) of `scale` inside the pixel `ref`
 * points at: 0 when there is no replacement, 1 when it is transparent there, else
 * 0xFF000000 | RGB. */
static inline uint32_t texpack_texel(uint32_t ref, int sx, int sy, int scale) {
    if (!ref) return 0;
    int size;
    const uint32_t *t = texpack_texels((uint16_t)(ref >> 8), &size);
    if (!t) return 0;
    int u = ref & 7, v = (ref >> 3) & 7;
    if (ref & 0x80) sx = scale - 1 - sx;
    if (ref & 0x40) sy = scale - 1 - sy;
    int tx = (u * scale + sx) * size / (8 * scale), ty = (v * scale + sy) * size / (8 * scale);
    uint32_t c = t[ty * size + tx];
    return (c >> 24) < 128 ? 1 : 0xFF000000u | (c & 0xFFFFFF);
}

#endif
