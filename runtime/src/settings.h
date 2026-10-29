/* settings.ini next to the executable; shared by the launcher and the in-game menu. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char rom_path[1024];
    bool widescreen;
    int  aspect_w, aspect_h;            /* target aspect ratio when widescreen is on */
    bool fullscreen;
    bool integer_scale;
    bool no_sprite_limit;
    int  window_scale;
    bool rewind;                        /* keep a rewind buffer (hold the rewind key to step back) */
    int  rewind_mb;                     /* rewind buffer size; frames cost ~1 KB, so 64 MB is many minutes */
    int  state_slot;                    /* save state slot, 1-9 */
    bool show_launcher;                 /* show the launcher at startup (--no-launcher skips it once) */
} Settings;

extern Settings settings;

void settings_load(const char *path);
void settings_save(const char *path);
/* Options the launcher and the in-game menu show: a value label, and stepping it. */
enum { OPT_WIDESCREEN, OPT_ASPECT, OPT_FULLSCREEN, OPT_INTEGER, OPT_WINDOW, OPT_FLICKER, OPT_REWIND, OPT_REWIND_MB,
       OPT_LAUNCHER };
void settings_option_text(int id, char *out, size_t n);
void settings_option_change(int id, int dir);
int  settings_cycle(int v, int count, int dir);     /* (v + dir) wrapped into 0..count-1 */

/* Widescreen pixels to add on each side for the current settings. */
int  settings_extra_width(void);

#endif
