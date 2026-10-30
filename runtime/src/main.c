/* Beyond Oasis Recomp: window, input, frame pacing.
 *   BeyondOasis.exe [rom path]                 play
 *   BeyondOasis.exe --headless N --shot F.bmp  run N frames without a window, save the last frame
 *   headless extras: --press "FRAME:BUTTONS[:FRAMES],..." --shots EVERY DIR --widescreen 0|1 */
#include "genesis.h"
#include "settings.h"
#include "png.h"
#include "launcher.h"
#include "cheats.h"
#include "hd.h"
#include "motion.h"
#include "filter.h"
#include "input.h"
#include "controls.h"
#include "outfit.h"
#include "preview.h"
#include "fx.h"
#include "texpack.h"
#include "menu.h"
#include "savestate.h"
#include "ui.h"
#include <SDL.h>
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_W (320 + MAX_EXTRA_WIDTH * 2)
#define FB_H 240

static uint32_t framebuffer[FB_W * FB_H];
extern uint8_t coverage[ROM_SIZE / 16];
extern uint32_t *pc_profile;

/* Prints the most executed instructions of the last frames, to find busy-wait loops. */
static void print_pc_profile(void) {
    for (int k = 0; k < 12; k++) {
        uint32_t best = 0, at = 0;
        for (uint32_t i = 0; i < ROM_SIZE / 2; i++) if (pc_profile[i] > best) { best = pc_profile[i]; at = i; }
        if (!best) break;
        printf("  $%06X  %u\n", at * 2, best);
        pc_profile[at] = 0;
    }
}

/* ---- Scripted input for headless runs ------------------------------------------
 * --press "FRAME:BUTTONS[:FRAMES],..." holds BUTTONS (letters U D L R A B C S) from
 * FRAME for FRAMES frames (default 4). Several entries may overlap. */
typedef struct { int frame, len; uint16_t bits; } Press;
static Press presses[256];
static int press_count;

static void parse_presses(const char *spec) {
    while (*spec && press_count < (int)(sizeof presses / sizeof *presses)) {
        Press p = { atoi(spec), 4, 0 };
        const char *c = strchr(spec, ':');
        if (!c) break;
        for (c++; *c && *c != ':' && *c != ','; c++) {
            switch (*c) {
            case 'U': p.bits |= PAD_UP; break;    case 'D': p.bits |= PAD_DOWN; break;
            case 'L': p.bits |= PAD_LEFT; break;  case 'R': p.bits |= PAD_RIGHT; break;
            case 'A': p.bits |= PAD_A; break;     case 'B': p.bits |= PAD_B; break;
            case 'C': p.bits |= PAD_C; break;     case 'S': p.bits |= PAD_START; break;
            }
        }
        if (*c == ':') p.len = atoi(++c);
        presses[press_count++] = p;
        const char *next = strchr(c, ',');
        if (!next) break;
        spec = next + 1;
    }
}

static uint16_t scripted_pad(int frame) {
    uint16_t bits = 0;
    for (int i = 0; i < press_count; i++)
        if (frame >= presses[i].frame && frame < presses[i].frame + presses[i].len) bits |= presses[i].bits;
    return bits;
}

static char base_dir[1024];

static void path_in_base(char *out, size_t n, const char *name) { snprintf(out, n, "%s%s", base_dir, name); }

static void save_coverage(void) {
    char p[1100];
    path_in_base(p, sizeof p, "coverage.bin");
    /* Merge with earlier runs so coverage accumulates across play sessions. */
    FILE *f = fopen(p, "rb");
    if (f) {
        uint8_t old[sizeof coverage];
        if (fread(old, 1, sizeof old, f) == sizeof old)
            for (size_t i = 0; i < sizeof old; i++) coverage[i] |= old[i];
        fclose(f);
    }
    f = fopen(p, "wb");
    if (f) { fwrite(coverage, 1, sizeof coverage, f); fclose(f); }
}

static void save_sram(void) {
    if (!bus.sram_dirty) return;
    char p[1100];
    path_in_base(p, sizeof p, "save.srm");
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(bus.sram, 1, SRAM_SIZE, f); fclose(f); bus.sram_dirty = false; }
}

static void load_sram(void) {
    char p[1100];
    path_in_base(p, sizeof p, "save.srm");
    memset(bus.sram, 0xFF, SRAM_SIZE);
    FILE *f = fopen(p, "rb");
    if (f) { if (fread(bus.sram, 1, SRAM_SIZE, f) != SRAM_SIZE) { /* keep what was read */ } fclose(f); }
}

static int frame_width(void) { return vdp_screen_width() + vdp_opts.extra_width * 2; }

static int save_bmp(const char *path) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(framebuffer, frame_width(), vdp_screen_height(), 32,
                                                        FB_W * 4, SDL_PIXELFORMAT_XRGB8888);
    if (!s) return -1;
    int r = SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    return r;
}

static int save_png(const char *path) { return png_write(path, framebuffer, frame_width(), vdp_screen_height(), FB_W); }

/* Saves as PNG or BMP depending on the file extension. */
static int save_shot(const char *path) {
    size_t n = strlen(path);
    return n > 4 && !strcmp(path + n - 4, ".png") ? save_png(path) : save_bmp(path);
}

static void apply_settings(void) {
    vdp_opts.extra_width = settings_extra_width();
    widescreen_patches(vdp_opts.extra_width);
    vdp_opts.no_sprite_limit = settings.no_sprite_limit;
    vdp_opts.hd_capture = true;         /* the layers are also what the menu's previews draw from */
    machine_opts.overclock = settings.overclock;
    outfit_apply();
    sound_quality = settings.audio;
    hd_scaler = settings.scaler;
    hd_shadows = settings.shadows;
    hd_water = settings.water;
    /* Texture packs: load a pack when it changes (it can be many images), dump on request. */
    static char loaded_pack[64] = "";
    static bool pack_tried;
    if (!pack_tried || strcmp(loaded_pack, settings.texture_pack)) {
        int n = texpack_load(base_dir, settings.texture_pack);
        if (settings.texture_pack[0]) fprintf(stderr, "texture pack %s: %d images\n", settings.texture_pack, n);
        snprintf(loaded_pack, sizeof loaded_pack, "%s", settings.texture_pack);
        pack_tried = true;
    }
    texpack_dump(base_dir, settings.dump_tiles);
    vdp_opts.tile_refs = texpack_active();
    hd_texpack = texpack_count() > 0;
}

