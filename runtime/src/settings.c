#include "settings.h"
#include "outfit.h"
#include "genesis.h"
#include "filter.h"
#include "input.h"
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
    .internal_scale = 3,
    .smooth_motion = true,
    .filter = FILTER_OFF,
    .overclock = 100,
    .window_scale = 4,
    .rewind = true,
    .rewind_mb = 64,
    .state_slot = 1,
    .show_launcher = true,
    .launcher_music = true,
    .ost_track = -1,
    .volume = 100,
};

static const char *filter_keys[FILTER_COUNT] = { "off", "scanlines", "crt", "lcd" };
static const char *filter_names[FILTER_COUNT] = { "Off", "Scanlines", "CRT", "LCD" };

static bool parse_bool(const char *v) { return !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on"); }

void settings_load(const char *path) {
    input_defaults();
    outfit_defaults();
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
        else if (!strcmp(k, "internal_scale")) settings.internal_scale = atoi(v);
        else if (!strcmp(k, "smooth_motion")) settings.smooth_motion = parse_bool(v);
        else if (!strcmp(k, "overclock")) settings.overclock = atoi(v);
        else if (!strcmp(k, "filter")) {
            for (int i = 0; i < FILTER_COUNT; i++) if (!strcmp(v, filter_keys[i])) settings.filter = i;
        }
        else if (!strcmp(k, "window_scale")) settings.window_scale = atoi(v);
        else if (!strcmp(k, "rewind")) settings.rewind = parse_bool(v);
        else if (!strcmp(k, "rewind_mb")) settings.rewind_mb = atoi(v);
        else if (!strcmp(k, "state_slot")) settings.state_slot = atoi(v);
        else if (!strcmp(k, "show_launcher")) settings.show_launcher = parse_bool(v);
        else if (!strcmp(k, "launcher_music")) settings.launcher_music = parse_bool(v);
        else if (!strcmp(k, "volume")) settings.volume = atoi(v);
        else if (!strcmp(k, "ost_mode")) settings.ost_mode = atoi(v);
        else if (!strcmp(k, "sharpen")) settings.sharpen = atoi(v);
        else if (!strcmp(k, "scaler")) settings.scaler = !strcmp(v, "smooth");
        else if (!strcmp(k, "lighting")) settings.lighting = atoi(v);
        else if (!strcmp(k, "ambience")) settings.ambience = parse_bool(v);
        else if (!strcmp(k, "audio")) settings.audio = atoi(v);
        else if (!strcmp(k, "ost_track")) settings.ost_track = atoi(v);
        else if (input_ini_load(k, v)) {}
        else if (outfit_ini_load(k, v)) {}
        else if (!strcmp(k, "cheat_health")) settings.cheat_health = parse_bool(v);
        else if (!strcmp(k, "cheat_spirit")) settings.cheat_spirit = parse_bool(v);
        else if (!strcmp(k, "cheat_weapons")) settings.cheat_weapons = parse_bool(v);
        else if (!strcmp(k, "cheat_codes")) snprintf(settings.cheat_codes, sizeof settings.cheat_codes, "%s", v);
    }
    fclose(f);
    if (settings.aspect_w <= 0 || settings.aspect_h <= 0) { settings.aspect_w = 16; settings.aspect_h = 9; }
    if (settings.window_scale < 1 || settings.window_scale > 10) settings.window_scale = 4;
    if (settings.internal_scale < 1 || settings.internal_scale > 4) settings.internal_scale = 3;
    if (settings.filter < 0 || settings.filter >= FILTER_COUNT) settings.filter = FILTER_OFF;
    if (settings.overclock < 100 || settings.overclock > 400) settings.overclock = 100;
    if (settings.volume < 0 || settings.volume > 100) settings.volume = 100;
    if (settings.ost_mode < 0 || settings.ost_mode >= OST_MODE_COUNT) settings.ost_mode = OST_MODE_ALL;
    if (settings.sharpen < 0 || settings.sharpen > 3) settings.sharpen = 0;
    if (settings.lighting < 0 || settings.lighting > 2) settings.lighting = 0;
    if (settings.audio < 0 || settings.audio > 2) settings.audio = 0;
    if (settings.ost_track < -1 || settings.ost_track > 0xFF) settings.ost_track = -1;
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
    fprintf(f, "internal_scale=%d\n", settings.internal_scale);
    fprintf(f, "smooth_motion=%d\n", settings.smooth_motion);
    fprintf(f, "filter=%s\n", filter_keys[settings.filter]);
    fprintf(f, "overclock=%d\n", settings.overclock);
    fprintf(f, "window_scale=%d\n", settings.window_scale);
    fprintf(f, "rewind=%d\n", settings.rewind);
    fprintf(f, "rewind_mb=%d\n", settings.rewind_mb);
    fprintf(f, "state_slot=%d\n", settings.state_slot);
    fprintf(f, "show_launcher=%d\n", settings.show_launcher);
    fprintf(f, "launcher_music=%d\n", settings.launcher_music);
    fprintf(f, "volume=%d\n", settings.volume);
    fprintf(f, "ost_mode=%d\n", settings.ost_mode);
    fprintf(f, "sharpen=%d\n", settings.sharpen);
    fprintf(f, "scaler=%s\n", settings.scaler ? "smooth" : "edge");
    fprintf(f, "lighting=%d\n", settings.lighting);
    fprintf(f, "ambience=%d\n", settings.ambience);
    fprintf(f, "audio=%d\n", settings.audio);
    fprintf(f, "ost_track=%d\n", settings.ost_track);
    fprintf(f, "cheat_health=%d\n", settings.cheat_health);
    fprintf(f, "cheat_spirit=%d\n", settings.cheat_spirit);
    fprintf(f, "cheat_weapons=%d\n", settings.cheat_weapons);
    fprintf(f, "# Extra RAM codes held every frame: FFxxxx:VV (byte) or FFxxxx:VVVV (word), comma-separated\n");
    fprintf(f, "cheat_codes=%s\n", settings.cheat_codes);
    input_ini_save(f);
    outfit_ini_save(f);
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

/* ---- Options shown by the launcher and the in-game menu ---------------------------- */
static const int aspects[][2] = { { 16, 10 }, { 16, 9 }, { 2, 1 } };
static const int rewind_sizes[] = { 32, 64, 128, 256, 512 };
static const int overclocks[] = { 100, 150, 200, 300 };
static const char *sharpen_names[4] = { "Off", "Light", "Medium", "Strong" };
static const char *ost_modes[OST_MODE_COUNT] = { "Play all", "Repeat track", "Shuffle" };
static const char *cheat_presets[] = { "Off", "Health", "Health + spirit", "All" };

/* Index of the cheat preset matching the current toggles, or -1 for another mix. */
static int cheat_preset(void) {
    int h = settings.cheat_health, s = settings.cheat_spirit, w = settings.cheat_weapons;
    if (!h && !s && !w) return 0;
    if (h && !s && !w) return 1;
    if (h && s && !w) return 2;
    if (h && s && w) return 3;
    return -1;
}

void settings_option_text(int id, char *out, size_t n) {
    switch (id) {
    case OPT_WIDESCREEN: snprintf(out, n, "%s", settings.widescreen ? "On" : "Off"); break;
    case OPT_ASPECT:     snprintf(out, n, "%d:%d", settings.aspect_w, settings.aspect_h); break;
    case OPT_FULLSCREEN: snprintf(out, n, "%s", settings.fullscreen ? "On" : "Off"); break;
    case OPT_INTEGER:    snprintf(out, n, "%s", settings.integer_scale ? "On" : "Off"); break;
    case OPT_WINDOW:     snprintf(out, n, "%d\xC3\x97", settings.window_scale); break;
    case OPT_FLICKER:    snprintf(out, n, "%s", settings.no_sprite_limit ? "On" : "Off"); break;
    case OPT_SMOOTH:     snprintf(out, n, "%s", settings.smooth_motion ? "On" : "Off"); break;
    case OPT_FILTER:     snprintf(out, n, "%s", filter_names[settings.filter]); break;
    case OPT_VOLUME:     snprintf(out, n, "%d%%", settings.volume); break;
    case OPT_MUSIC:      snprintf(out, n, "%s", settings.launcher_music ? "On" : "Off"); break;
    case OPT_OST_MODE:   snprintf(out, n, "%s", ost_modes[settings.ost_mode]); break;
    case OPT_SHARPEN:    snprintf(out, n, "%s", sharpen_names[settings.sharpen]); break;
    case OPT_SCALER:     snprintf(out, n, "%s", settings.scaler ? "Smooth (xBR)" : "Edge"); break;
    case OPT_LIGHTING:   snprintf(out, n, "%s", settings.lighting == 2 ? "Bright" : settings.lighting ? "Soft" : "Off"); break;
    case OPT_AMBIENCE:   snprintf(out, n, "%s", settings.ambience ? "On" : "Off"); break;
    case OPT_AUDIO:      snprintf(out, n, "%s", settings.audio == 2 ? "Rich" : settings.audio ? "Clean" : "Original"); break;
    case OPT_OVERCLOCK:
        if (settings.overclock <= 100) snprintf(out, n, "Off");
        else snprintf(out, n, "%d%% CPU", settings.overclock);
        break;
    case OPT_WINDOW_ALL:
        if (settings.fullscreen) snprintf(out, n, "Fullscreen");
        else snprintf(out, n, "Window %d\xC3\x97", settings.window_scale);
        break;
    case OPT_REWIND_ALL:
        if (!settings.rewind) snprintf(out, n, "Off");
        else snprintf(out, n, "%d MB", settings.rewind_mb);
        break;
    case OPT_INTERNAL:
        if (settings.internal_scale <= 1) snprintf(out, n, "Native");
        else snprintf(out, n, "%d\xC3\x97", settings.internal_scale);
        break;
    case OPT_REWIND:     snprintf(out, n, "%s", settings.rewind ? "On" : "Off"); break;
    case OPT_REWIND_MB:  snprintf(out, n, "%d MB", settings.rewind_mb); break;
    case OPT_LAUNCHER:   snprintf(out, n, "%s", settings.show_launcher ? "On" : "Off"); break;
    case OPT_CHEAT_HEALTH:  snprintf(out, n, "%s", settings.cheat_health ? "On" : "Off"); break;
    case OPT_CHEAT_SPIRIT:  snprintf(out, n, "%s", settings.cheat_spirit ? "On" : "Off"); break;
    case OPT_CHEAT_WEAPONS: snprintf(out, n, "%s", settings.cheat_weapons ? "On" : "Off"); break;
    case OPT_CHEATS: {
        int p = cheat_preset();
        snprintf(out, n, "%s", p < 0 ? "Custom" : cheat_presets[p]);
        break;
    }
    }
}

void settings_set_volume_fraction(float v) {
    int x = (int)(v * 20 + .5f) * 5;
    settings.volume = x < 0 ? 0 : x > 100 ? 100 : x;
}

int settings_cycle(int v, int count, int dir) { return ((v + dir) % count + count) % count; }

void settings_option_change(int id, int dir) {
    switch (id) {
    case OPT_WIDESCREEN: settings.widescreen = !settings.widescreen; break;
    case OPT_ASPECT: {
        int cur = 1;
        for (int i = 0; i < 3; i++) if (aspects[i][0] == settings.aspect_w && aspects[i][1] == settings.aspect_h) cur = i;
        cur = settings_cycle(cur, 3, dir);
        settings.aspect_w = aspects[cur][0]; settings.aspect_h = aspects[cur][1];
        break;
    }
    case OPT_FULLSCREEN: settings.fullscreen = !settings.fullscreen; break;
    case OPT_INTEGER:    settings.integer_scale = !settings.integer_scale; break;
    case OPT_WINDOW:     settings.window_scale = settings_cycle(settings.window_scale - 2, 5, dir) + 2; break;
    case OPT_FLICKER:    settings.no_sprite_limit = !settings.no_sprite_limit; break;
    case OPT_SMOOTH:     settings.smooth_motion = !settings.smooth_motion; break;
    case OPT_FILTER:     settings.filter = settings_cycle(settings.filter, FILTER_COUNT, dir); break;
    case OPT_VOLUME:                    /* stops at the ends instead of wrapping around */
        settings.volume += dir * 10;
        settings.volume = settings.volume < 0 ? 0 : settings.volume > 100 ? 100 : settings.volume;
        break;
    case OPT_OVERCLOCK: {
        int cur = 0;
        for (int i = 0; i < 4; i++) if (overclocks[i] == settings.overclock) cur = i;
        settings.overclock = overclocks[settings_cycle(cur, 4, dir)];
        break;
    }
    case OPT_WINDOW_ALL: {
        int cur = settings.fullscreen ? 0 : settings.window_scale - 1;      /* 0 = fullscreen, 1-5 = 2x-6x */
        if (cur < 0 || cur > 5) cur = 3;
        cur = settings_cycle(cur, 6, dir);
        settings.fullscreen = cur == 0;
        if (cur) settings.window_scale = cur + 1;
        break;
    }
    case OPT_REWIND_ALL: {
        int cur = 0;                    /* 0 = off, 1.. = rewind_sizes */
        if (settings.rewind) for (int i = 0; i < 5; i++) if (rewind_sizes[i] == settings.rewind_mb) cur = i + 1;
        if (settings.rewind && !cur) cur = 2;
        cur = settings_cycle(cur, 6, dir);
        settings.rewind = cur > 0;
        if (cur) settings.rewind_mb = rewind_sizes[cur - 1];
        break;
    }
    case OPT_INTERNAL:   settings.internal_scale = settings_cycle(settings.internal_scale - 1, 4, dir) + 1; break;
    case OPT_REWIND:     settings.rewind = !settings.rewind; break;
    case OPT_REWIND_MB: {
        int cur = 1;
        for (int i = 0; i < 5; i++) if (rewind_sizes[i] == settings.rewind_mb) cur = i;
        settings.rewind_mb = rewind_sizes[settings_cycle(cur, 5, dir)];
        break;
    }
    case OPT_LAUNCHER: settings.show_launcher = !settings.show_launcher; break;
    case OPT_MUSIC:    settings.launcher_music = !settings.launcher_music; break;
    case OPT_OST_MODE: settings.ost_mode = settings_cycle(settings.ost_mode, OST_MODE_COUNT, dir); break;
    case OPT_SHARPEN:  settings.sharpen = settings_cycle(settings.sharpen, 4, dir); break;
    case OPT_SCALER:   settings.scaler = !settings.scaler; break;
    case OPT_LIGHTING: settings.lighting = settings_cycle(settings.lighting, 3, dir); break;
    case OPT_AMBIENCE: settings.ambience = !settings.ambience; break;
    case OPT_AUDIO:    settings.audio = settings_cycle(settings.audio, 3, dir); break;
    case OPT_CHEAT_HEALTH:  settings.cheat_health = !settings.cheat_health; break;
    case OPT_CHEAT_SPIRIT:  settings.cheat_spirit = !settings.cheat_spirit; break;
    case OPT_CHEAT_WEAPONS: settings.cheat_weapons = !settings.cheat_weapons; break;
    case OPT_CHEATS: {
        int p = cheat_preset();
        p = p < 0 ? (dir > 0 ? 0 : 3) : settings_cycle(p, 4, dir);
        settings.cheat_health = p >= 1;
        settings.cheat_spirit = p >= 2;
        settings.cheat_weapons = p >= 3;
        break;
    }
    }
}

