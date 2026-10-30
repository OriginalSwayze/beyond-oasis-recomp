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
    int  internal_scale;                /* 1 = native; 2, 3, 4 or 6 = layers redrawn at that scale (hd.c) */
    bool smooth_motion;                 /* draw between the game's 30 Hz ticks (hd.c, motion.c) */
    int  filter;                        /* FILTER_* (filter.h): off, scanlines, CRT, LCD */
    int  sharpen;                       /* sharpening: 0 off, 1 light, 2 medium, 3 strong (hd_sharpen) */
    int  scaler;                        /* layer scaler: 0 edge (Scale2x/3x), 1 smooth (xBR rules) */
    int  lighting;                      /* glow of bright pixels: 0 off, 1 soft, 2 bright (hd_post) */
    bool elements;                      /* particles from spirits and magic (fx.c) */
    bool footsteps;                     /* dust, grass and splashes where Ali walks (fx.c) */
    bool water;                         /* water shimmers, foams, glints and reflects (hd_water) */
    char texture_pack[64];              /* folder in texture_packs/, empty for none (texpack.h) */
    bool dump_tiles;                    /* write every tile shown to texture_packs/dump/ */
    int  audio;                         /* sound output: 0 original, 1 clean, 2 rich (sound_quality) */
    int  overclock;                     /* 68000 speed in percent (100 = original): removes slowdown */
    int  window_scale;
    bool rewind;                        /* keep a rewind buffer (hold the rewind key to step back) */
    int  rewind_mb;                     /* rewind buffer size; frames cost ~1 KB, so 64 MB is many minutes */
    int  state_slot;                    /* save state slot, 1-9 */
    bool show_launcher;                 /* show the launcher at startup (--no-launcher skips it once) */
    bool launcher_music;                /* the launcher plays the soundtrack (M toggles it) */
    int  ost_mode;                      /* soundtrack player: OST_MODE_* (launcher.c) */
    int  ost_track;                     /* the song it played last, or -1 for the title theme */
    int  volume;                        /* 0-100: game sound and launcher music */
    int  bind_key[10];                  /* input.h IN_*: SDL scancode for each action */
    int  bind_pad[10];                  /* input.h IN_*: SDL game controller button for each action */
    bool cheat_health;                  /* infinite health: no damage, health held full */
    bool cheat_spirit;                  /* infinite spirit power */
    bool cheat_weapons;                 /* weapons never break */
    int  cheat_attack;                  /* attack power: 0 normal, 1 double, 2 quadruple, 3 one-hit kills */
    int  cheat_speed;                   /* move speed: 0 normal, 1 fast (1.5x), 2 very fast (2x) */
    char cheat_codes[512];              /* extra RAM codes, "FFxxxx:VV[VV],..." (settings.ini only) */
    bool outfit;                        /* Ali wears outfit_color (outfit.c) */
    int  outfit_color[8];               /* CRAM colour for each part (OUTFIT_PARTS, outfit.c), -1 = his own */
    int  outfit_anim[8];                /* its animation: OUTFIT_ANIM_* (genesis.h) */
} Settings;

extern Settings settings;

void settings_load(const char *path);
void settings_save(const char *path);
/* Options the launcher and the in-game menu show: a value label, and stepping it. */
enum { OPT_WIDESCREEN, OPT_ASPECT, OPT_FULLSCREEN, OPT_INTEGER, OPT_WINDOW, OPT_FLICKER, OPT_REWIND, OPT_REWIND_MB,
       OPT_LAUNCHER, OPT_CHEAT_HEALTH, OPT_CHEAT_SPIRIT, OPT_CHEAT_WEAPONS,
       OPT_CHEAT_ATTACK, OPT_CHEAT_SPEED, OPT_INTERNAL, OPT_SMOOTH,
       OPT_REWIND_ALL,                  /* rewind off or its buffer size, in one option */
       OPT_FILTER, OPT_WINDOW_ALL,      /* fullscreen or a window size, in one option */
       OPT_OVERCLOCK, OPT_VOLUME,
       OPT_MUSIC, OPT_OST_MODE,          /* launcher music on/off, soundtrack player mode */
       OPT_SHARPEN, OPT_SCALER, OPT_LIGHTING, OPT_AUDIO, OPT_ELEMENTS, OPT_FOOTSTEPS, OPT_WATER,
       OPT_TEXPACK, OPT_DUMP };
enum { OST_MODE_ALL, OST_MODE_REPEAT, OST_MODE_SHUFFLE, OST_MODE_COUNT };
void settings_set_volume_fraction(float v);         /* sets volume from a slider (0-1), in steps of 5 */                    /* all three cheats as presets: Off, Health, Health + spirit, All */
void settings_option_text(int id, char *out, size_t n);
void settings_option_change(int id, int dir);
int  settings_cycle(int v, int count, int dir);     /* (v + dir) wrapped into 0..count-1 */

/* Widescreen pixels to add on each side for the current settings. */
int  settings_extra_width(void);

/* The folder next to the executable (texture packs are listed under it). */
void settings_set_base(const char *dir);

#endif