/* The finishing steps for this frame (hd_post), with the camera from the game's RAM: the
 * plane A layer struct at $FF1716 holds it (+00 x, +04 y) while a map layer is active. */
static HdPost finishing(int scale, float t) {
    const uint8_t *layer = bus.ram + 0x1716;
    HdPost p = { settings.sharpen, settings.lighting, settings.ambience, settings.lights, settings.cinematic,
                 settings.weather, settings.elements, settings.footsteps, false, 0, 0, t, scale < 1 ? 1 : scale, vdp_opts.extra_width };
    p.world = (layer[0x27] & 1) && !vdp_opts.mask_sides;
    p.cam_x = (float)(int16_t)(layer[0] << 8 | layer[1]);
    p.cam_y = (float)(int16_t)(layer[4] << 8 | layer[5]);
    return p;
}

/* The particles move on after each game frame, placed in the world by the camera. */
static void fx_after_frame(void) {
    HdPost here = finishing(1, 0);
    fx_game_frame(&hd_frame, here.world, here.cam_x, here.cam_y);
}

/* Ali's middle on screen (frame pixels), for the previews' close-ups; -1 if unknown. */
static float ali_x(void) { return vdp_player_w ? vdp_player_x + vdp_opts.extra_width + vdp_player_w / 2.f : -1; }
static float ali_y(void) { return vdp_player_w ? vdp_player_y + vdp_player_h / 2.f : -1; }

/* ---- The pause menu's sounds -------------------------------------------------------- *
 * The game's own menu sounds, rendered once from the ROM (launcher.c) into
 * menu_sounds.cache: the pause menu cannot call the sound driver while the game is paused
 * without changing the game. Played straight into the audio queue, which is empty while
 * the game is paused. */
static int16_t *menu_pcm[4];
static int menu_pcm_frames[4];
static SDL_AudioDeviceID menu_audio;

static void load_menu_sounds(void) {
    char path[1100];
    path_in_base(path, sizeof path, "menu_sounds.cache");
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (!launcher_make_menu_sounds(path)) return;
        f = fopen(path, "rb");
        if (!f) return;
    }
    for (int k = 0; k < 4; k++) {
        int n = 0;
        if (fread(&n, 4, 1, f) != 1 || n <= 0 || n > 48000) break;
        menu_pcm[k] = malloc((size_t)n * 4);
        if (!menu_pcm[k] || fread(menu_pcm[k], 4, (size_t)n, f) != (size_t)n) { free(menu_pcm[k]); menu_pcm[k] = NULL; break; }
        menu_pcm_frames[k] = n;
    }
    fclose(f);
}

static void play_menu_sound(int kind) {
    if (!menu_audio || kind < 0 || kind > 3 || !menu_pcm[kind]) return;
    int n = menu_pcm_frames[kind], out_n = (int)((int64_t)n * sound_out_rate / 48000);
    int16_t *buf = malloc((size_t)out_n * 4);
    if (!buf) return;
    int v = settings.volume * 256 / 100;
    for (int i = 0; i < out_n; i++) {           /* to the device's rate, and the volume */
        int j = (int)((int64_t)i * 48000 / sound_out_rate);
        if (j >= n) j = n - 1;
        buf[i * 2] = (int16_t)(menu_pcm[kind][j * 2] * v / 256);
        buf[i * 2 + 1] = (int16_t)(menu_pcm[kind][j * 2 + 1] * v / 256);
    }
    SDL_ClearQueuedAudio(menu_audio);
    SDL_QueueAudio(menu_audio, buf, (Uint32)out_n * 4);
    free(buf);
}

/* Largest rectangle with the game's display aspect that fits the window. */
static SDL_Rect display_rect(int win_w, int win_h) {
    int fw = frame_width(), fh = vdp_screen_height();
    double par = (4.0 / 3.0) / (320.0 / 224.0);
    double aspect = fw * par / fh;
    SDL_Rect r;
    if (settings.integer_scale) {
        int s = win_h / fh;
        if (s < 1) s = 1;
        while (s > 1 && (int)(fh * s * aspect) > win_w) s--;
        r.h = fh * s;
        r.w = (int)(r.h * aspect);
    } else if ((double)win_w / win_h > aspect) {
        r.h = win_h;
        r.w = (int)(win_h * aspect);
    } else {
        r.w = win_w;
        r.h = (int)(win_w / aspect);
    }
    r.x = (win_w - r.w) / 2;
    r.y = (win_h - r.h) / 2;
    return r;
}

void console_attach(void);           /* console.c */

