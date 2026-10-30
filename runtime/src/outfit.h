/* Ali's outfit colours (outfit.c): the parts of his clothing that can be recoloured, the
 * settings that hold the picks, and the page that edits them with a live preview. The
 * page is drawn over the ui canvas by the pause menu and the launcher, in their 1280x720
 * layout, like the controls page. */
#ifndef OUTFIT_H
#define OUTFIT_H

#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>

#define OUTFIT_PARTS 8                  /* trousers, sash, boots, hair, headband, armlet, skin, jewel */

/* settings.ini: outfit=0/1, outfit_colors=a,b,... (CRAM colours in hex, - = his own; the
 * older four-part list still loads) and outfit_anims=0,1,... (OUTFIT_ANIM_*). */
void outfit_defaults(void);
bool outfit_ini_load(const char *key, const char *value);
void outfit_ini_save(FILE *f);

/* Hands the settings to the renderer (vdp_opts.outfit and its colour tables). */
void outfit_apply(void);

/* The picture of Ali the page previews: the renderer's latest one in the game; the
 * launcher loads the one saved by the game (outfit_preview_save) next to the executable. */
void outfit_preview_save(const char *dir);
bool outfit_preview_load(const char *dir);

void outfit_open(void);
bool outfit_is_open(void);
/* Handles one event while the page is open; (mx, my) is the mouse position in layout
 * units for mouse events. Returns true when Apply changed the settings. */
bool outfit_event(const SDL_Event *e, float mx, float my);
void outfit_render(double time_s);
/* Called for the page's sounds (0 move, 1 change or confirm, 2 close), if set. */
extern void (*outfit_sound)(int kind);

#endif
