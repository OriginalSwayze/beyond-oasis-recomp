/* High internal resolution (hd.c): the VDP keeps each line's layers, and hd_compose()
 * scales every layer on its own with an edge-directed pixel-art scaler and composites
 * them at that size, with the same priority and shadow/highlight rules as the VDP. */
#ifndef HD_H
#define HD_H

#include <stdbool.h>
#include <stdint.h>

#include "genesis.h"

#define HD_MAX_W (320 + MAX_EXTRA_WIDTH * 2)
#define HD_MAX_H 240
#define HD_MAX_SCALE 4

/* One frame of layers, written by vdp_render_line while vdp_opts.hd_capture is set.
 * Pixels are layer pixels as vdp_compose() takes them: bits 0-15 the CRAM colour,
 * bits 16-23 the layer byte (colour index, 0x40 = high priority). */
typedef struct {
    int w, h;                           /* line width (with widescreen) and line count */
    int extra;                          /* widescreen columns on each side */
    bool mask_sides;
    uint32_t a[HD_MAX_H][HD_MAX_W];     /* plane A with the window */
    uint32_t b[HD_MAX_H][HD_MAX_W];
    uint32_t s[HD_MAX_H][HD_MAX_W];
    uint16_t backdrop[HD_MAX_H];
    uint8_t  flags[HD_MAX_H];           /* HD_LINE_* */
} HdFrame;

enum { HD_LINE_DISPLAY = 1, HD_LINE_SH = 2 };

extern HdFrame hd_frame;

/* Composites the captured frame at scale (2-4) into hd_px (hd_w x hd_h, 0x00RRGGBB). */
void hd_compose(int scale);
extern uint32_t *hd_px;
extern int hd_w, hd_h;

#endif
