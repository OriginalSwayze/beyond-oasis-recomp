/* The steelbook look (foil.c): surfaces of dark brushed steel with embossed artwork, raised
 * holographic foil and gold, lit by a light that drifts and follows the mouse like tilting
 * a collector's case, and the dark, misty atmosphere around them. Shared by the launcher
 * and the pause menu; everything is in the ui canvas's 1280x720 layout units. */
#ifndef FOIL_H
#define FOIL_H

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"
#include "launcher_scene.h"             /* Art */

enum { FOIL_STEEL, FOIL_HOLO, FOIL_GOLD, FOIL_ART, FOIL_DARK };

typedef struct {
    float x, y, w, h, r;                /* layout rectangle and corner radius */
    float built_s;                      /* ui_s it was built at (0: not built) */
    int sw, sh;                         /* size in pixels */
    float *height;                      /* emboss height field */
    uint8_t *mat, *cov, *brush;
    uint32_t *color;                    /* art colours, for FOIL_GOLD and FOIL_ART */
    float *nx, *ny, *nz;                /* surface normals, made by foil_finish() */
    Rgb tint;                           /* the steel's colour */
} FoilSurface;

/* Starts a surface over (x, y, w, h) with rounded corners and a bevelled edge. Returns
 * false if it is already built at the current canvas scale (nothing to do), so callers
 * can write `if (foil_begin(...)) { ...place things...; foil_finish(s); }`. */
bool foil_begin(FoilSurface *s, float x, float y, float w, float h, float r, Rgb tint);
/* A raised double frame of holographic foil `inset` units in from the edge. */
void foil_frame(FoilSurface *s, float inset, float r);
/* A raised foil rule from (x0, y) to (x1, y). */
void foil_rule(FoilSurface *s, float x0, float x1, float y, float thick);
/* An engraved, darker recess (for text to sit on). */
void foil_recess(FoilSurface *s, float x, float y, float w, float h, float r, float depth);
/* A raised foil ring (a gem socket) centred at (cx, cy). */
void foil_ring(FoilSurface *s, float cx, float cy, float r, float thick);
/* Artwork embossed into the steel, `width` units wide, centred at cx with its top at top;
 * material FOIL_GOLD (shaded as polished gold) or FOIL_ART (its own colours). Returns its
 * bottom edge. */
float foil_art(FoilSurface *s, const Art *a, float cx, float top, float width, int material, float lift, float detail);
/* Text stamped into the steel (depth > 0 engraves, < 0 raises), centred at (cx, cy). */
void foil_text(FoilSurface *s, const char *text, const char *face, float px, int weight, float tracking,
               float cx, float cy, float depth, int material);
/* Works out the lighting normals; call once after placing everything. */
void foil_finish(FoilSurface *s);
/* Shades the surface into ui_px for the light direction (lx, ly, about -1..1) at time t. */
void foil_draw(const FoilSurface *s, float lx, float ly, float t);
void foil_free(FoilSurface *s);

/* The light: drifting on its own, leaning towards the mouse (layout units) when it moved
 * in the last few seconds (mouse_t: when, or a negative value for never). */
void foil_light(float t, float mouse_x, float mouse_y, float mouse_t, float *lx, float *ly);

/* The dark backdrop (night sky over the dunes, a warm glow low on the left), drawn into
 * ui_px; built once per canvas size, when it returns true: then add the shadows. */
bool foil_backdrop(float t);
/* A soft shadow under a panel at (x, y, w, h), baked into the backdrop (and ui_px). */
void foil_shadow(float x, float y, float w, float h, float r);
/* The same, into a ui-sized buffer of the caller's. */
void foil_shadow_into(uint32_t *buf, float x, float y, float w, float h, float r);
/* Mist drifting low over what is in ui_px, and embers rising (strength 0-1). */
void foil_atmosphere(float t, float strength);

#endif