int main(int argc, char **argv) {
    console_attach();
    char *bp = SDL_GetBasePath();
    if (bp) { snprintf(base_dir, sizeof base_dir, "%s", bp); SDL_free(bp); }
    settings_set_base(base_dir);
    char ini[1100];
    path_in_base(ini, sizeof ini, "settings.ini");
    savestate_init(base_dir);
    settings_load(ini);

    int headless_frames = 0;
    const char *shot = NULL;
    int profile_frames = 0;
    const char *wav = NULL;
    const char *hash_log = NULL;
    const char *shot_dir = NULL;
    int shot_every = 0;
    const char *dump = NULL;
    uint32_t watch[16];
    int watch_count = 0;
    int check_from = -1, check_to = -1;     /* --state-check A B */
    int rewind_check = 0;                   /* --rewind-check K */
    bool no_launcher = false;
    const char *launcher_shot = NULL;
    int shot_width = 1280;
    float shot_lx = 0, shot_ly = 0;
    const char *menu_shot = NULL;
    int menu_w = 1920, menu_h = 1080;
    bool controls_shot = false, outfit_page_shot = false;
    int menu_tab = -1, menu_row = 0;    /* --menu-tab TAB ROW, with --menu-shot */
    bool save_preview = false;          /* --save-preview: keep the last frame for the launcher's previews */
    int save_to = 0;
    int load_from = 0;
    bool rom_from_args = false;         /* a ROM path was given on the command line */
    bool test_overrides = false;        /* settings overridden on the command line: don't save them */
    const char *hd_shot = NULL;
    const char *outfit_shot = NULL;     /* --outfit-check F.png */
    const char *smooth_dir = NULL;      /* --smooth-check DIR SCALE */
    bool motion_check = false;
    const char *sprite_dir = NULL;      /* --sprite-check EVERY DIR SCALE */
    int sprite_every = 0;
    const char *filter_shot = NULL;     /* --filter-shot F.png W H MODE */
    int filter_w = 1920, filter_h = 1080, filter_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless") && i + 1 < argc) headless_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--pc-profile") && i + 1 < argc) profile_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--interpret")) machine_opts.recomp = false;
        else if (!strcmp(argv[i], "--hash-log") && i + 1 < argc) hash_log = argv[++i];
        else if (!strcmp(argv[i], "--verify")) machine_opts.verify = true;
        else if (!strcmp(argv[i], "--hide-layers") && i + 1 < argc) vdp_opts.hide_layers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace-vram") && i + 2 < argc) {
            extern uint32_t vdp_trace_lo, vdp_trace_hi;
            vdp_trace_lo = (uint32_t)strtoul(argv[++i], NULL, 16);
            vdp_trace_hi = (uint32_t)strtoul(argv[++i], NULL, 16);
        }
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--state-check") && i + 2 < argc) { check_from = atoi(argv[++i]); check_to = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--rewind-check") && i + 1 < argc) rewind_check = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) {
            /* --watch ADDR,ADDR,...: print these RAM bytes whenever one changes. */
            for (char *p = argv[++i]; *p && watch_count < 16; ) {
                watch[watch_count++] = (uint32_t)strtoul(p, &p, 16) & 0xFFFF;
                if (*p == ',') p++;
            }
        }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) parse_presses(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 2 < argc) { shot_every = atoi(argv[++i]); shot_dir = argv[++i]; }
        else if (!strcmp(argv[i], "--widescreen") && i + 1 < argc) { settings.widescreen = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--overclock") && i + 1 < argc) { settings.overclock = atoi(argv[++i]); test_overrides = true; }   /* percent */
        else if (!strcmp(argv[i], "--sharpen") && i + 1 < argc) { settings.sharpen = atoi(argv[++i]) & 3; test_overrides = true; }   /* 0-3 */
        else if (!strcmp(argv[i], "--lighting") && i + 1 < argc) { settings.lighting = atoi(argv[++i]) % 3; test_overrides = true; }   /* 0-2 */
        else if (!strcmp(argv[i], "--ambience") && i + 1 < argc) { settings.ambience = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--lights") && i + 1 < argc) { settings.lights = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--shadows") && i + 1 < argc) { settings.shadows = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--cinematic") && i + 1 < argc) { settings.cinematic = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--water") && i + 1 < argc) { settings.water = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--weather") && i + 1 < argc) { settings.weather = atoi(argv[++i]) % WEATHER_COUNT; test_overrides = true; }
        else if (!strcmp(argv[i], "--elements") && i + 1 < argc) { settings.elements = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--footsteps") && i + 1 < argc) { settings.footsteps = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--texture-pack") && i + 1 < argc) { snprintf(settings.texture_pack, sizeof settings.texture_pack, "%s", argv[++i]); test_overrides = true; }
        else if (!strcmp(argv[i], "--dump-tiles") && i + 1 < argc) { settings.dump_tiles = atoi(argv[++i]) != 0; test_overrides = true; }
        else if (!strcmp(argv[i], "--scaler") && i + 1 < argc) { settings.scaler = atoi(argv[++i]) != 0; test_overrides = true; }   /* 0 edge, 1 smooth */
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) { settings.audio = atoi(argv[++i]) % 3; test_overrides = true; }   /* 0-2 */
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) { settings.volume = atoi(argv[++i]); test_overrides = true; }   /* 0-100 */
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) { settings.filter = atoi(argv[++i]) % FILTER_COUNT; test_overrides = true; }   /* 0 off, 1 scanlines, 2 CRT, 3 LCD */
        else if (!strcmp(argv[i], "--no-launcher")) no_launcher = true;
        else if (!strcmp(argv[i], "--launcher-music-check") && i + 1 < argc) {
            /* --launcher-music-check SECONDS: run the launcher music headless, print its level. */
            apply_settings();
            return launcher_music_check(atoi(argv[++i]));
        }
        else if (!strcmp(argv[i], "--png-check") && i + 1 < argc) {
            /* --png-check F.png: read a PNG and print its size and a checksum of its pixels. */
            int w = 0, h = 0;
            uint32_t *px = png_read(argv[++i], &w, &h);
            uint32_t sum = 0;
            for (int k = 0; px && k < w * h; k++) sum = sum * 31 + px[k];
            printf("png %s: %dx%d checksum %08X\n", px ? "read" : "NOT read", w, h, sum);
            free(px);
            return px ? 0 : 1;
        }
        else if (!strcmp(argv[i], "--ost-check") && i + 1 < argc) {
            /* --ost-check SECONDS: play every song headless, print its level per second. */
            apply_settings();
            return launcher_ost_check(atoi(argv[++i]));
        }
        else if (!strcmp(argv[i], "--filter-shot") && i + 4 < argc) {
            /* --filter-shot F.png W H MODE: draw the last frame into a W x H picture with
             * filter MODE (0 off, 1 scanlines, 2 CRT, 3 LCD), on the GPU in a hidden window. */
            filter_shot = argv[++i];
            filter_w = atoi(argv[++i]);
            filter_h = atoi(argv[++i]);
            filter_mode = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--sprite-check") && i + 3 < argc) {
            /* --sprite-check EVERY DIR SCALE: every EVERY frames, compare the frame drawn layer by
             * layer with the same frame drawn sprite by sprite (as smooth motion does). They
             * must match except at overlapping sprite edges; the worst frame is saved to DIR. */
            sprite_every = atoi(argv[++i]);
            sprite_dir = argv[++i];
            settings.internal_scale = atoi(argv[++i]);
            settings.smooth_motion = true;
        }
        else if (!strcmp(argv[i], "--motion-check")) {
            /* --motion-check: from frame 300 on, check that the sprites shown are found in the
             * sprite lists the game built and how many belong to objects (motion.c). */
            motion_check = true;
            settings.smooth_motion = true;
        }
        else if (!strcmp(argv[i], "--smooth-check") && i + 2 < argc) {
            /* --smooth-check DIR SCALE: smooth motion over the last 8 frames at 120 Hz: prints
             * where each picture is between the motion updates and saves it as DIR/NN.png. */
            smooth_dir = argv[++i];
            settings.internal_scale = atoi(argv[++i]);
            settings.smooth_motion = true;
        }
        else if (!strcmp(argv[i], "--outfit") && i + 1 < argc) {
            /* --outfit a,b,c,d: Ali's outfit colours for this run (as outfit_colors in settings.ini). */
            outfit_ini_load("outfit_colors", argv[++i]);
            settings.outfit = true;
            test_overrides = true;
        }
        else if (!strcmp(argv[i], "--outfit-check") && i + 1 < argc) {
            /* --outfit-check F.png: at the end, save the player's sprites 6x with their colours
             * and list the colour indices he uses. */
            outfit_shot = argv[++i];
        }
        else if (!strcmp(argv[i], "--hd-shot") && i + 2 < argc) {
            /* --hd-shot F.png SCALE: save the last frame at the internal resolution SCALE (2-4). */
            hd_shot = argv[++i];
            settings.internal_scale = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--save-to") && i + 1 < argc) save_to = atoi(argv[++i]);   /* save a slot at the end */
        else if (!strcmp(argv[i], "--states-base") && i + 1 < argc) savestate_init(argv[++i]);  /* e.g. "test_" -> test_states/ */
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc) { settings.state_slot = atoi(argv[++i]); test_overrides = true; }
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load_from = atoi(argv[++i]);   /* load a slot first */
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) cheats_add_codes(argv[++i]);   /* FFxxxx:VV[VV],... */
        else if (!strcmp(argv[i], "--cheat") && i + 1 < argc) {
            /* --cheat health|spirit|weapons: turn a built-in cheat on for this run. */
            const char *c = argv[++i];
            test_overrides = true;
            if (!strcmp(c, "health")) settings.cheat_health = true;
            else if (!strcmp(c, "spirit")) settings.cheat_spirit = true;
            else if (!strcmp(c, "weapons")) settings.cheat_weapons = true;
        }
        else if (!strcmp(argv[i], "--menu-shot") && i + 3 < argc) {
            /* --menu-shot F.png W H: open the in-game menu over the last frame and render it. */
            menu_shot = argv[++i];
            menu_w = atoi(argv[++i]);
            menu_h = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--controls-shot")) controls_shot = true;   /* with --menu-shot: the Controls page */
        else if (!strcmp(argv[i], "--outfit-shot")) outfit_page_shot = true;  /* with --menu-shot: the outfit page */
        else if (!strcmp(argv[i], "--save-preview")) save_preview = true;
        else if (!strcmp(argv[i], "--menu-tab") && i + 2 < argc) { menu_tab = atoi(argv[++i]); menu_row = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--input-check") && i + 1 < argc) {
            /* --input-check FILE: rebind a few keys and buttons, save to FILE, load it back,
             * and print the bindings (never touches settings.ini). */
            const char *file = argv[++i];
            input_bind_key(IN_A, SDL_SCANCODE_J);
            input_bind_key(IN_B, SDL_SCANCODE_J);           /* taken by A: A gets B's old key */
            input_bind_button(IN_START, SDL_CONTROLLER_BUTTON_Y);
            input_bind_button(IN_REWIND, SDL_CONTROLLER_BUTTON_START);   /* taken by Start: swaps */
            settings_save(file);
            input_defaults();
            settings_load(file);
            for (int k = 0; k < IN_COUNT; k++)
                printf("  %-14s keyboard %-12s controller %s\n", input_labels[k], input_key_name(k), input_button_name(k));
            return 0;
        }
        else if (!strcmp(argv[i], "--launcher-shot") && i + 4 < argc) {
            /* --launcher-shot F.png WIDTH TAB TIME: render the launcher with tab TAB (0-3) open at TIME seconds. */
            launcher_shot = argv[++i];
            shot_width = atoi(argv[++i]);
            shot_lx = (float)atof(argv[++i]);
            shot_ly = (float)atof(argv[++i]);
        }
        else { snprintf(settings.rom_path, sizeof settings.rom_path, "%s", argv[i]); rom_from_args = true; }
    }

    if (launcher_shot) {
        apply_settings();
        return launcher_screenshot(launcher_shot, shot_width, (int)shot_lx, shot_ly) ? 1 : 0;
    }
    if (SDL_Init(headless_frames ? 0 : SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!headless_frames && settings.show_launcher && !no_launcher) {
        apply_settings();
        if (!launcher_run(ini)) { SDL_Quit(); return 0; }
    }
    const char *err = settings.rom_path[0] ? bus_load_rom(settings.rom_path) : "no ROM selected";
    if (err) {
        char msg[1400];
        snprintf(msg, sizeof msg, "Could not load the Beyond Oasis (USA) ROM:\n%s\n\nPath: %s", err, settings.rom_path);
        if (headless_frames) fprintf(stderr, "%s\n", msg);
        else SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Beyond Oasis Recomp", msg, NULL);
        return 1;
    }
    if (!headless_frames && rom_from_args) {
        /* Remember a ROM path given on the command line, but not the other options
         * given there (--filter, --widescreen, ...): those are for this run. */
        Settings run = settings;
        settings_load(ini);
        snprintf(settings.rom_path, sizeof settings.rom_path, "%s", run.rom_path);
        settings_save(ini);
        settings = run;
    }
    /* Headless runs start from blank save RAM, so scripted routes play the same whatever
     * the player has saved (and they never write it back, see below). */
    if (!headless_frames) load_menu_sounds();   /* before the reset: it runs the ROM to the title */
    if (headless_frames) memset(bus.sram, 0xFF, SRAM_SIZE);
    else load_sram();
    apply_settings();
    machine_reset();
    rewind_reset(settings.rewind ? (size_t)settings.rewind_mb << 20 : 0);

    if (headless_frames) {
        /* --state-check A B: snapshot before frame A, run to B, load the snapshot, run
         * A..B again with the same input; both runs must end in the same state.
         * --rewind-check K: record every frame, then rewind K frames; the result must
         * equal the snapshot taken K frames before the end. */
        static uint8_t snap_a[STATE_MAX], snap_b[STATE_MAX], snap_ref[STATE_MAX];
        size_t len_a = 0, len_b = 0, len_ref = 0;
        rewind_reset(rewind_check ? (size_t)512 << 20 : 0);
        if (load_from && !savestate_load(load_from)) fprintf(stderr, "could not load slot %d\n", load_from);
        /* --wav: record the audio of the run (16-bit stereo, 48 kHz). */
        FILE *wf = wav ? fopen(wav, "wb") : NULL;
        uint32_t wav_frames = 0;
        if (wf) fseek(wf, 44, SEEK_SET);
        /* --hash-log: one line per frame with a hash of RAM, VRAM and the 68000 PC,
         * to find the first frame where two runs (e.g. native vs --interpret) differ. */
        FILE *hf = hash_log ? fopen(hash_log, "w") : NULL;
        for (int f = 0; f < headless_frames; f++) {
            if (profile_frames && f == headless_frames - profile_frames) pc_profile = calloc(ROM_SIZE / 2, 4);
            pad_state[0] = scripted_pad(f);
            if (f == check_from) len_a = state_save(snap_a, sizeof snap_a);
            if (f == check_to) len_b = state_save(snap_b, sizeof snap_b);
            if (rewind_check && f == headless_frames - rewind_check) len_ref = state_save(snap_ref, sizeof snap_ref);
            rewind_push(pad_state[0]);
            cheats_frame();
            machine_run_frame(framebuffer, FB_W);
            if (vdp_opts.hd_capture) { hd_frame_done(); fx_after_frame(); }
            if (sprite_every > 0 && f >= 300 && f % sprite_every == 0) {
                static uint32_t *layers, *worst_a, *worst_b;
                static int worst = -1, worst_frame, checked, over;
                hd_compose(settings.internal_scale);
                size_t n = (size_t)hd_w * hd_h;
                layers = realloc(layers, n * 4);
                memcpy(layers, hd_px, n * 4);
                hd_compose_sprites(settings.internal_scale);
                int diff = 0;
                for (size_t k = 0; k < n; k++) diff += layers[k] != hd_px[k];
                checked++;
                if (diff > (int)(n / 200)) over++;      /* more than 0.5% of the picture */
                if (diff > worst) {
                    worst = diff; worst_frame = f;
                    worst_a = realloc(worst_a, n * 4); worst_b = realloc(worst_b, n * 4);
                    memcpy(worst_a, layers, n * 4); memcpy(worst_b, hd_px, n * 4);
                }
                if (f + sprite_every >= headless_frames) {
                    char p[1200];
                    printf("sprite check: %d frames, %d differ by more than 0.5%%; worst frame %d: %.2f%% of pixels\n",
                           checked, over, worst_frame, 100.0 * worst / n);
                    snprintf(p, sizeof p, "%s/layers.png", sprite_dir); png_write(p, worst_a, hd_w, hd_h, hd_w);
                    snprintf(p, sizeof p, "%s/sprites.png", sprite_dir); png_write(p, worst_b, hd_w, hd_h, hd_w);
                }
            }
            if (motion_check && f >= 300 && vdp_opts.hd_capture) {
                /* How well the sprites on screen are tied to the game's objects. */
                static int frames, unmatched, moving, drawn_total, owned_total, biggest;
                int dx[80], dy[80], owned;
                unsigned tick = motion_sprite_moves(&hd_frame, dx, dy, &owned);
                frames++;
                if (hd_frame.sprite_count && !tick) unmatched++;
                drawn_total += hd_frame.sprite_count;
                owned_total += owned;
                bool any = false;
                for (int k = 0; k < 80; k++) {
                    if (dx[k] || dy[k]) any = true;
                    if (abs(dx[k]) > biggest) biggest = abs(dx[k]);
                    if (abs(dy[k]) > biggest) biggest = abs(dy[k]);
                }
                moving += any;
                if (f + 1 == headless_frames)
                    printf("motion check: %d frames, %d with sprites not found in a tick, %d with moving objects; "
                           "%.1f%% of sprites follow an object; largest move %d px\n",
                           frames, unmatched, moving, drawn_total ? 100.0 * owned_total / drawn_total : 0, biggest);
            }
            if (smooth_dir && f >= headless_frames - 8) {
                for (int k = 0; k < 2; k++) {
                    char p[1200];
                    float frac = k * .5f;
                    Uint64 t0 = SDL_GetPerformanceCounter();
                    hd_compose_at(settings.internal_scale, frac);
                    double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency();
                    int n = (f - (headless_frames - 8)) * 2 + k;
                    printf("frame %d +%.1f: t=%.2f  %d of %d sprites follow objects  %.2f ms (layers %.2f, sprites %.2f + %.2f, compose %.2f)\n",
                           f, frac, hd_motion_t(frac), hd_owned, hd_frame.sprite_count, ms, hd_ms[0], hd_ms[1], hd_ms[2], hd_ms[3]);
                    snprintf(p, sizeof p, "%s/%02d.png", smooth_dir, n);
                    png_write(p, hd_px, hd_w, hd_h, hd_w);
                }
            }
            if (watch_count) {
                static uint8_t last[16];
                bool changed = f == 0;
                for (int k = 0; k < watch_count; k++) changed |= last[k] != bus.ram[watch[k]];
                if (changed) {
                    printf("%5d", f);
                    for (int k = 0; k < watch_count; k++) printf(" %02X", last[k] = bus.ram[watch[k]]);
                    printf("\n");
                }
            }
            if (shot_every && f % shot_every == 0) {
                char p[1200];
                snprintf(p, sizeof p, "%s/%05d.png", shot_dir, f);
                save_png(p);
            }
            if (wf) { fwrite(sound_out, 4, (size_t)sound_out_count, wf); wav_frames += (uint32_t)sound_out_count; }
            if (hf) {
                uint64_t h = 1469598103934665603ull;
                for (size_t k = 0; k < sizeof bus.ram; k++) h = (h ^ bus.ram[k]) * 1099511628211ull;
                uint64_t v = 1469598103934665603ull;
                for (size_t k = 0; k < sizeof vdp.vram; k++) v = (v ^ vdp.vram[k]) * 1099511628211ull;
                fprintf(hf, "%d ram=%016llx vram=%016llx pc=%06x\n", f, (unsigned long long)h, (unsigned long long)v,
                        m68k_get_reg(NULL, M68K_REG_PC));
            }
        }
        if (hf) fclose(hf);
        if (len_a && len_b) {
            static uint8_t again[STATE_MAX];
            bool loaded = state_load(snap_a, len_a);
            for (int f = check_from; f < check_to; f++) { pad_state[0] = scripted_pad(f); machine_run_frame(framebuffer, FB_W); }
            size_t len = state_save(again, sizeof again);
            printf("state check %d-%d: snapshot %zu bytes, %s\n", check_from, check_to, len_a,
                   loaded && len == len_b && !memcmp(again, snap_b, len) ? "replay identical" : "REPLAY DIFFERS");
        }
        if (rewind_check)
            printf("rewind buffer: %d frames in %.1f MB\n", rewind_frames(), rewind_bytes() / 1048576.0);
        if (rewind_check && len_ref) {
            static uint8_t now[STATE_MAX];
            rewind_push(0);
            uint16_t p;
            int steps = 0;
            while (steps < rewind_check && rewind_step(&p)) steps++;
            size_t len = state_save(now, sizeof now);
            printf("rewind check: %d frames back, %s\n", steps,
                   steps == rewind_check && len == len_ref && !memcmp(now, snap_ref, len) ? "state identical" : "STATE DIFFERS");
        }
        if (wf) {
            uint32_t data = wav_frames * 4, rate = (uint32_t)sound_out_rate, br = rate * 4;
            uint8_t h[44] = { 0x52,0x49,0x46,0x46, 0,0,0,0, 0x57,0x41,0x56,0x45, 0x66,0x6D,0x74,0x20,
                             16,0,0,0, 1,0, 2,0, 0,0,0,0, 0,0,0,0, 4,0, 16,0, 0x64,0x61,0x74,0x61 };
            uint32_t riff = 36 + data;
            memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4); memcpy(h + 40, &data, 4);
            fseek(wf, 0, SEEK_SET); fwrite(h, 1, 44, wf); fclose(wf);
        }
        if (pc_profile) print_pc_profile();
        if (dump) {
            /* --dump: raw machine state for offline analysis:
             * RAM 64K, VRAM 64K, CRAM 128, VSRAM 80, VDP registers 32. */
            FILE *df = fopen(dump, "wb");
            if (df) {
                fwrite(bus.ram, 1, sizeof bus.ram, df);
                fwrite(vdp.vram, 1, sizeof vdp.vram, df);
                fwrite(vdp.cram, 1, sizeof vdp.cram, df);
                fwrite(vdp.vsram, 1, sizeof vdp.vsram, df);
                fwrite(vdp.reg, 1, sizeof vdp.reg, df);
                fclose(df);
            }
        }
        if (shot && save_shot(shot) != 0) fprintf(stderr, "could not save %s\n", shot);
        if (filter_shot) {
            SDL_InitSubSystem(SDL_INIT_VIDEO);
            SDL_Window *hw = SDL_CreateWindow("filter", 0, 0, 64, 64, SDL_WINDOW_HIDDEN);
            SDL_Renderer *sr = hw ? SDL_CreateRenderer(hw, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE) : NULL;
            SDL_Texture *target = sr ? SDL_CreateTexture(sr, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_TARGET, filter_w, filter_h) : NULL;
            uint32_t *pixels = malloc((size_t)filter_w * filter_h * 4);
            if (target && pixels) {
                SDL_SetRenderTarget(sr, target);
                bool hd = settings.internal_scale > 1;
                if (hd) hd_compose(settings.internal_scale);
                int tw = hd ? hd_w : frame_width(), th = hd ? hd_h : vdp_screen_height();
                SDL_Texture *t = SDL_CreateTexture(sr, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, tw, th);
                HdPost post = finishing(hd ? hd_w / frame_width() : 1, 1.0f);
                hd_post(hd ? hd_px : framebuffer, tw, th, hd ? hd_w : FB_W, &post);   /* once to set up, then timed */
                Uint64 p0 = SDL_GetPerformanceCounter();
                const uint32_t *finished = hd_post(hd ? hd_px : framebuffer, tw, th, hd ? hd_w : FB_W, &post);
                printf("finishing steps: %.2f ms at %dx%d\n", (SDL_GetPerformanceCounter() - p0) * 1000.0 / SDL_GetPerformanceFrequency(), tw, th);
                SDL_UpdateTexture(t, NULL, finished, tw * 4);
                SDL_SetRenderDrawColor(sr, 0, 0, 0, 255);
                SDL_RenderClear(sr);
                Uint64 t0 = SDL_GetPerformanceCounter();
                if (hd) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
                filter_draw(sr, t, NULL, display_rect(filter_w, filter_h), frame_width(), vdp_screen_height(), filter_mode);
                SDL_RenderReadPixels(sr, NULL, SDL_PIXELFORMAT_XRGB8888, pixels, filter_w * 4);
                printf("filter %d drawn in %.1f ms (GPU, first draw builds the overlay)\n", filter_mode,
                       (double)(SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency());
                png_write(filter_shot, pixels, filter_w, filter_h, filter_w);
                SDL_DestroyTexture(t);
            } else {
                fprintf(stderr, "no GPU renderer for --filter-shot: %s\n", SDL_GetError());
            }
            free(pixels);
            if (sr) SDL_DestroyRenderer(sr);
            if (hw) SDL_DestroyWindow(hw);
        }
        if (outfit_shot) {
            int w = vdp_player_w, h = vdp_player_h, count[64] = { 0 };
            printf("player: %dx%d pixels\n", w, h);
            uint32_t *img = calloc((size_t)w * h * 36 + 1, 4);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int v = vdp_player[y * PLAYER_IMG + x];
                    if (!v) continue;
                    count[v]++;
                    uint16_t c = vdp_opts.outfit && vdp_outfit_pick[v] != OUTFIT_KEEP
                                     ? vdp_outfit_shade(vdp_outfit_pick[v], vdp_player_cram[v], vdp_outfit_ref[v]) : vdp_player_cram[v];
                    uint32_t rgbv = (uint32_t)(((c >> 1) & 7) * 36) << 16 | (uint32_t)(((c >> 5) & 7) * 36) << 8 | (uint32_t)(((c >> 9) & 7) * 36);
                    for (int k = 0; k < 36; k++) img[(size_t)(y * 6 + k / 6) * w * 6 + x * 6 + k % 6] = rgbv;
                }
            for (int v = 0; v < 64; v++)
                if (count[v]) printf("  index %2d (palette %d colour %2d): %4d pixels, CRAM %03X\n", v, v >> 4, v & 15, count[v], vdp_player_cram[v]);
            if (w && h) png_write(outfit_shot, img, w * 6, h * 6, w * 6);
            free(img);
        }
        if (hd_shot) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            hd_compose(settings.internal_scale);
            double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency();
            printf("hd frame %dx%d in %.2f ms\n", hd_w, hd_h, ms);
            png_write(hd_shot, hd_px, hd_w, hd_h, hd_w);
        }
        if (save_preview) {
            HdPost here = finishing(1, 0);
            preview_set_world(here.world, here.cam_x, here.cam_y, ali_x(), ali_y());
            preview_save(base_dir);
        }
        if (save_to && !savestate_save(save_to, framebuffer, frame_width(), vdp_screen_height(), FB_W))
            fprintf(stderr, "could not save slot %d\n", save_to);
        if (menu_shot) {
            SDL_Rect r = display_rect(menu_w, menu_h);
            menu_open(framebuffer, frame_width(), vdp_screen_height(), FB_W, r.x, r.y, r.w, r.h, menu_w, menu_h, 1);
            if (controls_shot) controls_open();
            if (outfit_page_shot) outfit_open();
            if (menu_tab >= 0) {
                HdPost here = finishing(1, 0);
                preview_set_world(here.world, here.cam_x, here.cam_y, ali_x(), ali_y());
                menu_select(menu_tab, menu_row);
            }
            menu_render(1.0);
            png_write(menu_shot, ui_px, ui_w, ui_h, ui_w);
        }
        save_coverage();
        /* Headless runs are tests: they never write the player's save RAM (save.srm),
         * whatever the scripted input or a loaded state did to it. */
        printf("ran %d frames, screen %dx%d\n", headless_frames, frame_width(), vdp_screen_height());
        if (stat_total_cycles)
            printf("68000 at %d%%: %.1f%% of its time spare, %u game ticks, %u late (slowdown)\n", machine_opts.overclock,
                   100.0 * stat_wait_cycles / stat_total_cycles, stat_ticks, stat_late_ticks);
        if (stat_native_cycles + stat_interp_cycles)
            printf("68000 time: %.2f%% native, %.2f%% interpreted\n",
                   100.0 * stat_native_cycles / (stat_native_cycles + stat_interp_cycles),
                   100.0 * stat_interp_cycles / (stat_native_cycles + stat_interp_cycles));
        if (machine_opts.verify) printf("verified %u block runs, %u mismatches\n", verify_checked, verify_failed);
        return 0;
    }

    SDL_Window *win = SDL_CreateWindow("Beyond Oasis", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       frame_width() * settings.window_scale * 15 / 14, 224 * settings.window_scale,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                       (settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, FB_W, FB_H);
    SDL_Texture *hd_tex[2] = { NULL, NULL };   /* the frame at the internal resolution, two in turn */
    int hd_tex_now = 0;
    int hd_tex_w = 0, hd_tex_h = 0;
    bool vsync_on = false;
    SDL_GameController *pad = NULL;
    SDL_AudioSpec want = { .freq = 48000, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024 }, have;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) { sound_out_rate = have.freq; SDL_PauseAudioDevice(audio, 0); }
    menu_audio = audio;
    menu_sound = play_menu_sound;

    /* Short messages (save, load, slot) show in the window title for two seconds. */
    char message[128] = "";
    Uint64 message_until = 0;
