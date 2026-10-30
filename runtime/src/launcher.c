/* Launcher: a settings screen shown before the game. The Gold Armlet floats over a desert
 * at dusk with the four spirits circling it (launcher_scene.c); the settings sit on a stone
 * tablet with one tab per spirit, and the game's soundtrack plays through a built-in player,
 * with the game's own menu sounds for every choice.
 *
 * The artwork comes from the user's own ROM: the launcher runs the game's intro
 * silently and captures the armlet scene and the title screen, then caches them in
 * launcher_art.cache next to the executable. Everything is drawn in software into one
 * ui_px (also used for headless screenshots), at the window's pixel scale. */
#include "launcher.h"
#include "ost.h"
#include "launcher_scene.h"
#include "recomp_rt.h"
#include "genesis.h"
#include "settings.h"
#include "ui.h"
#include "png.h"
#include "controls.h"
#include "outfit.h"
#include "preview.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#endif

#define BASE_W UI_BASE_W
#define BASE_H UI_BASE_H

/* ---- Artwork captured from the ROM -------------------------------------------- */
static Art armlet, logo;

#define ART_MAGIC    0x34524142u       /* cache file tag (bump when the capture changes) */

static char cache_path[1100];

static void art_free(Art *a) { free(a->rgb); free(a->mask); memset(a, 0, sizeof *a); }

/* Crops the pixels of a 320x224 frame that `keep` selects into an Art. */
static void art_crop(Art *a, const uint32_t *fb, int pitch, int (*keep)(uint32_t), int y_limit) {
    int x0 = 320, y0 = 224, x1 = -1, y1 = -1;
    for (int y = 0; y < y_limit; y++)
        for (int x = 0; x < 320; x++)
            if (keep(fb[y * pitch + x])) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    art_free(a);
    if (x1 < 0) return;
    a->w = x1 - x0 + 1; a->h = y1 - y0 + 1;
    a->rgb = malloc((size_t)a->w * a->h * 4);
    a->mask = malloc((size_t)a->w * a->h);
    for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w; x++) {
            uint32_t p = fb[(y0 + y) * pitch + x0 + x];
            a->rgb[y * a->w + x] = p;
            a->mask[y * a->w + x] = keep(p) ? 255 : 0;
        }
}

static int is_lit(uint32_t p) { int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255; return r > 28 || g > 28 || b > 28; }
static int is_gold(uint32_t p) { int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255; return r > 100 && r >= g && r > b + 50; }
/* Title lettering: gold faces and their dark brown outlines (the stone behind is blue-green). */
static int is_letter(uint32_t p) { int r = (p >> 16) & 255, b = p & 255; return r >= b + 6; }
static int is_teal(uint32_t p) { int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255; return g > r + 12 && b > r + 12; }

static int count(const uint32_t *fb, int (*pred)(uint32_t), int y_limit) {
    int n = 0;
    for (int i = 0; i < 320 * y_limit; i++) n += pred(fb[i]);
    return n;
}

/* The intro's Gold Armlet scene: a large gold object on black, fully on screen and still. */
static bool armlet_shown(const uint32_t *fb, int *last) {
    int gold = count(fb, is_gold, 224), lit = count(fb, is_lit, 224);
    bool edge = false;
    for (int y = 0; y < 224; y++) edge |= is_lit(fb[y * 320 + 318]) || is_lit(fb[y * 320 + 1]);
    bool still = abs(gold - *last) < 40;
    *last = gold;
    return gold > 4000 && lit < 320 * 224 / 2 && !edge && still;
}

/* The title screen: gold lettering over the teal stone background, finished fading in. */
static bool title_shown(const uint32_t *fb, int *last) {
    int gold = count(fb, is_gold, 140), teal = count(fb, is_teal, 224);
    bool still = abs(gold - *last) < 20;
    *last = gold;
    return gold > 900 && teal > 25000 && still;
}

/* Runs the game until `shown` accepts a frame (checked every few frames), pressing Start
 * every `press_every` frames if nonzero. Returns false if it never appears. */
static bool run_until(uint32_t *fb, bool (*shown)(const uint32_t *, int *), int press_every, int limit) {
    int last = -1000;
    for (int f = 0; f < limit; f++) {
        pad_state[0] = press_every && f >= 500 && f % press_every < 4 ? PAD_START : 0;
        bool check = f >= 300 && f % 4 == 0;
        machine_run_frame(check ? fb : NULL, 320);
        if (check && shown(fb, &last)) { pad_state[0] = 0; return true; }
    }
    pad_state[0] = 0;
    return false;
}

static bool art_save_cache(void) {
    FILE *f = fopen(cache_path, "wb");
    if (!f) return false;
    uint32_t magic = ART_MAGIC;
    fwrite(&magic, 4, 1, f);
    Art *arts[2] = { &armlet, &logo };
    for (int i = 0; i < 2; i++) {
        fwrite(&arts[i]->w, 4, 1, f); fwrite(&arts[i]->h, 4, 1, f);
        fwrite(arts[i]->rgb, 4, (size_t)arts[i]->w * arts[i]->h, f);
        fwrite(arts[i]->mask, 1, (size_t)arts[i]->w * arts[i]->h, f);
    }
    fclose(f);
    return true;
}

static bool art_load_cache(void) {
    FILE *f = fopen(cache_path, "rb");
    if (!f) return false;
    uint32_t magic = 0;
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == ART_MAGIC;
    Art *arts[2] = { &armlet, &logo };
    for (int i = 0; ok && i < 2; i++) {
        Art *a = arts[i];
        art_free(a);
        ok = fread(&a->w, 4, 1, f) == 1 && fread(&a->h, 4, 1, f) == 1 && a->w > 0 && a->h > 0 && a->w <= 320 && a->h <= 224;
        if (!ok) break;
        a->rgb = malloc((size_t)a->w * a->h * 4);
        a->mask = malloc((size_t)a->w * a->h);
        ok = fread(a->rgb, 4, (size_t)a->w * a->h, f) == (size_t)a->w * a->h &&
             fread(a->mask, 1, (size_t)a->w * a->h, f) == (size_t)a->w * a->h;
    }
    fclose(f);
    if (!ok) { art_free(&armlet); art_free(&logo); }
    return ok;
}

/* Runs the intro twice (about 1.5 s) to capture the armlet and the title logo. */
static void art_capture(void) {
    static uint32_t fb[320 * 240];
    int extra = vdp_opts.extra_width;
    bool mask = vdp_opts.mask_sides;
    vdp_opts.extra_width = 0;
    widescreen_patches(0);
    /* Start from blank save RAM so the intro plays the same for everyone. */
    uint8_t *sram = malloc(SRAM_SIZE);
    memcpy(sram, bus.sram, SRAM_SIZE);
    memset(bus.sram, 0xFF, SRAM_SIZE);
    machine_reset();
    if (run_until(fb, armlet_shown, 0, 2400)) art_crop(&armlet, fb, 320, is_lit, 224);
    machine_reset();
    if (run_until(fb, title_shown, 100, 3000)) {
        /* Find the lettering by its gold, then take whole letterforms inside that box. */
        Art gold = { 0 };
        art_crop(&gold, fb, 320, is_gold, 140);
        art_crop(&logo, fb, 320, is_letter, 140);
        art_free(&gold);
        /* Close one-pixel gaps in the strokes, taking the color of a neighboring letter pixel. */
        uint8_t *m = malloc((size_t)logo.w * logo.h);
        memcpy(m, logo.mask, (size_t)logo.w * logo.h);
        for (int y = 1; y < logo.h - 1; y++)
            for (int x = 1; x < logo.w - 1; x++) {
                if (logo.mask[y * logo.w + x]) continue;
                int n = 0, from = -1;
                for (int k = 0; k < 9; k++) {
                    int i = (y - 1 + k / 3) * logo.w + x - 1 + k % 3;
                    if (logo.mask[i]) { n++; from = i; }
                }
                if (n >= 5) { m[y * logo.w + x] = 255; logo.rgb[y * logo.w + x] = logo.rgb[from]; }
            }
        memcpy(logo.mask, m, (size_t)logo.w * logo.h);
        free(m);
    }
    memcpy(bus.sram, sram, SRAM_SIZE);
    free(sram);
    vdp_opts.extra_width = extra;
    vdp_opts.mask_sides = mask;
    widescreen_patches(extra);
    pad_state[0] = 0;
    if (armlet.w && logo.w) art_save_cache();
}

