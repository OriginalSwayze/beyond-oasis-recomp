/* Motion from the game's own data (motion.c): while the game builds its sprite list, the
 * runtime notes which object every sprite belongs to and where the game placed that
 * object on screen. The pictures between two game ticks then move each sprite by its
 * own object's movement (hd.c), with nothing guessed from the picture. */
#ifndef MOTION_H
#define MOTION_H

#include <stdint.h>

#include "hd.h"

/* Called by machine.c before every native block, with its address. */
void motion_block(uint32_t pc);

/* Forgets the ticks seen so far (after loading a state or rewinding). */
void motion_reset(void);

/* For each sprite drawn in f: how far (pixels) its object moved on screen since the
 * previous tick, in dx/dy (0 for sprites without an object, new objects and jumps).
 * Returns the tick whose sprite list f shows (0 if none matches). owned (optional)
 * gets the number of drawn sprites that belong to an object. */
unsigned motion_sprite_moves(const HdFrame *f, int dx[80], int dy[80], int *owned);

/* Marks the entries of the VDP's sprite table (`sat`, `count` entries of 8 bytes) that
 * the player character's object drew in the latest tick that sent them. */
void motion_player_entries(const uint8_t *sat, int count, bool out[80]);

#endif
