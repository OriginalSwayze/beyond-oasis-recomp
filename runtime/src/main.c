/* Beyond Oasis Recomp: window, input, frame pacing.
 *   BeyondOasis.exe [rom path]                 play
 *   BeyondOasis.exe --headless N --shot F.bmp  run N frames without a window, save the last frame
 *   headless extras: --press "FRAME:BUTTONS[:FRAMES],..." --shots EVERY DIR --widescreen 0|1 */
#include "genesis.h"
#include "settings.h"
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

/* Minimal PNG writer (stored deflate blocks), so screenshots open anywhere. */
static uint32_t crc_table[256];
static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8];
    put32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    fwrite(data, 1, n, f);
    uint32_t c = png_crc(png_crc(0, (const uint8_t *)type, 4), data, n);
    put32(h, c);
    fwrite(h, 1, 4, f);
}

static int save_png(const char *path) {
    int w = frame_width(), h = vdp_screen_height();
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = framebuffer[y * FB_W + x];
            *r++ = (uint8_t)(p >> 16); *r++ = (uint8_t)(p >> 8); *r++ = (uint8_t)p;
        }
    }
    size_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(raw_n + blocks * 5 + 6), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t at = 0; at < raw_n; at += 65535) {
        size_t n = raw_n - at < 65535 ? raw_n - at : 65535;
        *o++ = at + n == raw_n;
        *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8); *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, raw + at, n);
        o += n;
    }
    put32(o, b << 16 | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    if (f) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        uint8_t ihdr[13];
        put32(ihdr, (uint32_t)w); put32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t)(o - z));
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(raw); free(z);
    return f ? 0 : -1;
}

/* Saves as PNG or BMP depending on the file extension. */
static int save_shot(const char *path) {
    size_t n = strlen(path);
    return n > 4 && !strcmp(path + n - 4, ".png") ? save_png(path) : save_bmp(path);
}

static void apply_settings(void) {
    vdp_opts.extra_width = settings_extra_width();
    widescreen_patches(vdp_opts.extra_width);
    vdp_opts.no_sprite_limit = settings.no_sprite_limit;
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
        else snprintf(settings.rom_path, sizeof settings.rom_path, "%s", argv[i]);
    }

    if (SDL_Init(headless_frames ? 0 : SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
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

    if (headless_frames) {
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
    SDL_GameController *pad = NULL;
    SDL_AudioSpec want = { .freq = 48000, .format = AUDIO_S16SYS, .channels = 2, .samples = 1024 }, have;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) { sound_out_rate = have.freq; SDL_PauseAudioDevice(audio, 0); }

    const double frame_s = 1.0 / 59.922743;     /* NTSC Genesis refresh rate */
    Uint64 freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
                settings.fullscreen = !settings.fullscreen;
                SDL_SetWindowFullscreen(win, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F5) {
                settings.widescreen = !settings.widescreen;
                apply_settings();
            }
        }
        pad_state[0] = key_bits(SDL_GetKeyboardState(NULL)) | controller_bits(pad);

        machine_run_frame(framebuffer, FB_W);
        save_sram();
        if (audio) {
            /* Keep about 3 frames queued: drop audio if we fall far behind. */
            if (SDL_GetQueuedAudioSize(audio) < (Uint32)sound_out_rate * 4 / 10)
                SDL_QueueAudio(audio, sound_out, (Uint32)sound_out_count * 4);
        }

        SDL_UpdateTexture(tex, NULL, framebuffer, FB_W * 4);
        int ww, wh;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        SDL_Rect src = { 0, 0, frame_width(), vdp_screen_height() };
        SDL_Rect dst = display_rect(ww, wh);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src, &dst);
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