static bool rom_ok;

static void art_prepare(void) {
    art_free(&armlet); art_free(&logo);
    rom_ok = settings.rom_path[0] && !bus_load_rom(settings.rom_path);
    if (!rom_ok) return;
    if (!art_load_cache()) art_capture();
}

static void init_common(void);

/* ---- Soundtrack player ------------------------------------------------------------ *
 * The launcher plays the game's own soundtrack. A background thread runs the ROM from a
 * reset (with blank save RAM, like the art capture) to the title screen in a fraction of
 * a second without sound, notes which song the title asked for, then parks the game and
 * drives its sound driver directly (ost.c): any of the 34 songs, and the game's own menu
 * sounds for the launcher's choices. Songs that loop play for PLAY_FOR seconds and fade
 * out; jingles end when they go quiet. The thread owns the machine until music_stop();
 * the game resets it afterwards. */
#define PLAY_FOR   (3 * 60)             /* seconds a looping song plays before the next */
#define FADE_OUT   (4 * 60)             /* frames */
#define FADE_STOP  15                   /* frames: quitting or switching off */
#define METER_BARS 24

static SDL_Thread *music_thread;
static SDL_AudioDeviceID music_dev;
static SDL_mutex *music_lock;
static uint8_t *music_sram;             /* the real save RAM, put back by music_stop() */
static bool music_sram_dirty;
static uint32_t music_fb[320 * 240];
static int title_song = -1;             /* the song the title screen starts */

/* Requests from the launcher and what the player is doing, under music_lock. */
static struct {
    bool quit;
    int  want_song;                     /* -1: none */
    int  step;                          /* +1 / -1: next / previous song */
    bool want_on, want_off;
    uint16_t sfx[8];
    int  sfx_count;
    /* published by the player */
    bool ready, on;
    int  song, frames;                  /* the song playing, and for how long */
    float meter[METER_BARS];            /* level of the latest frame, in slices, 0-1 */
} player;

/* Records the songs the game asks for while it runs to the title. */
static uint32_t note_song(uint32_t pc) {
    (void)pc;
    if ((D(0) & 0xFF00) == 0x0100) title_song = (int)(D(0) & 0xFF);
    return 0x06042A;                    /* on to the driver, as `bra` at $060004 does */
}

static bool music_to_title(void) {
    vdp_opts.extra_width = 0;
    widescreen_patches(0);
    memset(bus.sram, 0xFF, SRAM_SIZE);
    machine_reset();
    machine_override(0x060004, note_song);
    bool ok = run_until(music_fb, title_shown, 100, 3000);
    machine_override(0x060004, NULL);
    return ok;
}

/* One frame of sound, scaled by volume (0-1). Returns the stereo frames in sound_out and
 * the frame's level without its DC offset (the FM DAC can hold a constant level). */
static int music_frame(float volume, double *level) {
    ost_frame();
    sound_quality = settings.audio;     /* the Audio setting applies to the player too */
    machine_run_frame(NULL, 320);
    double sum = 0, sum2 = 0;
    int n = sound_out_count * 2;
    for (int i = 0; i < n; i++) { sum += sound_out[i]; sum2 += (double)sound_out[i] * sound_out[i]; }
    *level = n ? sqrt(fmax(0, sum2 / n - (sum / n) * (sum / n))) : 0;
    int v = (int)(volume * settings.volume / 100.f * 256);
    for (int i = 0; i < n; i++) sound_out[i] = (int16_t)(sound_out[i] * v / 256);
    return sound_out_count;
}

static int next_song(int song, int dir) {
    if (settings.ost_mode == OST_MODE_SHUFFLE && dir > 0) {
        int s = (int)(hash32((uint32_t)SDL_GetPerformanceCounter()) % (OST_SONGS - 1));
        return s >= song ? s + 1 : s;   /* any other song */
    }
    return settings_cycle(song, OST_SONGS, dir);
}

/* The player's state, kept by the thread (and by launcher_music_check()). */
typedef struct {
    int song, frames;
    bool on;
    float volume, peak;
    double top, quiet;                  /* loudest second so far; frames it has been quiet */
    int fading;                         /* frames of fade left (0: not fading) */
    int after_fade;                     /* song to start when the fade ends, -1 to stop */
} Player;

static void start_song(Player *p, int song) {
    p->song = song;
    p->frames = 0;
    p->top = p->quiet = 0;
    p->fading = 0;
    p->volume = p->peak;
    ost_command(OST_STOP);
    ost_command(OST_PLAY(song));
}

static void fade_to(Player *p, int song, int frames) {
    if (p->fading) { p->after_fade = song; return; }
    p->fading = frames;
    p->after_fade = song;
}

/* Runs one frame of the player; returns the stereo frames in sound_out. */
static int player_frame(Player *p) {
    double level;
    if (p->fading) {
        p->volume -= p->volume / p->fading;
        if (--p->fading == 0) {
            p->volume = 0;
            if (p->after_fade >= 0) start_song(p, p->after_fade);
            else { ost_command(OST_STOP); p->on = false; p->volume = p->peak; }
        }
    }
    int n = music_frame(p->on || p->fading ? p->volume : p->peak, &level);
    if (!p->on || p->fading) return n;
    p->frames++;
    /* A jingle is over once it stays quiet (well under its loudest part) for 1.5 s. */
    if (level > p->top) p->top = level;
    p->quiet = level < fmax(120, p->top * .03) ? p->quiet + 1 : 0;
    bool ended = p->frames > 3 * 60 && p->quiet > 90;
    bool long_enough = settings.ost_mode != OST_MODE_REPEAT && p->frames >= PLAY_FOR * 60;
    if (ended) start_song(p, settings.ost_mode == OST_MODE_REPEAT ? p->song : next_song(p->song, 1));
    else if (long_enough) fade_to(p, next_song(p->song, 1), FADE_OUT);
    return n;
}

static void publish(const Player *p) {
    float m[METER_BARS];
    int n = sound_out_count;
    for (int b = 0; b < METER_BARS; b++) {
        double s = 0;
        int i0 = n * b / METER_BARS, i1 = n * (b + 1) / METER_BARS, c = 0;
        for (int i = i0; i < i1; i++) { double v = (sound_out[i * 2] + sound_out[i * 2 + 1]) * .5; s += v * v; c++; }
        m[b] = c ? clampf((float)sqrt(s / c) / 9000.f, 0, 1) : 0;
    }
    SDL_LockMutex(music_lock);
    player.ready = true;
    player.on = p->on && !(p->fading && p->after_fade < 0);
    player.song = p->song;
    player.frames = p->frames;
    for (int b = 0; b < METER_BARS; b++) player.meter[b] = p->on ? fmaxf(m[b], player.meter[b] * .82f) : player.meter[b] * .8f;
    SDL_UnlockMutex(music_lock);
}

