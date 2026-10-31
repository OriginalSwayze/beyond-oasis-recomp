/* Combat info, rumble and stats (combat.c), from the health of the game's objects.
 *
 * After each game frame the enemies' health (+$8A of the main list's objects from slot 4),
 * Ali's and the spirit's slot are compared with the frame before. A drop in an enemy's
 * health shows its damage rising from it and its health bar; it also rumbles the
 * controller and counts in the stats, as do Ali's hits and the spirits' summons. */
#ifndef COMBAT_H
#define COMBAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hd.h"

/* After a game frame. `live` is false while rewinding: nothing is counted or felt then. */
void combat_frame(const HdFrame *f, bool world, float cam_x, float cam_y, bool live);
void combat_clear(void);                /* after a load, reset or scene change */

/* Whether combat_draw has anything to draw (numbers, bars or the timer). */
bool combat_active(const HdPost *p);
/* Draws the numbers, bars and timer onto a finished picture. */
void combat_draw(uint32_t *px, int w, int h, const HdPost *p);

/* Stats: kept in stats.ini next to the game, over all sessions. */
void combat_stats_load(const char *base_dir);
void combat_stats_save(const char *base_dir);
/* The stats as lines of "label\tvalue", for the pause menu's card. */
int  combat_stats_lines(char lines[][64], int max);

#endif
