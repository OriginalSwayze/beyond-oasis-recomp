/* High internal resolution (hd.c): the VDP keeps each line's layers, and hd_compose()
 * scales every layer on its own with an edge-directed pixel-art scaler and composites
 * them at that size, with the same priority and shadow/highlight rules as the VDP.
 * hd_compose_at() also draws the moments between the game's ticks (smooth motion at
 * 60 Hz and above), moving each sprite by its own object's movement (motion.c). */
#ifndef HD_H
#define HD_H

#include <stdbool.h>
#include <stdint.h>

#include "genesis.h"

#define HD_MAX_W (320 + MAX_EXTRA_WIDTH * 2)
#define HD_MAX_H 240
#define HD_MAX_SCALE 6                  /* 6x: 2544 x 1344, for 1440p and 4K screens */

enum { HD_LINE_DISPLAY = 1, HD_LINE_SH = 2, HD_LINE_COLUMN_VSCROLL = 4 };
#define HD_WINDOW 0x80                  /* layer byte bit: plane A pixel from the window (does not scroll) */

/* A sprite as drawn this frame (by SAT entry), with all of its pixels, including the
 * ones hidden behind earlier sprites. */
typedef struct {
    bool drawn;
    int  rank;                          /* position in the sprite list walk: lower is in front */
    int  x, y, w, h;                    /* screen position (x from the left of the 320 area) and size */
    uint16_t attr;
    bool player;                        /* drawn by the player character's object */
    bool item;                          /* a dropped item (fx.c decides, after each game frame) */
    uint32_t px[32 * 32];               /* layer pixels, w x h, row pitch 32; 0 = transparent */
    uint32_t ref[32 * 32];              /* their tile references, with vdp_opts.tile_refs (texpack.h) */
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
    uint32_t ta[HD_MAX_H][HD_MAX_W];    /* tile references of a, b and s, with vdp_opts.tile_refs */
    uint32_t tb[HD_MAX_H][HD_MAX_W];
    uint32_t ts[HD_MAX_H][HD_MAX_W];
    uint16_t backdrop[HD_MAX_H];
    uint8_t  flags[HD_MAX_H];           /* HD_LINE_* */
    int16_t  hscroll[2][HD_MAX_H];      /* plane A / B horizontal scroll per line */
    int16_t  vscroll[2][HD_MAX_H];
    HdSprite sprites[80];
    int      sprite_count;              /* sprites drawn this frame */
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
 * game's ticks. hd_motion_t() is how far that moment is between the sprites' ticks. */
void  hd_frame_done(void);
void  hd_compose_at(int scale, float frac);
float hd_motion_t(float frac);
/* The newest frame drawn sprite by sprite, as smooth motion draws it, with nothing moving
 * (for checking it against hd_compose()). */
void  hd_compose_sprites(int scale);
extern double hd_ms[4];                 /* last hd_compose_at: ms scaling layers, scaling sprites, sprite layer, compositing */
extern int hd_owned;                    /* sprites of the newest frame that belong to a game object */

/* Scaler for the layers: 0 edge (Scale2x / Scale3x), 1 smooth (xBR rules, picking whole
 * source pixels like Scale2x so layers, priorities and transparency stay exact). */
extern int hd_scaler;


/* A texture pack is loaded: replaced tiles are drawn from their images (texpack.h). */
extern bool hd_texpack;
extern float hd_time;                   /* seconds, for animations drawn with the layers */
/* Water shimmers, foams, glints and reflects the sprites above it (while compositing). */
extern bool hd_water;
/* Dropped items float: their sprites are drawn a little higher, bobbing (fx.c finds them). */
extern bool hd_items;

/* The finishing steps on the picture before it goes to the GPU (hd_post): dynamic lights,
 * glow, particles, sharpening. */
typedef struct {
    int   sharpen;                      /* 0-3 */
    int   lighting;                     /* 0 off, 1 soft, 2 bright: bright pixels glow */
    bool  elements;                     /* particles from the spirits and their magic (fx.c) */
    bool  footsteps;                    /* dust, grass and splashes where Ali walks (fx.c) */
    bool  items;                        /* dropped items bob and sparkle (fx.c) */
    bool  dynlight;                     /* the light map of lights.c: lights and soft shadows */
    bool  world;                        /* the picture shows the game world (not a menu or story scene) */
    float cam_x, cam_y;                 /* the game's camera, in game pixels (particles stay put in the world) */
    float t;                            /* seconds, for animations */
    int   scale;                        /* picture pixels per game pixel */
    int   extra;                        /* widescreen game pixels on each side */
} HdPost;
/* Returns the finished picture (row pitch w): src itself when nothing is on, else a copy
 * valid until the next call. */
const uint32_t *hd_post(const uint32_t *src, int w, int h, int pitch, const HdPost *p);

/* Sharpening (level 1-3: light, medium, strong) of a w x h picture with row pitch
 * `pitch`, contrast-adaptive: each pixel is pushed away from its four neighbours by an
 * amount that shrinks where the contrast is already high, so soft edges (scaled layers,
 * the smooth final stretch) get crisper while hard pixel edges get no halos. Returns the
 * sharpened copy (row pitch w), valid until the next call. */
const uint32_t *hd_sharpen(const uint32_t *src, int w, int h, int pitch, int level);

#endif