static int music_run(void *arg) {
    (void)arg;
    if (!music_to_title()) return 0;
    ost_start();
    Player p = { .song = title_song >= 0 ? title_song : 0, .peak = .8f, .after_fade = -1 };
    if (settings.ost_track >= 0 && settings.ost_track < OST_SONGS) p.song = settings.ost_track;
    p.volume = p.peak;
    p.on = settings.launcher_music;
    if (p.on) start_song(&p, p.song);
    else ost_command(OST_STOP);
    const Uint32 target = (Uint32)sound_out_rate * 4 / 10;      /* keep about 100 ms queued */
    bool quitting = false;
    for (;;) {
        SDL_LockMutex(music_lock);
        bool quit = player.quit;
        int want = player.want_song, step = player.step;
        bool want_on = player.want_on, want_off = player.want_off;
        for (int k = 0; k < player.sfx_count; k++) ost_command(player.sfx[k]);
        player.sfx_count = 0;
        player.want_song = -1; player.step = 0; player.want_on = player.want_off = false;
        SDL_UnlockMutex(music_lock);

        if (quit && !quitting) { quitting = true; if (p.on) fade_to(&p, -1, FADE_STOP); }
        if (!quitting) {
            if (step) want = next_song(p.song, step);
            if (want >= 0) { p.on = true; start_song(&p, want); }
            if (want_on && !p.on) { p.on = true; start_song(&p, p.song); }
            if (want_off && p.on) fade_to(&p, -1, FADE_STOP);
        }
        if (quitting && !p.fading) break;
        if (SDL_GetQueuedAudioSize(music_dev) > target) { SDL_Delay(2); continue; }
        int n = player_frame(&p);
        SDL_QueueAudio(music_dev, sound_out, (Uint32)n * 4);
        publish(&p);
    }
    ost_end();
    return 0;
}

static void music_start(void) {
    if (music_thread || !rom_ok) return;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want = { .freq = 48000, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024 }, have;
    music_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!music_dev) return;
    sound_out_rate = have.freq;
    if (!music_lock) music_lock = SDL_CreateMutex();
    memset(&player, 0, sizeof player);
    player.want_song = -1;
    music_sram = malloc(SRAM_SIZE);
    memcpy(music_sram, bus.sram, SRAM_SIZE);
    music_sram_dirty = bus.sram_dirty;
    SDL_PauseAudioDevice(music_dev, 0);
    music_thread = SDL_CreateThread(music_run, "music", NULL);
}

static void music_stop(void) {
    if (!music_thread) return;
    SDL_LockMutex(music_lock);
    player.quit = true;
    SDL_UnlockMutex(music_lock);
    SDL_WaitThread(music_thread, NULL);
    music_thread = NULL;
    SDL_Delay(60);                      /* let the faded tail play out */
    SDL_CloseAudioDevice(music_dev);
    music_dev = 0;
    memcpy(bus.sram, music_sram, SRAM_SIZE);
    bus.sram_dirty = music_sram_dirty;
    free(music_sram);
    music_sram = NULL;
    vdp_opts.extra_width = settings_extra_width();
    widescreen_patches(vdp_opts.extra_width);
    pad_state[0] = 0;
}

/* Requests to the player; they do nothing before it is running. */
static void music_sfx(uint16_t sfx) {
    if (!music_thread) return;
    SDL_LockMutex(music_lock);
    if (player.sfx_count < 8) player.sfx[player.sfx_count++] = sfx;
    SDL_UnlockMutex(music_lock);
}

static void music_request(int song, int step, bool on, bool off) {
    if (!music_thread) return;
    SDL_LockMutex(music_lock);
    if (song >= 0) player.want_song = song;
    player.step += step;
    player.want_on |= on;
    player.want_off |= off;
    SDL_UnlockMutex(music_lock);
}

/* --launcher-music-check: the player without an audio device, at full speed. Prints each
 * second's level and every song change, starting with the title theme. */
