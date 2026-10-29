/* Built-in cheats (cheats.c): RAM values held every frame. */
#ifndef CHEATS_H
#define CHEATS_H

#include <stdbool.h>
#include <stdint.h>

/* Raw codes: "FFxxxx:VV" (byte) or "FFxxxx:VVVV" (word), comma-separated. Returns
 * the number of codes parsed. They are held every frame until cheats_clear_codes(). */
int  cheats_add_codes(const char *spec);
void cheats_clear_codes(void);

/* Called once per frame before the game runs: applies the enabled cheats. */
void cheats_frame(void);

#endif
