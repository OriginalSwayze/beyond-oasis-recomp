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
    vdp_opts.hd_capture = settings.internal_scale > 1;
}

/* ---- Input -------------------------------------------------------------------- */
static uint16_t key_bits(const Uint8 *k) {
    uint16_t p = 0;
    if (k[SDL_SCANCODE_UP]) p |= PAD_UP;
    if (k[SDL_SCANCODE_DOWN]) p |= PAD_DOWN;
    if (k[SDL_SCANCODE_LEFT]) p |= PAD_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) p |= PAD_RIGHT;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_A]) p |= PAD_A;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_S]) p |= PAD_B;
    if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_D]) p |= PAD_C;
    if (k[SDL_SCANCODE_RETURN]) p |= PAD_START;
    return p;
}

static uint16_t controller_bits(SDL_GameController *c) {
    if (!c) return 0;
    uint16_t p = 0;
    const int dead = 12000;
    int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -dead) p |= PAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > dead) p |= PAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -dead) p |= PAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > dead) p |= PAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) p |= PAD_A;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) p |= PAD_B;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) p |= PAD_C;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) p |= PAD_START;
    return p;
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

int main(int argc, char **argv) {
    char *bp = SDL_GetBasePath();
    if (bp) { snprintf(base_dir, sizeof base_dir, "%s", bp); SDL_free(bp); }
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
    int save_to = 0;
    int load_from = 0;
    const char *hd_shot = NULL;
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
        else if (!strcmp(argv[i], "--widescreen") && i + 1 < argc) settings.widescreen = atoi(argv[++i]) != 0;
        else if (!strcmp(argv[i], "--no-launcher")) no_launcher = true;
        else if (!strcmp(argv[i], "--hd-shot") && i + 2 < argc) {
            /* --hd-shot F.png SCALE: save the last frame at the internal resolution SCALE (2-4). */
            hd_shot = argv[++i];
            settings.internal_scale = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--save-to") && i + 1 < argc) save_to = atoi(argv[++i]);   /* save a slot at the end */
        else if (!strcmp(argv[i], "--states-base") && i + 1 < argc) savestate_init(argv[++i]);  /* e.g. "test_" -> test_states/ */
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc) settings.state_slot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load_from = atoi(argv[++i]);   /* load a slot first */
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) cheats_add_codes(argv[++i]);   /* FFxxxx:VV[VV],... */
        else if (!strcmp(argv[i], "--cheat") && i + 1 < argc) {
            /* --cheat health|spirit|weapons: turn a built-in cheat on for this run. */
            const char *c = argv[++i];
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
        else if (!strcmp(argv[i], "--launcher-shot") && i + 4 < argc) {
            /* --launcher-shot F.png WIDTH LX LY: render the launcher (light direction LX, LY). */
            launcher_shot = argv[++i];
            shot_width = atoi(argv[++i]);
            shot_lx = (float)atof(argv[++i]);
            shot_ly = (float)atof(argv[++i]);
        }
        else snprintf(settings.rom_path, sizeof settings.rom_path, "%s", argv[i]);
    }

    if (launcher_shot) {
        apply_settings();
        return launcher_screenshot(launcher_shot, shot_width, shot_lx, shot_ly, 1.0f) ? 1 : 0;
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
    if (!headless_frames) settings_save(ini); /* remember a ROM path given on the command line */
    load_sram();
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
        if (hd_shot) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            hd_compose(settings.internal_scale);
            double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency();
            printf("hd frame %dx%d in %.2f ms\n", hd_w, hd_h, ms);
            png_write(hd_shot, hd_px, hd_w, hd_h, hd_w);
        }
        if (save_to && !savestate_save(save_to, framebuffer, frame_width(), vdp_screen_height(), FB_W))
            fprintf(stderr, "could not save slot %d\n", save_to);
        if (menu_shot) {
            SDL_Rect r = display_rect(menu_w, menu_h);
            menu_open(framebuffer, frame_width(), vdp_screen_height(), FB_W, r.x, r.y, r.w, r.h, menu_w, menu_h, 1);
            menu_render(1.0);
            png_write(menu_shot, ui_px, ui_w, ui_h, ui_w);
        }
        save_coverage();
        save_sram();
        printf("ran %d frames, screen %dx%d\n", headless_frames, frame_width(), vdp_screen_height());
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
    SDL_Texture *hd_tex = NULL;         /* the frame at the internal resolution */
    int hd_tex_w = 0, hd_tex_h = 0;
    SDL_GameController *pad = NULL;
    SDL_AudioSpec want = { .freq = 48000, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024 }, have;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) { sound_out_rate = have.freq; SDL_PauseAudioDevice(audio, 0); }

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
            bool open_menu = (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !e.key.repeat) ||
                             (e.type == SDL_CONTROLLERBUTTONDOWN && (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK ||
                                                                     e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE));
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

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        bool rewinding = settings.rewind && (keys[SDL_SCANCODE_BACKSPACE] ||
                         (pad && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)));
        uint16_t old_pad;
        if (rewinding && rewind_step(&old_pad)) {
            /* Redraw the frame we stepped back to, then return to the state before it. */
            pad_state[0] = old_pad;
            machine_run_frame(framebuffer, FB_W);
            rewind_restore_head();
            sound_out_count = 0;            /* silent while rewinding */
        } else if (!rewinding) {
            pad_state[0] = key_bits(keys) | controller_bits(pad);
            rewind_push(pad_state[0]);
            cheats_frame();
            machine_run_frame(framebuffer, FB_W);
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
            if (SDL_GetQueuedAudioSize(audio) < (Uint32)sound_out_rate * 4 / 10)
                SDL_QueueAudio(audio, sound_out, (Uint32)sound_out_count * 4);
        }

        SDL_Rect dst = display_rect(ww, wh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (settings.internal_scale > 1 && hd_frame.h == vdp_screen_height()) {
            hd_compose(settings.internal_scale);
            if (!hd_tex || hd_tex_w != hd_w || hd_tex_h != hd_h) {
                if (hd_tex) SDL_DestroyTexture(hd_tex);
                hd_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, hd_w, hd_h);
                SDL_SetTextureScaleMode(hd_tex, SDL_ScaleModeLinear);   /* the last, non-integer step to the window */
                hd_tex_w = hd_w; hd_tex_h = hd_h;
            }
            SDL_UpdateTexture(hd_tex, NULL, hd_px, hd_w * 4);
            SDL_RenderCopy(ren, hd_tex, NULL, &dst);
        } else {
            SDL_UpdateTexture(tex, NULL, framebuffer, FB_W * 4);
            SDL_Rect src = { 0, 0, frame_width(), vdp_screen_height() };
            SDL_RenderCopy(ren, tex, &src, &dst);
        }
        SDL_RenderPresent(ren);

        next += (Uint64)(frame_s * freq);
        Uint64 now = SDL_GetPerformanceCounter();
        if (now < next) SDL_Delay((Uint32)((next - now) * 1000 / freq));
        else if (now - next > freq / 4) next = now; /* fell far behind: don't try to catch up */
    }

    save_coverage();
    save_sram();
    settings_save(ini);
    SDL_Quit();
    return 0;
}