int launcher_music_check(int seconds) {
    init_common();
    if (!rom_ok) {
        const char *why = settings.rom_path[0] ? bus_load_rom(settings.rom_path) : "no ROM path in settings";
        fprintf(stderr, "could not load the ROM: %s\n", why ? why : "?");
        return 1;
    }
    uint8_t *sram = malloc(SRAM_SIZE);
    memcpy(sram, bus.sram, SRAM_SIZE);
    bool dirty = bus.sram_dirty;
    Uint64 t0 = SDL_GetPerformanceCounter();
    bool ok = music_to_title();
    printf("title reached in %.0f ms: %s; the title theme is song %02X\n",
           (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency(), ok ? "yes" : "NO", title_song);
    if (ok) {
        ost_start();
        Player p = { .peak = 1, .volume = 1, .on = true, .after_fade = -1 };
        start_song(&p, title_song >= 0 ? title_song : 0);
        int shown = -1;
        for (int s = 0; s < seconds; s++) {
            double sum = 0;
            long n = 0;
            for (int f = 0; f < 60; f++) {
                if (p.song != shown) { printf("  second %d: song %02X\n", s, p.song); shown = p.song; }
                int c = player_frame(&p);
                for (int i = 0; i < c * 2; i++) { sum += (double)sound_out[i] * sound_out[i]; n++; }
            }
            if (s % 30 == 0) printf("second %4d: level %5.0f rms\n", s, n ? sqrt(sum / n) : 0);
        }
        ost_end();
    }
    memcpy(bus.sram, sram, SRAM_SIZE);
    bus.sram_dirty = dirty;
    free(sram);
    return ok ? 0 : 1;
}

/* Renders the four menu sounds the launcher uses into `path` for the pause menu, which
 * cannot call the driver while the game is paused: 48 kHz 16-bit stereo, each sound's
 * frame count first. Runs the ROM to the title like the player (about half a second). */
bool launcher_make_menu_sounds(const char *path) {
    if (!rom_ok && !(settings.rom_path[0] && !bus_load_rom(settings.rom_path))) return false;
    rom_ok = true;
    int rate = sound_out_rate, quality = sound_quality;
    sound_out_rate = 48000;
    sound_quality = 0;
    bool ok = music_to_title();
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        ost_start();
        static const uint16_t sfx[4] = { SFX_CURSOR, SFX_CONFIRM, SFX_CANCEL, SFX_ERROR };
        static int16_t pcm[48000 * 2];
        for (int k = 0; k < 4; k++) {
            ost_command(OST_STOP);
            for (int i = 0; i < 20; i++) { ost_frame(); machine_run_frame(NULL, 320); }
            ost_command(sfx[k]);
            int n = 0;
            for (int i = 0; i < 36; i++) {      /* 0.6 s */
                ost_frame();
                machine_run_frame(NULL, 320);
                if (n + sound_out_count <= 48000) { memcpy(pcm + n * 2, sound_out, (size_t)sound_out_count * 4); n += sound_out_count; }
            }
            /* Without the DC offset, faded out over the last 20 ms. */
            double dc[2] = { 0, 0 };
            for (int i = 0; i < n; i++) { dc[0] += pcm[i * 2]; dc[1] += pcm[i * 2 + 1]; }
            for (int i = 0; i < n; i++)
                for (int c = 0; c < 2; c++) {
                    double v = pcm[i * 2 + c] - dc[c] / n;
                    if (i > n - 960) v *= (n - i) / 960.0;
                    pcm[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
                }
            fwrite(&n, 4, 1, f);
            fwrite(pcm, 4, (size_t)n, f);
        }
        ost_end();
        fclose(f);
    }
    sound_out_rate = rate;
    sound_quality = quality;
    return f != NULL;
}

/* Plays every song for up to `seconds` and prints its level per second (in tenths of the
 * loudest second), to tell songs from jingles and empty entries. */
int launcher_ost_check(int seconds) {
    init_common();
    if (!rom_ok) { fprintf(stderr, "could not load the ROM\n"); return 1; }
    uint8_t *sram = malloc(SRAM_SIZE);
    memcpy(sram, bus.sram, SRAM_SIZE);
    bool dirty = bus.sram_dirty;
    bool ok = music_to_title();
    ost_start();
    for (int song = 0; ok && song < OST_SONGS; song++) {
        ost_command(OST_STOP);
        ost_command(OST_PLAY(song));
        double level[600] = { 0 }, top = 0;
        for (int s = 0; s < seconds && s < 600; s++) {
            double sum = 0, sum2 = 0;
            long n = 0;
            for (int f = 0; f < 60; f++) {
                ost_frame();
                machine_run_frame(NULL, 320);
                for (int i = 0; i < sound_out_count * 2; i++) { sum += sound_out[i]; sum2 += (double)sound_out[i] * sound_out[i]; n++; }
            }
            /* Without the DC offset (the FM DAC holds a constant level after a jingle). */
            level[s] = n ? sqrt(fmax(0, sum2 / n - (sum / n) * (sum / n))) : 0;
            if (level[s] > top) top = level[s];
        }
        printf("song %02X peak %5.0f: ", song, top);
        for (int s = 0; s < seconds && s < 600; s++) putchar(top > 0 ? '0' + (int)(level[s] * 9.99 / top) : '.');
        putchar('\n');
    }
    /* The menu sound effects over silence: level of the first half second. */
    static const uint16_t sfx[] = { SFX_CURSOR, SFX_CONFIRM, SFX_CANCEL, SFX_ERROR };
    for (int k = 0; ok && k < 4; k++) {
        ost_command(OST_STOP);
        for (int f = 0; f < 30; f++) { ost_frame(); machine_run_frame(NULL, 320); }
        ost_command(sfx[k]);
        double sum = 0, sum2 = 0;
        long n = 0;
        for (int f = 0; f < 30; f++) {
            ost_frame();
            machine_run_frame(NULL, 320);
            for (int i = 0; i < sound_out_count * 2; i++) { sum += sound_out[i]; sum2 += (double)sound_out[i] * sound_out[i]; n++; }
        }
        printf("sound effect %04X: level %.0f\n", sfx[k], n ? sqrt(fmax(0, sum2 / n - (sum / n) * (sum / n))) : 0);
    }
    ost_end();
    memcpy(bus.sram, sram, SRAM_SIZE);
    bus.sram_dirty = dirty;
    free(sram);
    return ok ? 0 : 1;
}

/* ---- The tablet: four tabs, one per spirit ---------------------------------------- */
typedef enum { ROW_OPTION, ROW_ROM, ROW_CONTROLS, ROW_TRACK, ROW_OUTFIT } RowKind;
typedef struct { RowKind kind; const char *label; int id; const char *about; } Row;

#define TABS 4
static const char *const tab_names[TABS] = { "DISPLAY", "EFFECTS", "GAME", "MUSIC" };
static const Row tab_rows[TABS][9] = {
    { { ROW_OPTION, "Widescreen", OPT_WIDESCREEN, "Shows more of the world at the sides, streamed by the game's own map code." },
      { ROW_OPTION, "Aspect ratio", OPT_ASPECT, "The shape of the widescreen picture." },
      { ROW_OPTION, "Integer scaling", OPT_INTEGER, "Whole-number scaling only: every pixel the same size." },
      { ROW_OPTION, "Window", OPT_WINDOW_ALL, "Fullscreen, or a window of this many times the game's size." },
      { ROW_OPTION, "Filter", OPT_FILTER, "Scanlines, a curved CRT tube or an LCD grid over the picture." },
      { ROW_OPTION, "Sharpening", OPT_SHARPEN, "Crisper edges after scaling, without halos (contrast-adaptive)." },
      { ROW_OPTION, "Internal resolution", OPT_INTERNAL, "Redraws each layer at a higher resolution; 6x (4K) needs a fast CPU." },
      { ROW_OPTION, "Scaler", OPT_SCALER, "Edge rounds corners; Smooth follows curves and slopes (xBR)." },
      { ROW_OPTION, "Texture pack", OPT_TEXPACK, "High-resolution tile replacements from a folder in texture_packs." } },
    { { ROW_OPTION, "Lighting", OPT_LIGHTING, "Fire, magic and bright light glow into their surroundings." },
      { ROW_OPTION, "Spirit effects", OPT_ELEMENTS, "The spirits and their magic give off embers, drops, wisps or leaves." },
      { ROW_OPTION, "Item animations", OPT_ITEMS, "Dropped items float on a soft glow and glint, easy to spot." },
      { ROW_OPTION, "Water", OPT_WATER, "Water swells, foams at the shore, glints and reflects what stands above it." },
      { ROW_OPTION, "Footsteps", OPT_FOOTSTEPS, "Ali kicks up dust, grass or splashes as he walks." },
      { ROW_OPTION, "Dynamic lights", OPT_DYNLIGHT, "Spirits, magic and torches light the scene; characters cast soft shadows." },
      { ROW_OPTION, "HD-2D look", OPT_HD2D, "Tilt-shift focus, drifting sunbeams, warm grading and floating motes." } },
    { { ROW_ROM, "ROM", 0, "Your Beyond Oasis (USA) cartridge file." },
      { ROW_OPTION, "Cheats", OPT_CHEATS, "Infinite health, spirit power and unbreakable weapons." },
      { ROW_OPTION, "Smooth motion", OPT_SMOOTH, "Moves everything smoothly between the game's 30 Hz updates." },
      { ROW_OPTION, "Sprite flicker fix", OPT_FLICKER, "Lifts the hardware's sprites-per-line limit, so nothing flickers." },
      { ROW_OPTION, "Slowdown fix", OPT_OVERCLOCK, "A faster 68000, so busy scenes no longer slow down." },
      { ROW_OPTION, "Rewind", OPT_REWIND_ALL, "Hold the rewind key to step back in time." },
      { ROW_CONTROLS, "Controls", 0, "Keyboard keys and controller buttons for every action." },
      { ROW_OUTFIT, "Ali's outfit", 0, "Colours and animations for his clothes, hair, skin and jewellery." },
      { ROW_OPTION, "Show launcher at startup", OPT_LAUNCHER, "Off starts the game directly (show_launcher=1 in settings.ini brings it back)." } },
    { { ROW_OPTION, "Launcher music", OPT_MUSIC, NULL }, { ROW_TRACK, "Track", 0, NULL },
      { ROW_OPTION, "Player", OPT_OST_MODE, NULL }, { ROW_OPTION, "Volume", OPT_VOLUME, NULL },
      { ROW_OPTION, "Audio", OPT_AUDIO, NULL } },
};
static const int tab_count[TABS] = { 9, 7, 9, 5 };

/* ---- Layout (1280x720 units) ------------------------------------------------------- */
#define PANEL_X (TABLET_X + 40)
#define PANEL_R (TABLET_X + TABLET_W - 44)
#define TAB_Y   (TABLET_Y + 26)
#define TAB_H   70
#define TAB_X0  (TABLET_X + 26)
#define TAB_W   ((TABLET_W - 52) / TABS)
#define ROWS_Y  (TAB_Y + TAB_H + 26)
#define ROW_H   40
#define CARD_Y  (ROWS_Y + 5 * ROW_H + 14)
#define CARD_H  160
#define BTN_Y   (TABLET_Y + TABLET_H - 118)
#define BTN_H   54
#define PLAY_X  PANEL_X
#define PLAY_W  330
#define QUIT_X  (PANEL_R - 150)
#define QUIT_W  150
#define VOLUME_W 150
#define VOLUME_X (PANEL_R - 22 - 52 - 26 - VOLUME_W)
#define PLAYER_BTN_Y (CARD_Y + 112)

/* Selection: the tab bar, a row of the open tab, or a button. */
enum { ITEM_TABS = -1, ITEM_PLAY = 100, ITEM_QUIT = 101 };
enum { HIT_PREV = 200, HIT_TOGGLE, HIT_NEXT, HIT_TAB0 = 300 };

static struct {
    int tab, selected;
    float focus;                        /* the spirit circling to the front, follows tab */
    float tab_t0;                       /* when the tab last changed, and from where */
    int tab_from;
    float hl_y, hl_x;                   /* the highlight, gliding to the selected item */
    float changed_t0;                   /* when a value last changed (it pulses) */
    float selected_t0;                  /* when the selection last moved (the preview fades in) */
    int changed_item;
    float flash;                        /* the spirits' flare on a choice, fading */
    float t;
} ui;

static bool play_enabled(void) { return rom_ok; }
static char rom_status[96];

static float row_y(int r) { return ROWS_Y + r * ROW_H; }

static int hit_test(float mx, float my) {
    for (int k = 0; k < TABS; k++)
        if (mx >= TAB_X0 + k * TAB_W && mx < TAB_X0 + (k + 1) * TAB_W && my >= TAB_Y && my < TAB_Y + TAB_H) return HIT_TAB0 + k;
    for (int r = 0; r < tab_count[ui.tab]; r++) {
        float y = row_y(r);
        if (mx >= PANEL_X - 14 && mx <= PANEL_R + 8 && my >= y && my < y + ROW_H) return r;
    }
    if (ui.tab == 3 && my >= PLAYER_BTN_Y - 20 && my <= PLAYER_BTN_Y + 20) {
        float cx = (PANEL_X + PANEL_R) / 2.f;
        if (fabsf(mx - (cx - 70)) < 24) return HIT_PREV;
        if (fabsf(mx - cx) < 28) return HIT_TOGGLE;
        if (fabsf(mx - (cx + 70)) < 24) return HIT_NEXT;
    }
    if (my >= BTN_Y && my <= BTN_Y + BTN_H) {
        if (mx >= PLAY_X && mx <= PLAY_X + PLAY_W) return ITEM_PLAY;
        if (mx >= QUIT_X && mx <= QUIT_X + QUIT_W) return ITEM_QUIT;
    }
    return -2;
}

static void select_item(int item) {
    if (item == ui.selected) return;
    ui.selected = item;
    ui.selected_t0 = ui.t;
    music_sfx(SFX_CURSOR);
}

static void move_selection(int dir) {
    int order[16], n = 0, at = 0;
    order[n++] = ITEM_TABS;
    for (int r = 0; r < tab_count[ui.tab]; r++) order[n++] = r;
    order[n++] = ITEM_PLAY; order[n++] = ITEM_QUIT;
    for (int i = 0; i < n; i++) if (order[i] == ui.selected) at = i;
    select_item(order[settings_cycle(at, n, dir)]);
}

static void open_tab(int tab) {
    tab = (tab % TABS + TABS) % TABS;
    if (tab == ui.tab) return;
    ui.tab_from = ui.tab;
    ui.tab = tab;
    ui.tab_t0 = ui.t;
    ui.selected_t0 = ui.t;
    if (ui.selected >= 0 && ui.selected < 100 && ui.selected >= tab_count[tab]) ui.selected = tab_count[tab] - 1;
    ui.flash = 1;
    music_sfx(SFX_CURSOR);
}

/* What the player is doing (a copy, taken under its lock). */
typedef struct { bool ready, on; int song, frames; float meter[METER_BARS]; } PlayerView;

static PlayerView player_view(void) {
    PlayerView v = { 0 };
    if (!music_lock || !music_thread) return v;
    SDL_LockMutex(music_lock);
    v.ready = player.ready; v.on = player.on; v.song = player.song; v.frames = player.frames;
    memcpy(v.meter, player.meter, sizeof v.meter);
    SDL_UnlockMutex(music_lock);
    return v;
}

static void song_name(int song, char *out, size_t n) {
    if (song == title_song) snprintf(out, n, "Title Theme");
    else snprintf(out, n, "Track %02d", song + 1);
}

/* ---- Drawing ----------------------------------------------------------------------- */
static float ease_out(float x) { x = clampf(x, 0, 1); return 1 - (1 - x) * (1 - x) * (1 - x); }

/* A filled triangle (layout units), antialiased by 2x2 supersampling. */
static void fill_tri(float ax, float ay, float bx, float by, float cx, float cy, Rgb c, float a) {
    float x0 = fminf(ax, fminf(bx, cx)), x1 = fmaxf(ax, fmaxf(bx, cx)), y0 = fminf(ay, fminf(by, cy)), y1 = fmaxf(ay, fmaxf(by, cy));
    int px0 = (int)ui_x(x0) - 1, px1 = (int)ui_x(x1) + 1, py0 = (int)ui_y(y0) - 1, py1 = (int)ui_y(y1) + 1;
    for (int y = py0; y <= py1; y++)
        for (int x = px0; x <= px1; x++) {
            int in = 0;
            for (int s = 0; s < 4; s++) {
                float ux = (x + .25f + (s & 1) * .5f - ui_ox) / ui_s, uy = (y + .25f + (s >> 1) * .5f - ui_oy) / ui_s;
                float e0 = (bx - ax) * (uy - ay) - (by - ay) * (ux - ax);
                float e1 = (cx - bx) * (uy - by) - (cy - by) * (ux - bx);
                float e2 = (ax - cx) * (uy - cy) - (ay - cy) * (ux - cx);
                in += (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
            }
            if (in) ui_blend_px(x, y, c, a * in / 4.f);
        }
}

static void draw_tabs(void) {
    for (int k = 0; k < TABS; k++) {
        float x = TAB_X0 + k * TAB_W, cx = x + TAB_W / 2.f;
        bool open = ui.tab == k, focus = ui.selected == ITEM_TABS && open;
        const Spirit *sp = &spirits[k];
        float bright = open ? 1 : .45f;
        if (open) {
            ui_fill_rect(x + 6, TAB_Y, TAB_W - 12, TAB_H, sp->light, focus ? .10f : .06f);
            /* A glowing line under the open tab. */
            for (int i = 0; i < 3; i++) ui_fill_rect(x + 14 - i * 3, TAB_Y + TAB_H - 3 - i, TAB_W - 28 + i * 6, 2, sp->light, .5f - i * .15f);
        }
        scene_orb(k, cx, TAB_Y + 24, open ? 15 : 12, ui.t, bright + (open ? ui.flash * .4f : 0));
        Rgb tc = open ? mix(rgb_hex(0xFFF4DA), sp->light, .25f) : rgb_hex(0x8A8170);
        float tw = ui_text_width(tab_names[k], "Segoe UI Semibold", 12.5f, 600, 3);
        ui_draw_text(tab_names[k], "Segoe UI Semibold", 12.5f, 600, 3, cx - tw / 2, TAB_Y + 46, tc, tc, 1);
    }
}

static void draw_row(int r, const Row *row, float dx, float alpha, const PlayerView *pv) {
    float y = row_y(r);
    bool focus = ui.selected == r;
    Rgb lc = focus ? rgb_hex(0xFFFFFF) : rgb_hex(0xD8CDB6);
    ui_draw_text(row->label, "Segoe UI", 17.5f, 400, 0, PANEL_X + 4 + dx, y + 10, lc, lc, alpha);
    char value[96];
    Rgb vc = focus ? rgb_hex(0xFFE4A0) : rgb_hex(0xEBD9AE);
    const char *hint = NULL;
    switch (row->kind) {
    case ROW_ROM:
        snprintf(value, sizeof value, "%s", rom_status);
        vc = rom_ok ? rgb_hex(0x9FDCA6) : rgb_hex(0xF09A86);
        hint = "Enter to choose\xE2\x80\xA6";
        break;
    case ROW_CONTROLS:
        snprintf(value, sizeof value, "Keyboard & controller");
        hint = "Enter to change\xE2\x80\xA6";
        break;
    case ROW_OUTFIT: {
        int changed = 0;
        for (int k = 0; k < OUTFIT_PARTS; k++) changed += settings.outfit_color[k] >= 0;
        if (!settings.outfit || !changed) snprintf(value, sizeof value, "His own colours");
        else snprintf(value, sizeof value, "%d part%s recoloured", changed, changed == 1 ? "" : "s");
        hint = "Enter to change\xE2\x80\xA6";
        break;
    }
    case ROW_TRACK:
        song_name(pv->ready ? pv->song : (settings.ost_track >= 0 ? settings.ost_track : title_song), value, sizeof value);
        hint = pv->on ? "Enter to stop" : "Enter to play";
        break;
    default:
        settings_option_text(row->id, value, sizeof value);
    }
    float pulse = ui.changed_item == r ? 1 - ease_out((ui.t - ui.changed_t0) / .35f) : 0;
    if (pulse > 0) vc = mix(vc, rgb(1, 1, 1), pulse * .8f);
    float px = 17.5f + pulse * 1.5f;
    float vw = ui_text_width(value, "Segoe UI Semibold", px, 600, 0);
    float vx = PANEL_R - vw;
    bool arrows = row->kind == ROW_OPTION || row->kind == ROW_TRACK;
    if (row->kind == ROW_OPTION && row->id == OPT_VOLUME) {
        vw = ui_text_width("100%", "Segoe UI Semibold", px, 600, 0);
        vx = PANEL_R - 22 - vw;
        ui_slider(VOLUME_X + dx, y + ROW_H / 2.f, VOLUME_W, settings.volume / 100.f, focus);
    } else if (arrows) {
        vx -= 22;
    }
    if (focus && hint) {
        float hw = ui_text_width(hint, "Segoe UI", 13, 400, 0);
        ui_draw_text(hint, "Segoe UI", 13, 400, 0, vx - hw - (arrows ? 34 : 16) + dx, y + 13, rgb_hex(0x8F8673), rgb_hex(0x8F8673), alpha);
    }
    if (arrows) {
        Rgb ac = focus ? spirits[ui.tab].light : rgb_hex(0x6A5F4C);
        float nudge = pulse * 3;
        ui_draw_text("\xE2\x80\xB9", "Segoe UI Semibold", 20, 600, 0, vx - 18 - nudge + dx, y + 7, ac, ac, alpha);
        ui_draw_text("\xE2\x80\xBA", "Segoe UI Semibold", 20, 600, 0, PANEL_R - 12 + nudge + dx, y + 7, ac, ac, alpha);
    }
    ui_draw_text(value, "Segoe UI Semibold", px, 600, 0, vx + dx, y + 10 - pulse * .8f, vc, vc, alpha);
}

/* The soundtrack player: the song, its time, a level meter and the transport buttons. */
static void draw_player(const PlayerView *pv, float dx, float alpha) {
    float x = PANEL_X - 6 + dx, w = PANEL_R - PANEL_X + 12, y = CARD_Y;
    const Spirit *sp = &spirits[3];
    ui_panel(x, y, w, CARD_H, 12, rgb_hex(0x1B1426), rgb_hex(0x100C16), .9f * alpha, sp->light, .28f * alpha);
    char name[64], time[48];
    int song = pv->ready ? pv->song : (settings.ost_track >= 0 ? settings.ost_track : title_song);
    song_name(song, name, sizeof name);
    ui_draw_text(pv->on ? "NOW PLAYING" : "STOPPED", "Segoe UI Semibold", 11, 600, 3, x + 20, y + 16, sp->light, sp->light, alpha * .9f);
    ui_draw_text(name, "Palatino Linotype", 26, 700, 1, x + 20, y + 32, rgb_hex(0xFFF1D0), rgb_hex(0xD9B567), alpha);
    char count[32];
    snprintf(count, sizeof count, "%d / %d", (song < 0 ? 0 : song) + 1, OST_SONGS);
    float cw = ui_text_width(count, "Segoe UI", 14, 400, 0);
    ui_draw_text(count, "Segoe UI", 14, 400, 0, x + w - 20 - cw, y + 20, rgb_hex(0x9A8FB0), rgb_hex(0x9A8FB0), alpha);
    /* Level meter, right of the name. */
    float mx0 = x + w - 20 - METER_BARS * 7, my = y + 72;
    for (int b = 0; b < METER_BARS; b++) {
        float h = 3 + pv->meter[b] * 26;
        Rgb c = mix(sp->light, rgb_hex(0xFFE4A0), pv->meter[b]);
        ui_fill_rect(mx0 + b * 7, my - h, 4, h, c, alpha * (.35f + .65f * pv->meter[b]));
    }
    /* Progress through the song's turn, and the time. */
    int secs = pv->frames / 60;
    snprintf(time, sizeof time, "%d:%02d", secs / 60, secs % 60);
    float bar_x = x + 20, bar_w = w - 40, bar_y = y + 86;
    float prog = clampf(pv->frames / (PLAY_FOR * 60.f), 0, 1);
    ui_fill_rect(bar_x, bar_y, bar_w, 3, rgb_hex(0x3A2F4A), alpha);
    ui_fill_rect(bar_x, bar_y, bar_w * prog, 3, sp->light, alpha);
    ui_draw_text(time, "Segoe UI", 12, 400, 0, bar_x, bar_y + 8, rgb_hex(0x9A8FB0), rgb_hex(0x9A8FB0), alpha);
    /* Previous, play/stop, next. */
    float cx = (PANEL_X + PANEL_R) / 2.f + dx, by = PLAYER_BTN_Y;
    Rgb bc = rgb_hex(0xEADFC8);
    fill_tri(cx - 62, by, cx - 50, by - 9, cx - 50, by + 9, bc, alpha * .85f);
    fill_tri(cx - 74, by, cx - 62, by - 9, cx - 62, by + 9, bc, alpha * .85f);
    fill_tri(cx + 62, by, cx + 50, by - 9, cx + 50, by + 9, bc, alpha * .85f);
    fill_tri(cx + 74, by, cx + 62, by - 9, cx + 62, by + 9, bc, alpha * .85f);
    scene_glow(cx, by, 30, sp->light, .25f * alpha);
    if (pv->on) {
        ui_fill_rect(cx - 9, by - 10, 18, 20, rgb_hex(0xFFF1D0), alpha);
    } else {
        fill_tri(cx - 8, by - 12, cx + 13, by, cx - 8, by + 12, rgb_hex(0xFFF1D0), alpha);
    }
}

static void draw_ui(float t) {
    PlayerView pv = player_view();
    draw_tabs();
    /* The open tab slides in from the side it came from. */
    float k = ease_out((t - ui.tab_t0) / .32f);
    int dir = ui.tab > ui.tab_from ? 1 : -1;
    float dx = (1 - k) * 36 * dir, alpha = k;
    /* The highlight glides to the selected row. */
    bool on_row = ui.selected >= 0 && ui.selected < tab_count[ui.tab];
    if (on_row) {
        float y = ui.hl_y;
        Rgb c = spirits[ui.tab].light;
        ui_fill_rect(PANEL_X - 14, y + 2, PANEL_R - PANEL_X + 22, ROW_H - 4, c, .07f);
        ui_fill_rect(PANEL_X - 14, y + 2, 3, ROW_H - 4, c, .95f);
        scene_glow(PANEL_X - 12, y + ROW_H / 2.f, 16, c, .35f);
    }
    for (int r = 0; r < tab_count[ui.tab]; r++) draw_row(r, &tab_rows[ui.tab][r], dx, alpha, &pv);
    if (ui.tab == 3) draw_player(&pv, dx, alpha);
    else if (on_row && tab_rows[ui.tab][ui.selected].about) {
        /* What the selected option does, under a thin rule in the tab's colour. */
        float y = row_y(tab_count[ui.tab]) + 22;
        ui_fill_rect(PANEL_X, y, 46, 2, spirits[ui.tab].light, .7f * alpha);
        ui_draw_text(tab_rows[ui.tab][ui.selected].about, "Segoe UI", 14.5f, 400, 0, PANEL_X + dx, y + 14,
                     rgb_hex(0xB8AC92), rgb_hex(0xB8AC92), alpha);
    }

    float lx = .5f * sinf(t * .45f), ly = .3f * cosf(t * .33f);
    ui_button(PLAY_X, BTN_Y, PLAY_W, BTN_H, "PLAY", 20, true, ui.selected == ITEM_PLAY, play_enabled(), lx, ly, t);
    ui_button(QUIT_X, BTN_Y, QUIT_W, BTN_H, "QUIT", 20, false, ui.selected == ITEM_QUIT, true, lx, ly, t);
    const char *keys = "\xE2\x86\x91\xE2\x86\x93 select    \xE2\x86\x90\xE2\x86\x92 change    Q E  tabs    [ ]  track    M  music    Esc  quit";
    ui_draw_text(keys, "Segoe UI", 12, 400, 1, PANEL_X, TABLET_Y + TABLET_H - 44, rgb_hex(0x7C7262), rgb_hex(0x7C7262), 1);

    /* A picture option: its before/after on the last frame the game saved, over the scene. */
    if (on_row && tab_rows[ui.tab][ui.selected].kind == ROW_OPTION && preview_supported(tab_rows[ui.tab][ui.selected].id) &&
        preview_ready()) {
        float a = ease_out((t - ui.selected_t0) / .3f);
        float px = 40, py = 226, pw = 556, ph = pw * 9 / 16.f;
        ui_fill_rect(0, 0, TABLET_X - 20, UI_BASE_H, rgb(0, 0, 0), .45f * a);
        ui_panel(px - 14, py - 46, pw + 28, ph + 66, 14, rgb_hex(0x1C1712), rgb_hex(0x0E0B09), .92f * a, rgb_hex(0xD9B567), .45f * a);
        ui_draw_text("PREVIEW  \xC2\xB7  YOUR LAST GAME FRAME", "Segoe UI Semibold", 12, 600, 3, px, py - 30,
                     spirits[ui.tab].light, spirits[ui.tab].light, a);
        if (a > .05f) preview_draw(tab_rows[ui.tab][ui.selected].id, px, py, pw, ph, t);
        ui_panel(px - 1, py - 1, pw + 2, ph + 2, 4, rgb(0, 0, 0), rgb(0, 0, 0), 0, rgb_hex(0xD9B567), .5f * a);
    }

    /* Under the armlet, on every tab: what is playing. */
    if (pv.ready && pv.on) {
        char name[64], line[96];
        song_name(pv.song, name, sizeof name);
        snprintf(line, sizeof line, "\xE2\x99\xAA  %s", name);
        float w = ui_text_width(line, "Segoe UI Semibold", 14, 600, 1);
        ui_draw_text(line, "Segoe UI Semibold", 14, 600, 1, 318 - w / 2, 662, rgb_hex(0xF6DFA6), rgb_hex(0xC99B4E), .85f);
    }
}

/* ---- Frame -------------------------------------------------------------------- */
static void layout(int pixel_w) {
    ui_begin(pixel_w, (int)(BASE_H * (pixel_w / (float)BASE_W) + .5f));
    scene_layout(&armlet, &logo);
}

/* Advances the animations by dt seconds. */
static void animate(float dt) {
    float k = 1 - expf(-dt * 10);
    float target = (float)ui.tab;
    float d = remainderf(target - ui.focus, TABS);
    ui.focus += d * (1 - expf(-dt * 6));
    float hy = ui.selected >= 0 && ui.selected < tab_count[ui.tab] ? row_y(ui.selected) : ui.hl_y;
    ui.hl_y += (hy - ui.hl_y) * k;
    ui.flash *= expf(-dt * 3);
}

static void render(float t) {
    ui.t = t;
    scene_draw(t, ui.focus, .6f + .4f * ui.flash);
    draw_ui(t);
    if (!logo.w) {
        Rgb gold_top = rgb_hex(0xFBEBB8), gold_bot = rgb_hex(0xC08A2C);
        float w = ui_text_width("BEYOND OASIS", "Palatino Linotype", 54, 700, 7);
        ui_draw_text("BEYOND OASIS", "Palatino Linotype", 54, 700, 7, 318 - w / 2, 60, gold_top, gold_bot, 1);
    }
}

static void update_rom_status(void) {
    if (rom_ok) snprintf(rom_status, sizeof rom_status, "Verified \xE2\x9C\x93");
    else if (!settings.rom_path[0]) snprintf(rom_status, sizeof rom_status, "Not selected");
    else snprintf(rom_status, sizeof rom_status, "Not the USA cartridge");
}

static void init_common(void) {
    char *bp = SDL_GetBasePath();
    snprintf(cache_path, sizeof cache_path, "%slauncher_art.cache", bp ? bp : "");
    settings_set_base(bp ? bp : "");
    art_prepare();                      /* (may run the intro, which draws over hd_frame) */
    preview_load(bp ? bp : "");
    if (bp) SDL_free(bp);
    update_rom_status();
}

static void art_write_png(const Art *a, const char *base, const char *suffix) {
    if (!a->w) return;
    char p[1200];
    snprintf(p, sizeof p, "%s%s", base, suffix);
    png_write(p, a->rgb, a->w, a->h, a->w);
}

int launcher_screenshot(const char *png, int width, int tab, float t) {
    init_common();
    art_write_png(&armlet, png, ".armlet.png");
    art_write_png(&logo, png, ".logo.png");
    layout(width);
    ui.tab = ui.tab_from = (tab % TABS + TABS) % TABS;
    ui.focus = (float)ui.tab;
    ui.selected = tab_count[ui.tab] > 1 ? 1 : 0;
    ui.hl_y = row_y(ui.selected);
    ui.tab_t0 = -10;
    Uint64 t0 = SDL_GetPerformanceCounter();
    for (int i = 0; i < 10; i++) render(t);
    printf("launcher frame: %.1f ms at %dx%d\n", (SDL_GetPerformanceCounter() - t0) * 100.0 / SDL_GetPerformanceFrequency(), ui_w, ui_h);
    return png_write(png, ui_px, ui_w, ui_h, ui_w);
}

/* ---- ROM picker ------------------------------------------------------------------ */
static void choose_rom(SDL_Window *win) {
#ifdef _WIN32
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof ofn;
    (void)win;
    ofn.lpstrFilter = L"Genesis / Mega Drive ROM (*.md;*.bin;*.gen;*.smd)\0*.md;*.bin;*.gen;*.smd\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"Choose your Beyond Oasis (USA) ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    char path[1024];
    WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL);
    for (char *p = path; *p; p++) if (*p == '\\') *p = '/';
    snprintf(settings.rom_path, sizeof settings.rom_path, "%s", path);
    remove(cache_path);
    music_stop();
    art_prepare();
    update_rom_status();
    scene_layout(&armlet, &logo);
    music_start();
#else
    (void)win;
#endif
}

/* ---- Interactive loop --------------------------------------------------------------- */
/* Changes the selected row's value (dir -1/+1), or activates it (dir 0). */
static void row_action(SDL_Window *win, int dir) {
    const Row *row = &tab_rows[ui.tab][ui.selected];
    switch (row->kind) {
    case ROW_ROM: if (!dir) { music_sfx(SFX_CONFIRM); choose_rom(win); } return;
    case ROW_CONTROLS: if (!dir) { music_sfx(SFX_CONFIRM); controls_open(); } return;
    case ROW_OUTFIT:
        if (!dir) {
            char *bp = SDL_GetBasePath();
            outfit_preview_load(bp ? bp : "");
            if (bp) SDL_free(bp);
            music_sfx(SFX_CONFIRM);
            outfit_open();
        }
        return;
    case ROW_TRACK: {
        PlayerView pv = player_view();
        if (dir) music_request(-1, dir, false, false);
        else music_request(-1, 0, !pv.on, pv.on);
        if (dir || !pv.on) settings.launcher_music = true;
        break;
    }
    default:
        if (row->id == OPT_VOLUME && !dir) return;
        settings_option_change(row->id, dir ? dir : 1);
        if (row->id == OPT_MUSIC) music_request(-1, 0, settings.launcher_music, !settings.launcher_music);
    }
    ui.changed_item = ui.selected;
    ui.changed_t0 = ui.t;
    ui.flash = 1;
    music_sfx(SFX_CONFIRM);
}

static void launcher_outfit_sound(int kind) { music_sfx(kind == 0 ? SFX_CURSOR : kind == 1 ? SFX_CONFIRM : SFX_CANCEL); }

int launcher_run(const char *ini_path) {
    init_common();
    SDL_Window *win = SDL_CreateWindow("Beyond Oasis", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, BASE_W, BASE_H,
                                       SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) return 1;
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    int ow, oh;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    layout(ow > 1920 ? 1920 : ow);      /* drawn in software: past 1080p the GPU scales it up */
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, ui_w, ui_h);
    SDL_GameController *pad = NULL;
    memset(&ui, 0, sizeof ui);
    outfit_sound = launcher_outfit_sound;
    ui.tab_t0 = -10;
    ui.changed_item = -1;
    ui.selected = ITEM_PLAY;
    if (!rom_ok) { ui.tab = ui.tab_from = 2; ui.focus = 2; ui.selected = 0; }   /* the ROM row */
    ui.hl_y = row_y(ui.selected >= 0 && ui.selected < 100 ? ui.selected : 0);
    music_start();

    Uint64 freq = SDL_GetPerformanceFrequency(), start = SDL_GetPerformanceCounter(), last = start;
    int result = -1;
    while (result < 0) {
        Uint64 now = SDL_GetPerformanceCounter();
        float t = (float)(now - start) / freq, dt = (float)(now - last) / freq;
        last = now;
        ui.t = t;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int activate = 0, change = 0;
            if ((controls_is_open() || outfit_is_open()) && e.type != SDL_QUIT) {
                /* A page takes all input while it is up (mouse points match the layout). */
                if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
                float mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.type == SDL_MOUSEBUTTONDOWN ? e.button.x : 0;
                float my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.type == SDL_MOUSEBUTTONDOWN ? e.button.y : 0;
                if (outfit_is_open()) { outfit_event(&e, mx, my); continue; }
                controls_event(&e, mx, my);
                if (!controls_is_open()) music_sfx(SFX_CANCEL);
                continue;
            }
            switch (e.type) {
            case SDL_QUIT: result = 0; break;
            case SDL_CONTROLLERDEVICEADDED: if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which); break;
            case SDL_MOUSEMOTION: {
                /* Mouse positions are in window points, which match the 1280x720 layout. */
                int h = hit_test(e.motion.x, e.motion.y);
                if (h >= HIT_TAB0) { if (ui.tab == h - HIT_TAB0) select_item(ITEM_TABS); }
                else if (h >= -1 && h < 200) select_item(h);
                break;
            }
            case SDL_MOUSEBUTTONDOWN: {
                int h = hit_test(e.button.x, e.button.y);
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (h >= HIT_TAB0) { open_tab(h - HIT_TAB0); ui.selected = ITEM_TABS; }
                    else if (h == HIT_PREV || h == HIT_NEXT) { music_request(-1, h == HIT_NEXT ? 1 : -1, false, false); settings.launcher_music = true; music_sfx(SFX_CONFIRM); }
                    else if (h == HIT_TOGGLE) { PlayerView pv = player_view(); music_request(-1, 0, !pv.on, pv.on); settings.launcher_music = !pv.on; music_sfx(SFX_CONFIRM); }
                    else if (h >= 0 && h < 100 && tab_rows[ui.tab][h].kind == ROW_OPTION && tab_rows[ui.tab][h].id == OPT_VOLUME &&
                             e.button.x >= VOLUME_X - 8 && e.button.x <= VOLUME_X + VOLUME_W + 8) {
                        ui.selected = h;        /* click on the track: set the volume there */
                        settings_set_volume_fraction(ui_slider_value(VOLUME_X, VOLUME_W, (float)e.button.x));
                        music_sfx(SFX_CURSOR);
                    } else if (h >= 0 || h == ITEM_PLAY || h == ITEM_QUIT) { ui.selected = h; activate = 1; }
                } else if (e.button.button == SDL_BUTTON_RIGHT && h >= 0 && h < 100) {
                    ui.selected = h; change = -1;
                }
                break;
            }
            case SDL_KEYDOWN:
                switch (e.key.keysym.sym) {
                case SDLK_UP: case SDLK_w: move_selection(-1); break;
                case SDLK_DOWN: case SDLK_s: case SDLK_TAB: move_selection(1); break;
                case SDLK_LEFT: case SDLK_a: change = -1; break;
                case SDLK_RIGHT: case SDLK_d: change = 1; break;
                case SDLK_q: case SDLK_PAGEUP: open_tab(ui.tab - 1); break;
                case SDLK_e: case SDLK_PAGEDOWN: open_tab(ui.tab + 1); break;
                case SDLK_LEFTBRACKET: music_request(-1, -1, false, false); settings.launcher_music = true; break;
                case SDLK_RIGHTBRACKET: music_request(-1, 1, false, false); settings.launcher_music = true; break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: activate = 1; break;
                case SDLK_ESCAPE: result = 0; break;
                case SDLK_m:            /* music on/off */
                    settings.launcher_music = !settings.launcher_music;
                    music_request(-1, 0, settings.launcher_music, !settings.launcher_music);
                    break;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: move_selection(-1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move_selection(1); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: change = -1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: change = 1; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: open_tab(ui.tab - 1); break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: open_tab(ui.tab + 1); break;
                case SDL_CONTROLLER_BUTTON_A: activate = 1; break;
                case SDL_CONTROLLER_BUTTON_START: if (play_enabled()) result = 1; else music_sfx(SFX_ERROR); break;
                case SDL_CONTROLLER_BUTTON_B: result = 0; break;
                }
                break;
            }
            if (ui.selected == ITEM_TABS && change) open_tab(ui.tab + change);
            else if (ui.selected >= 0 && ui.selected < tab_count[ui.tab] && (change || activate)) row_action(win, change);
            else if (activate && ui.selected == ITEM_PLAY) {
                if (play_enabled()) { music_sfx(SFX_CONFIRM); result = 1; }
                else music_sfx(SFX_ERROR);
            }
            else if (activate && ui.selected == ITEM_QUIT) result = 0;
        }

        animate(dt);
        render(t);
        controls_render(t);
        outfit_render(t);
        SDL_UpdateTexture(tex, NULL, ui_px, ui_w * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    if (music_thread) {
        PlayerView pv = player_view();
        if (pv.ready) settings.ost_track = pv.song;
    }
    music_stop();
    settings_save(ini_path);
    if (pad) SDL_GameControllerClose(pad);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return result;
}