#define NOTIFY(...) do { snprintf(message, sizeof message, __VA_ARGS__); \
                         message_until = SDL_GetPerformanceCounter() + SDL_GetPerformanceFrequency() * 2; } while (0)
    char shown_title[160] = "";

    const double frame_s = 1.0 / 59.922743;     /* NTSC Genesis refresh rate */
    Uint64 freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter(), start = next;
    SDL_Texture *menu_tex = NULL;
    int menu_tex_w = 0, menu_tex_h = 0;
    bool running = true;
    while (running) {
        SDL_Event e;
        int ww, wh;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        while (SDL_PollEvent(&e)) {
            /* The pause menu binding opens the menu; Esc and the Guide button always do too. */
            bool open_menu = input_event_is(IN_MENU, &e) ||
                             (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !e.key.repeat) ||
                             (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE);
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            else if (menu_is_open()) {
                bool was_rewind = settings.rewind;
                switch (menu_event(&e)) {
                case MENU_CLOSE: case MENU_LOADED: menu_close(); break;
                case MENU_SETTINGS:
                    apply_settings();
                    SDL_SetWindowFullscreen(win, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    if (settings.rewind != was_rewind) rewind_reset(settings.rewind ? (size_t)settings.rewind_mb << 20 : 0);
                    break;
                case MENU_RESET:
                    machine_reset();
                    rewind_reset(settings.rewind ? (size_t)settings.rewind_mb << 20 : 0);
                    menu_close();
                    break;
                case MENU_QUIT: running = false; break;
                case MENU_NONE: break;
                }
            } else if (open_menu) {
                int win_w, win_h;
                SDL_GetWindowSize(win, &win_w, &win_h);
                SDL_Rect r = display_rect(ww, wh);
                outfit_preview_save(base_dir);  /* Ali as the launcher's outfit page will show him */
                HdPost here = finishing(1, 0);
                preview_set_world(here.world, here.cam_x, here.cam_y, ali_x(), ali_y());
                preview_save(base_dir);         /* the frame the launcher's previews will show */
                menu_open(framebuffer, frame_width(), vdp_screen_height(), FB_W, r.x, r.y, r.w, r.h, ww, wh,
                          win_w ? (float)ww / win_w : 1);
            }
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
                settings.fullscreen = !settings.fullscreen;
                SDL_SetWindowFullscreen(win, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F5) {
                settings.widescreen = !settings.widescreen;
                apply_settings();
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F2 && !e.key.repeat) {
                if (savestate_save(settings.state_slot, framebuffer, frame_width(), vdp_screen_height(), FB_W))
                    NOTIFY("Saved to slot %d", settings.state_slot);
                else NOTIFY("Could not save slot %d", settings.state_slot);
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F4 && !e.key.repeat) {
                if (savestate_load(settings.state_slot)) NOTIFY("Loaded slot %d", settings.state_slot);
                else NOTIFY("Slot %d is empty or from another version", settings.state_slot);
            } else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_F6 || e.key.keysym.sym == SDLK_F7)) {
                settings.state_slot = (settings.state_slot + (e.key.keysym.sym == SDLK_F7 ? 1 : 7)) % 9 + 1;
                NOTIFY("Slot %d", settings.state_slot);
            }
        }
        if (menu_is_open()) {
            if (ww != ui_w || wh != ui_h) {     /* resized or fullscreen toggled: rebuild the backdrop */
                int win_w, win_h;
                SDL_GetWindowSize(win, &win_w, &win_h);
                SDL_Rect r = display_rect(ww, wh);
                menu_open(framebuffer, frame_width(), vdp_screen_height(), FB_W, r.x, r.y, r.w, r.h, ww, wh,
                          win_w ? (float)ww / win_w : 1);
            }
            menu_render((double)(SDL_GetPerformanceCounter() - start) / freq);
            if (!menu_tex || menu_tex_w != ui_w || menu_tex_h != ui_h) {
                if (menu_tex) SDL_DestroyTexture(menu_tex);
                menu_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, ui_w, ui_h);
                menu_tex_w = ui_w; menu_tex_h = ui_h;
            }
            SDL_UpdateTexture(menu_tex, NULL, ui_px, ui_w * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, menu_tex, NULL, NULL);
            SDL_RenderPresent(ren);
            if (strcmp(shown_title, "Beyond Oasis - Paused")) { SDL_SetWindowTitle(win, "Beyond Oasis - Paused"); strcpy(shown_title, "Beyond Oasis - Paused"); }
            next += (Uint64)(frame_s * freq);
            Uint64 now = SDL_GetPerformanceCounter();
            if (now < next) SDL_Delay((Uint32)((next - now) * 1000 / freq));
            else next = now;
            continue;
        }

        /* One pass of this loop is one display refresh: vsync paces it. A game frame runs
         * whenever one is due (60 times a second, on any display), and every refresh shows
         * the moment it is now, so motion is even at 60, 120, 144 Hz and up. */
        Uint64 now = SDL_GetPerformanceCounter();
        const Uint64 frame_ticks = (Uint64)(frame_s * freq);
        if (now >= next) {
            const Uint8 *keys = SDL_GetKeyboardState(NULL);
            bool rewinding = settings.rewind && input_held(IN_REWIND, keys, pad);
            uint16_t old_pad;
            if (rewinding && rewind_step(&old_pad)) {
                /* Redraw the frame we stepped back to, then return to the state before it. */
                pad_state[0] = old_pad;
                machine_run_frame(framebuffer, FB_W);
                if (vdp_opts.hd_capture) { hd_frame_done(); fx_after_frame(); }
                rewind_restore_head();
                sound_out_count = 0;            /* silent while rewinding */
            } else if (!rewinding) {
                pad_state[0] = input_pad_bits(keys, pad);
                rewind_push(pad_state[0]);
                cheats_frame();
                machine_run_frame(framebuffer, FB_W);
                if (vdp_opts.hd_capture) { hd_frame_done(); fx_after_frame(); }
            } else {
                sound_out_count = 0;            /* at the start of the buffer: hold the picture */
            }
            save_sram();

            char title[160];
            if (rewinding) snprintf(title, sizeof title, "Beyond Oasis - Rewinding (%.1f s left)", rewind_frames() / 60.0);
            else if (SDL_GetPerformanceCounter() < message_until) snprintf(title, sizeof title, "Beyond Oasis - %s", message);
            else snprintf(title, sizeof title, "Beyond Oasis");
            if (strcmp(title, shown_title)) { SDL_SetWindowTitle(win, title); strcpy(shown_title, title); }
            if (audio) {
                /* Keep about 3 frames queued: drop audio if we fall far behind. */
                if (SDL_GetQueuedAudioSize(audio) < (Uint32)sound_out_rate * 4 / 10) {
                    if (settings.volume < 100) {
                        int v = settings.volume * 256 / 100;
                        for (int i = 0; i < sound_out_count * 2; i++) sound_out[i] = (int16_t)(sound_out[i] * v / 256);
                    }
                    SDL_QueueAudio(audio, sound_out, (Uint32)sound_out_count * 4);
                }
            }
            next += frame_ticks;
            if (now > next + freq / 4) next = now + frame_ticks;  /* fell far behind: don't try to catch up */
        }
        if (!vsync_on) { SDL_RenderSetVSync(ren, 1); vsync_on = true; }

        /* The moment this picture shows: how far we are into the newest game frame (0-1). */
        now = SDL_GetPerformanceCounter();
        float frac = next > now ? 1 - (float)(next - now) / frame_ticks : 1;
        if (frac < 0) frac = 0;
        SDL_Rect dst = display_rect(ww, wh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        /* Shadows are drawn while the layers are put together, so they need this path too. */
        bool hd = (settings.internal_scale > 1 || settings.smooth_motion || settings.shadows || settings.water || hd_texpack) &&
                  hd_frame.h == vdp_screen_height();
        hd_time = (float)(SDL_GetPerformanceCounter() - start) / freq;
        if (hd) {
            if (settings.smooth_motion) hd_compose_at(settings.internal_scale, frac);
            else hd_compose(settings.internal_scale);
            /* Two textures used in turn: uploading into the one the GPU just drew from
             * can make the upload wait for the GPU. */
            if (!hd_tex[0] || hd_tex_w != hd_w || hd_tex_h != hd_h) {
                for (int k = 0; k < 2; k++) {
                    if (hd_tex[k]) SDL_DestroyTexture(hd_tex[k]);
                    hd_tex[k] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, hd_w, hd_h);
                    /* the last, non-integer step to the window; native pixels stay sharp */
                    SDL_SetTextureScaleMode(hd_tex[k], settings.internal_scale > 1 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
                }
                hd_tex_w = hd_w; hd_tex_h = hd_h;
            }
            hd_tex_now ^= 1;
            HdPost post = finishing(hd_w / frame_width(), (float)(SDL_GetPerformanceCounter() - start) / freq);
            SDL_UpdateTexture(hd_tex[hd_tex_now], NULL, hd_post(hd_px, hd_w, hd_h, hd_w, &post), hd_w * 4);
            filter_draw(ren, hd_tex[hd_tex_now], NULL, dst, frame_width(), vdp_screen_height(), settings.filter);
        } else {
            SDL_Rect src = { 0, 0, frame_width(), vdp_screen_height() };
            HdPost post = finishing(1, (float)(SDL_GetPerformanceCounter() - start) / freq);
            const uint32_t *img = hd_post(framebuffer, src.w, src.h, FB_W, &post);
            if (img != framebuffer) SDL_UpdateTexture(tex, &src, img, src.w * 4);
            else SDL_UpdateTexture(tex, NULL, framebuffer, FB_W * 4);
            filter_draw(ren, tex, &src, dst, frame_width(), vdp_screen_height(), settings.filter);
        }
        Uint64 before_present = SDL_GetPerformanceCounter();
        SDL_RenderPresent(ren);
        /* If the driver ignores vsync, don't spin: wait about one refresh. */
        Uint64 after = SDL_GetPerformanceCounter();
        if (after - before_present < freq / 2000) {
            SDL_DisplayMode mode;
            int refresh = SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(win), &mode) == 0 && mode.refresh_rate > 0
                          ? mode.refresh_rate : 60;
            Uint64 until = before_present + freq / (Uint64)refresh;
            if (until > after) SDL_Delay((Uint32)((until - after) * 1000 / freq));
        }
    }

    save_coverage();
    save_sram();
    outfit_preview_save(base_dir);
    if (!menu_is_open()) {
        HdPost here = finishing(1, 0);
        preview_set_world(here.world, here.cam_x, here.cam_y, ali_x(), ali_y());
    }
    preview_save(base_dir);
    if (!test_overrides) settings_save(ini);
    SDL_Quit();
    return 0;
}
