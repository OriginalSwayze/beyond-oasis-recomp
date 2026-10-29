#include "settings.h"
#include "genesis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings settings = {
    .rom_path = "",
    .widescreen = true,
    .aspect_w = 16, .aspect_h = 9,
    .fullscreen = false,
    .integer_scale = false,
    .no_sprite_limit = true,
    .window_scale = 4,
    .rewind = true,
    .rewind_mb = 64,
    .state_slot = 1,
    .show_launcher = true,
};

static bool parse_bool(const char *v) { return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on"); }

void settings_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "rom_path")) snprintf(settings.rom_path, sizeof settings.rom_path, "%s", v);
        else if (!strcmp(k, "widescreen")) settings.widescreen = parse_bool(v);
        else if (!strcmp(k, "aspect")) sscanf(v, "%d:%d", &settings.aspect_w, &settings.aspect_h);
        else if (!strcmp(k, "fullscreen")) settings.fullscreen = parse_bool(v);
        else if (!strcmp(k, "integer_scale")) settings.integer_scale = parse_bool(v);
        else if (!strcmp(k, "no_sprite_limit")) settings.no_sprite_limit = parse_bool(v);
        else if (!strcmp(k, "window_scale")) settings.window_scale = atoi(v);
        else if (!strcmp(k, "rewind")) settings.rewind = parse_bool(v);
        else if (!strcmp(k, "rewind_mb")) settings.rewind_mb = atoi(v);
        else if (!strcmp(k, "state_slot")) settings.state_slot = atoi(v);
        else if (!strcmp(k, "show_launcher")) settings.show_launcher = parse_bool(v);
    }
    fclose(f);
    if (settings.aspect_w <= 0 || settings.aspect_h <= 0) { settings.aspect_w = 16; settings.aspect_h = 9; }
    if (settings.window_scale < 1 || settings.window_scale > 10) settings.window_scale = 4;
    if (settings.rewind_mb < 8 || settings.rewind_mb > 2048) settings.rewind_mb = 64;
    if (settings.state_slot < 1 || settings.state_slot > 9) settings.state_slot = 1;
}

void settings_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Beyond Oasis Recomp settings\n");
    fprintf(f, "rom_path=%s\n", settings.rom_path);
    fprintf(f, "widescreen=%d\n", settings.widescreen);
    fprintf(f, "aspect=%d:%d\n", settings.aspect_w, settings.aspect_h);
    fprintf(f, "fullscreen=%d\n", settings.fullscreen);
    fprintf(f, "integer_scale=%d\n", settings.integer_scale);
    fprintf(f, "no_sprite_limit=%d\n", settings.no_sprite_limit);
    fprintf(f, "window_scale=%d\n", settings.window_scale);
    fprintf(f, "rewind=%d\n", settings.rewind);
    fprintf(f, "rewind_mb=%d\n", settings.rewind_mb);
    fprintf(f, "state_slot=%d\n", settings.state_slot);
    fprintf(f, "show_launcher=%d\n", settings.show_launcher);
    fclose(f);
}

int settings_extra_width(void) {
    if (!settings.widescreen) return 0;
    /* Genesis pixels are slightly wide (4:3 over 320x224), so convert the
     * display aspect to a pixel count using the 320x224 -> 4:3 pixel aspect. */
    double par = (4.0 / 3.0) / (320.0 / 224.0);
    int width = (int)(224.0 * settings.aspect_w / settings.aspect_h / par + 0.5);
    int extra = (width - 320) / 2;
    extra &= ~1;
    if (extra < 0) extra = 0;
    if (extra > MAX_EXTRA_WIDTH) extra = MAX_EXTRA_WIDTH;
    return extra;
}
