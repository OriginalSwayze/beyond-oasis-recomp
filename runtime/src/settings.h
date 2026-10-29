/* settings.ini next to the executable; shared by the launcher and the in-game menu. */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

typedef struct {
    char rom_path[1024];
    bool widescreen;
    int  aspect_w, aspect_h;            /* target aspect ratio when widescreen is on */
    bool fullscreen;
    bool integer_scale;
    bool no_sprite_limit;
    int  window_scale;
} Settings;

extern Settings settings;

void settings_load(const char *path);
void settings_save(const char *path);
/* Widescreen pixels to add on each side for the current settings. */
int  settings_extra_width(void);

#endif
