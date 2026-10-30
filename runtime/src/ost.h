/* The game's sound driver, played on its own (ost.c): songs for the launcher's soundtrack
 * player and the game's sound effects for its menus. */
#ifndef OST_H
#define OST_H

#include <stdbool.h>
#include <stdint.h>

#define OST_SONGS 0x22                  /* song numbers $00-$21 */

/* Driver commands: $01nn plays song nn, $02nn-$04nn play sound effects, 0 stops. */
#define OST_PLAY(song) ((uint16_t)(0x0100 | (song)))
#define OST_STOP 0
#define SFX_CURSOR  0x0256              /* the game's menu sounds */
#define SFX_CONFIRM 0x0347
#define SFX_CANCEL  0x03AA
#define SFX_ERROR   0x0253

/* Parks the game (its code stops; the driver, the Z80 and the sound chips keep their
 * state). Call once the game has set the driver up, e.g. at the title screen. */
void ost_start(void);
void ost_end(void);                     /* removes the hooks; the machine needs a reset after */

/* Queues a driver command; false if the queue is full. */
bool ost_command(uint16_t cmd);

/* Before each frame: queues the driver's once-a-frame update (as the game's V blank
 * does) and starts the queued calls; they run at the start of the frame. */
void ost_frame(void);

#endif
