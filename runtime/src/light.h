/* Dynamic light (light.c): fire, magic and the spirits light up what is around them, and
 * dark places dim a little so that light shows. */
#ifndef LIGHT_H
#define LIGHT_H

#include <stdint.h>

#include "hd.h"

/* The picture src (w x h, row pitch `pitch`, p->scale picture pixels per game pixel) lit by
 * the game frame's lights, in a buffer of its own (row pitch w, valid until the next call);
 * NULL when there is nothing to do (dynamic light off, not the game world, or a picture
 * that is not of the captured frame). */
uint32_t *light_apply(const uint32_t *src, int w, int h, int pitch, const HdPost *p);

#endif
