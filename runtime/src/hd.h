/* High internal resolution (hd.c): the VDP keeps each line's layers, and hd_compose()
 * scales every layer on its own with an edge-directed pixel-art scaler and composites
 * them at that size, with the same priority and shadow/highlight rules as the VDP.
 * hd_compose_at() also draws the moments between the game's motion updates (smooth
 * motion at 60 Hz and above). */
#ifndef HD_H
#define HD_H

#include <stdbool.h>
#include <stdint.h>

#include "genesis.h"

#define HD_MAX_W (320 + MAX_EXTRA_WIDTH * 2)
#define HD_MAX_H 240
#define HD_MAX_SCALE 4

enum { HD_LINE_DISPLAY = 1, HD_LINE_SH = 2, HD_LINE_COLUMN_VSCROLL = 4 };
#define HD_WINDOW 0x80                  /* layer byte bit: plane A pixel from the window (does not scroll) */

/* A sprite as drawn this frame (by SAT entry), with all of its pixels, including the
 * ones hidden behind earlier sprites. */
typedef struct {
    bool drawn;
    int  rank;                          /* order in the sprite list walk: lower is in front */
    int  x, y, w, h;                    /* screen position (x from the left of the 320 area) and size */
    uint16_t attr;
    uint32_t px[32 * 32];               /* layer pixels, w x h, row pitch 32; 0 = transparent */
} HdSprite;

/* One frame of layers, written by vdp_render_line while vdp_opts.hd_capture is set.
 * Pixels are layer pixels as vdp_compose() takes them: bits 0-15 the CRAM colour,
 * bits 16-23 the layer byte (colour index, 0x40 = high priority, HD_WINDOW). */
typedef struct {
    int w, h;                           /* line width (with widescreen) and line count */
    int extra;                          /* widescreen columns on each side */
    bool mask_sides;
    uint32_t a[HD_MAX_H][HD_MAX_W];     /* plane A with the window */
    uint32_t b[HD_MAX_H][HD_MAX_W];
    uint32_t s[HD_MAX_H][HD_MAX_W];
    uint16_t backdrop[HD_MAX_H];
    uint8_t  flags[HD_MAX_H];           /* HD_LINE_* */
    int16_t  hscroll[2][HD_MAX_H];      /* plane A / B horizontal scroll per line */
    int16_t  vscroll[2][HD_MAX_H];
    HdSprite sprites[80];
    int      sprite_count;              /* sprites walked (rank counter) */
    bool     sprite_mask;               /* a masking sprite (x = 0) was used: draw sprites as captured */
} HdFrame;

extern HdFrame hd_frame;
extern unsigned hd_serial;              /* bumps when the VDP starts capturing a frame */

/* Composites the captured frame at scale (1-4) into hd_px (hd_w x hd_h, 0x00RRGGBB). */
void hd_compose(int scale);
extern uint32_t *hd_px;
extern int hd_w, hd_h;

/* Smooth motion: call hd_frame_done() after running each frame; hd_compose_at() then
 * draws the picture `frac` frames (0-1) after the newest one, gliding between the
 * game's motion updates. hd_motion_t() is how far that moment is between the updates. */
void  hd_frame_done(void);
void  hd_compose_at(int scale, float frac);
float hd_motion_t(float frac);

#endif
