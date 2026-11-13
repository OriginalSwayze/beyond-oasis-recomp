/* Beyond Oasis Recomp Setup: the installer.
 *
 * A wizard in the launcher's look: welcome, the install folder, the player's own ROM
 * (checked by its SHA-1 as it is picked), the install, done. The source travels inside
 * Setup.exe, a zip appended to the program with an 8-byte length and the tag BOAPAYLD after
 * it (the release workflow appends it); the install unpacks it into <folder>\source and runs
 * tools\install.ps1 there, hidden, showing its progress lines ("@@ <percent> <what>",
 * "@@error <why>"). The ROM is copied into <folder>\rom, so the game keeps working if the
 * original moves.
 *
 * Setup.exe copies itself into the folder and registers the game under HKCU\...\Uninstall:
 * Modify runs it again (to rebuild, e.g. after an update), Uninstall runs it with
 * --uninstall. Run from a source folder without a payload, --source DIR uses that folder in
 * place (for testing). --shot PAGE F.png renders a page to a PNG; --install-test unpacks and
 * builds into --folder without a window, shortcuts or registry entry, printing the progress. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"
#include "ui.h"

#define APP_NAME   "Beyond Oasis"
#define GAME_EXE   "BeyondOasis.exe"
#define UNINST_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BeyondOasisRecomp"
#define VERSION    "1.7"
#define ROM_NAME   "Beyond Oasis (USA)"

/* ---- The ROMs --------------------------------------------------------------------------- *
 * Each version of the cartridge is a language (config/versions.json); any of them builds. */
#define VERSIONS 6
static const struct { const char *name, *language, *sha1; } versions[VERSIONS] = {
    { "Beyond Oasis (USA)", "English", "2944910c07c02eace98c17d78d07bef7859d386a" },
    { "Story of Thor, The (Europe)", "English (Europe)", "178ef742dad227d4128fa81dddb116bad0cabe1d" },
    { "Legende de Thor, La (France)", "Fran\xC3\xA7" "ais", "677e1fbf2f6f90dd5a016f3bd5f305547249205a" },
    { "Story of Thor, The (Germany)", "Deutsch", "a82ffb7c4bf4b0f89f42a9cdc6600bc5bac1c854" },
    { "Story of Thor, The (Spain)", "Espa\xC3\xB1" "ol", "0fcc02355176e1c96043f4d827a3ff88d2d272df" },
    { "Story of Thor, The - Hikari o Tsugumono (Japan)", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "54296f5cf1917c568bb29b0086641c282b8884bd" },
};
static char picked[VERSIONS][MAX_PATH]; /* each version's ROM file to install (a zip's ROM is unpacked first) */
static char shown[VERSIONS][MAX_PATH];  /* what the player picked */
static char problem[200];               /* why the last pick was not taken */

static int picked_count(void) {
    int n = 0;
    for (int v = 0; v < VERSIONS; v++) n += picked[v][0] != 0;
    return n;
}
static bool installed;                  /* the game is already in the folder */

static bool sha1_file(const char *path, char out[41]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) == 0 &&
              BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0;
    static unsigned char buf[1 << 16];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) ok = BCryptHashData(h, buf, (ULONG)n, 0) == 0;
    unsigned char d[20];
    ok = ok && BCryptFinishHash(h, d, 20, 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    if (ok) for (int k = 0; k < 20; k++) sprintf(out + k * 2, "%02x", d[k]);
    return ok;
}

/* Runs a command line hidden and waits; its exit code, or -1. */
static int run_hidden(const char *cmd, const char *dir) {
    STARTUPINFOA si = { .cb = sizeof si, .dwFlags = STARTF_USESHOWWINDOW, .wShowWindow = SW_HIDE };
    PROCESS_INFORMATION pi;
    char line[4096];
    snprintf(line, sizeof line, "%s", cmd);
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static const char *tar_exe(void) {
    static char p[MAX_PATH];
    GetSystemDirectoryA(p, MAX_PATH);
    strcat(p, "\\tar.exe");             /* Windows' own: Git's reads C:\ as a host */
    return p;
}

/* A ROM picked: unpacked from a zip if need be, then checked. */
static void take_rom(const char *path) {
    char file[MAX_PATH], sum[41] = "";
    snprintf(file, sizeof file, "%s", path);
    const char *dot = strrchr(path, '.');
    if (dot && !_stricmp(dot, ".zip")) {
        char tmp[MAX_PATH], cmd[2 * MAX_PATH + 64];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(tmp + strlen(tmp), MAX_PATH - strlen(tmp), "beyond_oasis_setup");
        CreateDirectoryA(tmp, NULL);
        snprintf(cmd, sizeof cmd, "\"%s\" -xf \"%s\" -C \"%s\"", tar_exe(), path, tmp);
        file[0] = 0;
        if (run_hidden(cmd, NULL) == 0) {
            const char *exts[3] = { "*.md", "*.bin", "*.gen" };
            for (int e = 0; e < 3 && !file[0]; e++) {
                char pat[MAX_PATH];
                WIN32_FIND_DATAA fd;
                snprintf(pat, sizeof pat, "%s\\%s", tmp, exts[e]);
                HANDLE fh = FindFirstFileA(pat, &fd);
                if (fh != INVALID_HANDLE_VALUE) { snprintf(file, sizeof file, "%s\\%s", tmp, fd.cFileName); FindClose(fh); }
            }
        }
        if (!file[0]) { snprintf(problem, sizeof problem, "No ROM (.md, .bin or .gen) in that zip."); return; }
    }
    if (!sha1_file(file, sum)) { snprintf(problem, sizeof problem, "Could not read that file."); return; }
    int v = 0;
    while (v < VERSIONS && strcmp(sum, versions[v].sha1)) v++;
    if (v == VERSIONS) {
        snprintf(problem, sizeof problem, "Not a ROM this takes: Beyond Oasis or The Story of Thor (USA, Europe, France, Germany, Spain or Japan), unchanged (no hacks or headers).");
        return;
    }
    /* A zip's ROM is unpacked to the same place each time: keep each version's apart. */
    if (strcmp(file, path)) {
        char own[MAX_PATH];
        snprintf(own, sizeof own, "%s.%d", file, v);
        MoveFileExA(file, own, MOVEFILE_REPLACE_EXISTING);
        snprintf(file, sizeof file, "%s", own);
    }
    snprintf(picked[v], MAX_PATH, "%s", file);
    snprintf(shown[v], MAX_PATH, "%s", path);
    problem[0] = 0;
}

/* ---- Pages ---------------------------------------------------------------------------- */
enum { PAGE_WELCOME, PAGE_FOLDER, PAGE_ROM, PAGE_INSTALL, PAGE_DONE, PAGE_FAILED, PAGE_UNINSTALL, PAGE_REMOVED };
static int page;
static char folder[MAX_PATH], source_dir[MAX_PATH];
static bool desktop_icon = true, launch_after = true, keep_saves = true, reinstall;
static int focus;                       /* the focused control on the page */
static char exe_path[MAX_PATH];

/* The install's progress, from the installer thread. */
static SDL_mutex *lock;
static struct { int percent; char what[200]; char log[6][200]; int nlog; bool done, failed; char error[300]; } prog;
static HANDLE child;

/* The launcher's look: dark brushed metal, gold lettering, an ember accent. */
#define C_GOLD    rgb_hex(0xD9B567)
#define C_GOLD_HI rgb_hex(0xFBEBB8)
#define C_GOLD_LO rgb_hex(0xC08A2C)
#define C_EMBER   rgb_hex(0xC8642C)
#define C_TEXT    rgb_hex(0xE6E9EF)
#define C_DIM     rgb_hex(0x8C93A3)
#define HEAD      "Palatino Linotype"
#define BODY      "Segoe UI"

typedef struct { float x, y, w, h; int id; } Button;
static Button buttons[16];
static int nbuttons;
static float mouse_x, mouse_y;

static bool hovering(const Button *b) { return mouse_x >= b->x && mouse_x < b->x + b->w && mouse_y >= b->y && mouse_y < b->y + b->h; }

static void title_text(const char *s, float px, float tr, float x, float y) {
    ui_draw_text(s, HEAD, px, 700, tr, x + 2, y + 3, rgb(0, 0, 0), rgb(0, 0, 0), .6f);
    ui_draw_text(s, HEAD, px, 700, tr, x, y, C_GOLD_HI, C_GOLD_LO, 1);
}

static float wrap(const char *s, float x, float y, float w, float px, Rgb c, float a) {
    char line[400] = "", word[200];
    const char *p = s;
    while (*p) {
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && *p != '\n' && n < 199) word[n++] = *p++;
        word[n] = 0;
        char trial[600];
        snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
        if (ui_text_width(trial, BODY, px, 400, 0) > w && line[0]) {
            ui_draw_text(line, BODY, px, 400, 0, x, y, c, c, a);
            y += px * 1.45f;
            snprintf(line, sizeof line, "%s", word);
        } else snprintf(line, sizeof line, "%s", trial);
        if (*p == '\n') { ui_draw_text(line, BODY, px, 400, 0, x, y, c, c, a); y += px * 1.45f; line[0] = 0; p++; }
    }
    if (line[0]) { ui_draw_text(line, BODY, px, 400, 0, x, y, c, c, a); y += px * 1.45f; }
    return y;
}

/* A button; `kind` 0 plain, 1 primary (gold), 2 disabled. */
static void button(float x, float y, float w, float h, const char *label, int kind, int id) {
    Button *b = &buttons[nbuttons < 16 ? nbuttons++ : 15];
    *b = (Button){ x, y, w, h, id };
    bool hot = kind != 2 && (hovering(b) || focus == id);
    Rgb top = kind == 1 ? rgb_hex(0xF3DDA4) : kind == 2 ? rgb_hex(0x15181F) : rgb_hex(0x232836);
    Rgb bottom = kind == 1 ? rgb_hex(0xB8862E) : kind == 2 ? rgb_hex(0x111319) : rgb_hex(0x161A24);
    if (hot) { top = add(top, rgb(1, 1, 1), .08f); bottom = add(bottom, rgb(1, 1, 1), .08f); }
    ui_panel(x, y, w, h, 10, top, bottom, 1, hot ? C_GOLD_HI : kind == 1 ? C_GOLD : rgb_hex(0x3A4252), 1);
    float tw = ui_text_width(label, HEAD, 20, 700, 5);
    Rgb tc = kind == 1 ? rgb_hex(0x2A1C08) : kind == 2 ? rgb_hex(0x4A505E) : C_TEXT;
    ui_draw_text(label, HEAD, 20, 700, 5, x + w / 2 - tw / 2, y + h / 2 - 14, tc, tc, 1);
}

static void checkbox(float x, float y, const char *label, bool on, int id) {
    Button *b = &buttons[nbuttons < 16 ? nbuttons++ : 15];
    *b = (Button){ x, y, 30 + ui_text_width(label, BODY, 17, 400, 0) + 10, 26, id };
    bool hot = hovering(b) || focus == id;
    ui_panel(x, y + 2, 22, 22, 5, rgb_hex(0x232836), rgb_hex(0x161A24), 1, hot ? C_GOLD_HI : rgb_hex(0x5A6272), 1);
    if (on) ui_fill_rect(x + 6, y + 8, 10, 10, C_GOLD, 1);
    ui_draw_text(label, BODY, 17, 400, 0, x + 32, y + 2, C_TEXT, C_TEXT, 1);
}

enum { B_NEXT = 1, B_BACK, B_CANCEL, B_BROWSE_DIR, B_ROM, B_DESKTOP, B_LAUNCH, B_KEEP, B_FINISH, B_REMOVE };

static const char *steps[4] = { "WELCOME", "FOLDER", "ROM", "INSTALL" };

static void draw_frame(float t) {
    for (int y = 0; y < ui_h; y++) {
        uint32_t v = pack(mix(rgb_hex(0x10131B), rgb_hex(0x06070A), (float)y / ui_h));
        for (int x = 0; x < ui_w; x++) ui_px[y * ui_w + x] = v;
    }
    /* A few slow embers drifting up behind the panel. */
    for (int k = 0; k < 24; k++) {
        float sx = fmodf(k * 211.7f + sinf(t * .3f + k) * 30, 1280), sy = 720 - fmodf(t * (12 + k % 5 * 6) + k * 97.3f, 760);
        ui_fill_rect(sx, sy, 2, 2, C_EMBER, .35f + .25f * sinf(t * 2 + k));
    }
    ui_panel(60, 40, 1160, 640, 18, rgb_hex(0x171B26), rgb_hex(0x0B0D12), 1, C_GOLD, .45f);
    title_text("BEYOND OASIS", 44, 8, 100, 62);
    float tw = ui_text_width("BEYOND OASIS", HEAD, 44, 700, 8);
    ui_draw_text("SETUP", HEAD, 18, 700, 6, 122 + tw, 86, C_TEXT, C_DIM, 1);
    ui_fill_rect(102, 124, 160, 2, C_GOLD, .9f);
    if (page <= PAGE_INSTALL) {         /* the steps, along the top right */
        for (int k = 0; k < 4; k++) {
            float x = 812 + k * 94;
            bool on = k == page, past = k < page;
            ui_fill_rect(x, 80, 86, 3, on ? C_GOLD_HI : past ? rgb_hex(0x8A7444) : rgb_hex(0x2E3442), 1);
            ui_draw_text(steps[k], HEAD, 12, 700, 2, x, 90, on ? C_GOLD_HI : C_DIM, on ? C_GOLD : C_DIM, 1);
        }
    }
}

static void draw_page(float t) {
    nbuttons = 0;
    draw_frame(t);
    float x = 102, y = 160, w = 1076;
    switch (page) {
    case PAGE_WELCOME:
        title_text(reinstall ? "WELCOME BACK" : "WELCOME", 32, 5, x, y);
        y = wrap(reinstall
                 ? "Beyond Oasis is installed. Setup can build it again from your ROM, for example after downloading a newer Setup. Your settings, saves and save states stay as they are."
                 : "This installs Beyond Oasis as a native Windows program, with widescreen, higher resolutions and a high frame rate, lighting, water and spirit effects, cheats, save states, rewind and more.",
                 x, y + 62, w, 19, C_TEXT, 1);
        y = wrap("No part of the game comes with it. You pick your own ROM of the cartridge, " ROM_NAME " or another country's version (The Story of Thor), and it is translated into a PC program on this computer.",
                 x, y + 14, w, 19, C_TEXT, 1);
        wrap("Setup downloads the build tools it needs from their publishers (Node.js, CMake, Ninja and LLVM-MinGW, about 260 MB, checked against pinned SHA-256s) into the install folder, unless they are already on this PC. Nothing is installed into Windows itself, and no administrator rights are needed.",
             x, y + 14, w, 17, C_DIM, 1);
        button(1010, 596, 170, 52, "NEXT", 1, B_NEXT);
        button(820, 596, 170, 52, "CANCEL", 0, B_CANCEL);
        break;
    case PAGE_FOLDER:
        title_text("INSTALL FOLDER", 32, 5, x, y);
        y = wrap("The game, its build tools and its settings, saves and save states go here. Around 700 MB.", x, y + 62, w, 19, C_TEXT, 1);
        ui_panel(x, y + 20, 860, 52, 10, rgb_hex(0x0B0D12), rgb_hex(0x0B0D12), 1, rgb_hex(0x3A4252), 1);
        ui_draw_text(folder, BODY, 18, 400, 0, x + 16, y + 34, C_TEXT, C_TEXT, 1);
        button(x + 880, y + 20, 196, 52, "BROWSE", 0, B_BROWSE_DIR);
        button(1010, 596, 170, 52, "NEXT", 1, B_NEXT);
        button(820, 596, 170, 52, "BACK", 0, B_BACK);
        break;
    case PAGE_ROM: {
        title_text("YOUR ROMS", 32, 5, x, y);
        wrap("Pick your " ROM_NAME " ROM, or The Story of Thor from Europe, France, Germany, Spain or Japan: each version is a language you can switch between in the game. A .md, .bin or .gen file, or a .zip with one inside; pick several at once or drop them on this window. Each is checked as you pick it.",
             x, y + 58, w, 18, C_TEXT, 1);
        float cy = 300;
        bool ok = picked_count() > 0;
        ui_panel(x, cy, w, 214, 14, rgb_hex(0x1C2130), rgb_hex(0x12151D), 1, ok ? C_GOLD : rgb_hex(0x2E3442), ok ? .9f : 1);
        ui_fill_rect(x, cy + 14, 4, 186, ok ? C_GOLD : C_EMBER, 1);
        title_text("Your cartridges", 24, 2, x + 26, cy + 16);
        /* One line per version picked: its language and file. */
        float ly = cy + 62;
        for (int v = 0; v < VERSIONS; v++) {
            if (!picked[v][0]) continue;
            const char *base = strrchr(shown[v], '\\');
            if (!base) base = strrchr(shown[v], '/');
            char buf[MAX_PATH + 80];
            snprintf(buf, sizeof buf, "\xE2\x9C\x93  %s  \xE2\x80\x94  %s", versions[v].language, base ? base + 1 : shown[v]);
            ui_draw_text(buf, BODY, 16, 400, 0, x + 26, ly, rgb_hex(0x9FDCA6), rgb_hex(0x9FDCA6), 1);
            ly += 22;
        }
        const char *state = NULL;
        Rgb sc = C_DIM;
        if (problem[0]) { state = problem; sc = rgb_hex(0xF09A86); }
        else if (!ok && installed) state = "Installed. Pick the ROM again to build the game again.";
        else if (!ok) state = "No ROM picked yet.";
        if (state) ui_draw_text(state, BODY, 16, 400, 0, x + 26, ok ? ly + 6 : ly, sc, sc, 1);
        button(x + w - 200, cy + 16, 176, 52, ok ? "ADD" : "BROWSE", 0, B_ROM);
        button(1010, 596, 170, 52, "INSTALL", ok ? 1 : 2, B_NEXT);
        button(820, 596, 170, 52, "BACK", 0, B_BACK);
        break;
    }
    case PAGE_INSTALL: case PAGE_DONE: case PAGE_FAILED: {
        SDL_LockMutex(lock);
        bool done = page == PAGE_DONE, failed = page == PAGE_FAILED;
        title_text(done ? "READY TO PLAY" : failed ? "THE INSTALL STOPPED" : "INSTALLING", 32, 5, x, y);
        if (!done) {
            float bar = prog.percent / 100.f;
            ui_panel(x, y + 70, w, 30, 10, rgb_hex(0x0B0D12), rgb_hex(0x0B0D12), 1, rgb_hex(0x3A4252), 1);
            if (bar > 0) ui_panel(x + 3, y + 73, (w - 6) * bar, 24, 8, failed ? rgb_hex(0x7A2A20) : C_GOLD_HI,
                                  failed ? rgb_hex(0x5A1A10) : C_GOLD_LO, 1, C_GOLD_LO, 0);
            if (!failed) {                 /* a sheen running along the bar while it works */
                float sx = x + 3 + fmodf(t * 260, (w - 6) * bar + 1);
                ui_fill_rect(sx, y + 73, 40, 24, rgb(1, 1, .9f), .22f);
            }
            char pct[16];
            snprintf(pct, sizeof pct, "%d%%", prog.percent);
            ui_draw_text(pct, HEAD, 20, 700, 2, x + w - 60, y + 112, C_GOLD_HI, C_GOLD, 1);
            ui_draw_text(failed ? prog.error : prog.what, BODY, 19, 400, 0, x, y + 112, failed ? rgb_hex(0xF09A86) : C_TEXT,
                         failed ? rgb_hex(0xF09A86) : C_TEXT, 1);
            for (int k = 0; k < prog.nlog; k++)
                ui_draw_text(prog.log[k], "Consolas", 14, 400, 0, x, y + 170 + k * 22, rgb_hex(0x5E6574), rgb_hex(0x5E6574), 1);
            if (failed) {
                button(1010, 596, 170, 52, "CLOSE", 1, B_CANCEL);
                button(820, 596, 170, 52, "BACK", 0, B_BACK);
            } else button(1010, 596, 170, 52, "CANCEL", 0, B_CANCEL);
        } else {
            y = wrap("Beyond Oasis is installed. Open it from the Start menu (and the desktop, if you like). Its launcher has all the settings; your saves and save states live in the install folder.",
                     x, y + 62, w, 19, C_TEXT, 1);
            checkbox(x, y + 24, "Put a shortcut on the desktop", desktop_icon, B_DESKTOP);
            checkbox(x, y + 64, "Start the game now", launch_after, B_LAUNCH);
            button(1010, 596, 170, 52, "FINISH", 1, B_FINISH);
        }
        SDL_UnlockMutex(lock);
        break;
    }
    case PAGE_UNINSTALL:
        title_text("REMOVE BEYOND OASIS", 32, 5, x, y);
        y = wrap("This removes Beyond Oasis from this PC: the game, its build tools and the shortcuts. Your ROM file elsewhere is not touched.", x, y + 62, w, 19, C_TEXT, 1);
        ui_draw_text(folder, BODY, 17, 400, 0, x, y + 10, C_DIM, C_DIM, 1);
        checkbox(x, y + 54, "Keep my settings, saves and save states (in the install folder)", keep_saves, B_KEEP);
        button(1010, 596, 170, 52, "REMOVE", 1, B_REMOVE);
        button(820, 596, 170, 52, "CANCEL", 0, B_CANCEL);
        break;
    case PAGE_REMOVED:
        title_text("REMOVED", 32, 5, x, y);
        wrap(keep_saves ? "Beyond Oasis is removed. Your settings and saves are still in the install folder; run Setup again to pick up where you left off."
                        : "Beyond Oasis is removed.", x, y + 62, w, 19, C_TEXT, 1);
        button(1010, 596, 170, 52, "CLOSE", 1, B_CANCEL);
        break;
    }
}

/* ---- Windows' pickers ------------------------------------------------------------------ */
static void browse_folder(void) {
    BROWSEINFOA bi = { 0 };
    bi.lpszTitle = "Where to install Beyond Oasis";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderA(&bi);
    if (!id) return;
    char p[MAX_PATH];
    if (SHGetPathFromIDListA(id, p)) {
        size_t n = strlen(p);
        /* A folder of its own inside the one picked, unless it already is one. */
        if (n < strlen(APP_NAME) || _stricmp(p + n - strlen(APP_NAME), APP_NAME))
            snprintf(folder, sizeof folder, "%s%s%s", p, p[n - 1] == '\\' ? "" : "\\", APP_NAME);
        else snprintf(folder, sizeof folder, "%s", p);
    }
    CoTaskMemFree(id);
}

static void browse_rom(void) {
    static char files[16 * MAX_PATH];
    files[0] = 0;
    OPENFILENAMEA of = { .lStructSize = sizeof of };
    of.lpstrFilter = "Genesis ROMs (*.md;*.bin;*.gen;*.zip)\0*.md;*.bin;*.gen;*.zip\0All files (*.*)\0*.*\0";
    of.lpstrFile = files;
    of.nMaxFile = sizeof files;
    of.lpstrTitle = "Your Beyond Oasis / Story of Thor ROMs";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameA(&of)) return;
    /* One file: its path. Several: the folder, then each name, each ending in a 0. */
    const char *name = files + strlen(files) + 1;
    if (!*name) { take_rom(files); return; }
    for (; *name; name += strlen(name) + 1) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", files, name);
        take_rom(p);
    }
}

/* ---- The install ----------------------------------------------------------------------- */
static bool print_progress;

static void progress_line(const char *s) {
    if (print_progress) { printf("%s\n", s); fflush(stdout); }
    SDL_LockMutex(lock);
    if (!strncmp(s, "@@error ", 8)) { prog.failed = true; snprintf(prog.error, sizeof prog.error, "%s", s + 8); }
    else if (!strncmp(s, "@@ ", 3)) {
        char *end;
        long p = strtol(s + 3, &end, 10);
        prog.percent = p < 0 ? 0 : p > 100 ? 100 : (int)p;
        snprintf(prog.what, sizeof prog.what, "%s", *end == ' ' ? end + 1 : end);
    } else if (s[0]) {
        if (prog.nlog == 6) { memmove(prog.log, prog.log + 1, sizeof prog.log[0] * 5); prog.nlog = 5; }
        snprintf(prog.log[prog.nlog++], sizeof prog.log[0], "%.150s", s);
    }
    SDL_UnlockMutex(lock);
}

/* Copies each ROM picked into <folder>\rom under its version's name; out gets their paths,
 * separated by | for install.ps1. */
static bool copy_roms(char *out, size_t n) {
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\rom", folder);
    CreateDirectoryA(dir, NULL);
    out[0] = 0;
    for (int v = 0; v < VERSIONS; v++) {
        if (!picked[v][0]) continue;
        char to[MAX_PATH];
        snprintf(to, sizeof to, "%s\\%s.md", dir, versions[v].name);
        if (_stricmp(to, picked[v]) && !CopyFileA(picked[v], to, FALSE)) return false;
        snprintf(out + strlen(out), n - strlen(out), "%s%s", out[0] ? "|" : "", to);
    }
    return out[0] != 0;
}

static int SDLCALL install_thread(void *arg) {
    (void)arg;
    char src[MAX_PATH], cmd[4 * MAX_PATH + 512];
    SHCreateDirectoryExA(NULL, folder, NULL);
    /* The source: the payload unpacked into <folder>\source, or a folder given for testing. */
    if (source_dir[0]) snprintf(src, sizeof src, "%s", source_dir);
    else {
        progress_line("@@ 1 Unpacking the source");
        snprintf(src, sizeof src, "%s\\source", folder);
        FILE *self = fopen(exe_path, "rb");
        char tag[9] = "";
        uint64_t size = 0;
        bool ok = self && !_fseeki64(self, -16, SEEK_END) && fread(&size, 8, 1, self) == 1 && fread(tag, 1, 8, self) == 8 &&
                  !memcmp(tag, "BOAPAYLD", 8) && size > 0;
        char zip[MAX_PATH];
        snprintf(zip, sizeof zip, "%s\\source.zip", folder);
        FILE *out = ok ? fopen(zip, "wb") : NULL;
        if (out && !_fseeki64(self, -16 - (long long)size, SEEK_END)) {
            static char buf[1 << 16];
            uint64_t left = size;
            while (left && ok) {
                size_t n = fread(buf, 1, left < sizeof buf ? (size_t)left : sizeof buf, self);
                ok = n > 0 && fwrite(buf, 1, n, out) == n;
                left -= n;
            }
        } else ok = false;
        if (out) fclose(out);
        if (self) fclose(self);
        if (ok) {
            SHCreateDirectoryExA(NULL, src, NULL);
            snprintf(cmd, sizeof cmd, "\"%s\" -xf \"%s\" -C \"%s\" --strip-components=1", tar_exe(), zip, src);
            ok = run_hidden(cmd, NULL) == 0;
        }
        DeleteFileA(zip);
        if (!ok) { progress_line("@@error Could not unpack the source (Setup.exe may be damaged: download it again)."); return 1; }
    }
    /* The ROM, copied in, for the installer. */
    static char rom[VERSIONS * MAX_PATH];
    if (!copy_roms(rom, sizeof rom)) { progress_line("@@error Could not copy the ROMs into the install folder."); return 1; }
    snprintf(cmd, sizeof cmd, "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\\tools\\install.ps1\" -Install \"%s\" -Rom \"%s\"",
             src, folder, rom);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { .cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW, .wShowWindow = SW_HIDE };
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, src, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        progress_line("@@error Could not start PowerShell.");
        return 1;
    }
    CloseHandle(wr);
    child = pi.hProcess;
    char buf[4096], line[1024];
    int ln = 0;
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got)
        for (DWORD k = 0; k < got; k++) {
            if (buf[k] == '\n' || ln == (int)sizeof line - 1) { line[ln] = 0; if (ln && line[ln - 1] == '\r') line[ln - 1] = 0; progress_line(line); ln = 0; }
            else line[ln++] = buf[k];
        }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    child = NULL;
    SDL_LockMutex(lock);
    if (code && !prog.failed) { prog.failed = true; snprintf(prog.error, sizeof prog.error, "The install stopped (code %lu).", (unsigned long)code); }
    prog.done = true;
    SDL_UnlockMutex(lock);
    return 0;
}

/* ---- Shortcuts and the Installed apps entry --------------------------------------------- */
static void shortcut(const char *lnk, const char *target, const char *dir) {
    IShellLinkA *sl;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&sl))) return;
    IShellLinkA_SetPath(sl, target);
    IShellLinkA_SetWorkingDirectory(sl, dir);
    IShellLinkA_SetDescription(sl, APP_NAME);
    IPersistFile *pf;
    if (SUCCEEDED(IShellLinkA_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        WCHAR w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, lnk, -1, w, MAX_PATH);
        IPersistFile_Save(pf, w, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkA_Release(sl);
}

static void link_path(int csidl, char *out) {
    char base[MAX_PATH] = "";
    SHGetFolderPathA(NULL, csidl, NULL, 0, base);
    snprintf(out, MAX_PATH, "%s\\%s.lnk", base, APP_NAME);
}

static void finish_install(void) {
    char app[MAX_PATH], lnk[MAX_PATH], setup[MAX_PATH], cmd[MAX_PATH + 32];
    snprintf(app, sizeof app, "%s\\" GAME_EXE, folder);
    link_path(CSIDL_PROGRAMS, lnk);
    shortcut(lnk, app, folder);
    link_path(CSIDL_DESKTOPDIRECTORY, lnk);
    if (desktop_icon) shortcut(lnk, app, folder);
    else DeleteFileA(lnk);
    snprintf(setup, sizeof setup, "%s\\Setup.exe", folder);
    if (_stricmp(setup, exe_path)) CopyFileA(exe_path, setup, FALSE);
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, UNINST_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        const char *strs[][2] = { { "DisplayName", APP_NAME " Recomp" }, { "DisplayVersion", VERSION }, { "Publisher", "OriginalSwayze" },
                                  { "InstallLocation", folder }, { "DisplayIcon", app } };
        for (int i = 0; i < 5; i++) RegSetValueExA(k, strs[i][0], 0, REG_SZ, (const BYTE *)strs[i][1], (DWORD)strlen(strs[i][1]) + 1);
        snprintf(cmd, sizeof cmd, "\"%s\" --uninstall", setup);
        RegSetValueExA(k, "UninstallString", 0, REG_SZ, (const BYTE *)cmd, (DWORD)strlen(cmd) + 1);
        snprintf(cmd, sizeof cmd, "\"%s\"", setup);
        RegSetValueExA(k, "ModifyPath", 0, REG_SZ, (const BYTE *)cmd, (DWORD)strlen(cmd) + 1);
        RegCloseKey(k);
    }
    if (launch_after) ShellExecuteA(NULL, "open", app, NULL, folder, SW_SHOWNORMAL);
}

/* Deletes a folder and everything in it (SHFileOperation wants a double-null-ended path). */
static void remove_tree(const char *path) {
    char p[MAX_PATH + 2] = "";
    snprintf(p, MAX_PATH, "%s", path);
    p[strlen(p) + 1] = 0;
    SHFILEOPSTRUCTA op = { .wFunc = FO_DELETE, .pFrom = p, .fFlags = FOF_NO_UI };
    SHFileOperationA(&op);
}

static void uninstall(void) {
    char lnk[MAX_PATH], p[MAX_PATH];
    link_path(CSIDL_PROGRAMS, lnk); DeleteFileA(lnk);
    link_path(CSIDL_DESKTOPDIRECTORY, lnk); DeleteFileA(lnk);
    RegDeleteKeyA(HKEY_CURRENT_USER, UNINST_KEY);
    const char *parts[] = { "source", "rom" };
    for (int i = 0; i < 2; i++) { snprintf(p, sizeof p, "%s\\%s", folder, parts[i]); remove_tree(p); }
    snprintf(p, sizeof p, "%s\\" GAME_EXE, folder); DeleteFileA(p);
    /* With keep_saves, the settings, saves and states next to the game stay; otherwise the
     * rest of the folder goes too (all but Setup.exe, which is still running). */
    if (!keep_saves) {
        WIN32_FIND_DATAA fd;
        char pat[MAX_PATH];
        snprintf(pat, sizeof pat, "%s\\*", folder);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..") || !_stricmp(fd.cFileName, "Setup.exe")) continue;
                snprintf(p, sizeof p, "%s\\%s", folder, fd.cFileName);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
                else DeleteFileA(p);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    /* Setup.exe itself, once this program has closed: a hidden cmd waits, then deletes it
     * (and the folder, if nothing is left in it). */
    char cmd[3 * MAX_PATH];
    snprintf(cmd, sizeof cmd, "cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\\Setup.exe\" & rmdir \"%s\"", folder, folder);
    STARTUPINFOA si = { .cb = sizeof si, .dwFlags = STARTF_USESHOWWINDOW, .wShowWindow = SW_HIDE };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
}

/* ---- Actions ------------------------------------------------------------------------------ */
static bool act(int id) {
    if (id == B_CANCEL) {
        if (page == PAGE_INSTALL && child) TerminateProcess(child, 1);
        return false;
    }
    if (id == B_NEXT) {
        if (page == PAGE_ROM) {
            if (!picked_count()) return true;
            prog = (typeof(prog)){ 0 };
            page = PAGE_INSTALL;
            SDL_CreateThread(install_thread, "install", NULL);
        } else if (page < PAGE_ROM) page++;
    } else if (id == B_BACK) {
        if (page == PAGE_FAILED) page = PAGE_ROM;
        else if (page > PAGE_WELCOME) page--;
    } else if (id == B_BROWSE_DIR) browse_folder();
    else if (id == B_ROM) browse_rom();
    else if (id == B_DESKTOP) desktop_icon = !desktop_icon;
    else if (id == B_LAUNCH) launch_after = !launch_after;
    else if (id == B_KEEP) keep_saves = !keep_saves;
    else if (id == B_FINISH) { finish_install(); return false; }
    else if (id == B_REMOVE) { uninstall(); page = PAGE_REMOVED; }
    return true;
}

static void default_folder(void) {
    HKEY k;
    DWORD n = sizeof folder;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, UNINST_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        bool ok = RegQueryValueExA(k, "InstallLocation", NULL, NULL, (BYTE *)folder, &n) == ERROR_SUCCESS && folder[0];
        RegCloseKey(k);
        if (ok) { reinstall = true; return; }
    }
    char base[MAX_PATH] = "";
    SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base);
    snprintf(folder, sizeof folder, "%s\\Programs\\%s", base, APP_NAME);
}

static void find_installed(void) {
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s\\" GAME_EXE, folder);
    installed = GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
    /* A reinstall offers the ROMs it was built from, if they are still there. */
    for (int v = 0; installed && v < VERSIONS; v++) {
        snprintf(p, sizeof p, "%s\\rom\\%s.md", folder, versions[v].name);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) take_rom(p);
    }
}

int main(int argc, char **argv) {
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    default_folder();
    const char *shot = NULL;
    bool install_test = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--uninstall")) page = PAGE_UNINSTALL;
        else if (!strcmp(argv[i], "--source") && i + 1 < argc) snprintf(source_dir, sizeof source_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--folder") && i + 1 < argc) snprintf(folder, sizeof folder, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--rom") && i + 1 < argc) take_rom(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc) { page = atoi(argv[++i]); shot = argv[++i]; }
        else if (!strcmp(argv[i], "--install-test")) install_test = true;
    }
    if (!picked_count()) find_installed();
    lock = SDL_CreateMutex();
    if (install_test) {
        print_progress = true;
        if (!picked_count()) { printf("@@error %s\n", problem[0] ? problem : "No ROM: pass --rom FILE."); return 1; }
        install_thread(NULL);
        return prog.failed ? 1 : 0;
    }
    if (shot) {
        ui_begin(1280, 720);
        if (page == PAGE_INSTALL) { prog.percent = 62; snprintf(prog.what, sizeof prog.what, "Recompiling and building (a few minutes)"); prog.nlog = 2;
                                    snprintf(prog.log[0], 200, "[214/347] Building C object CMakeFiles/recomp.dir/generated/recomp_07.c.obj");
                                    snprintf(prog.log[1], 200, "[215/347] Building C object CMakeFiles/recomp.dir/generated/recomp_08.c.obj"); }
        draw_page(1);
        return png_write(shot, ui_px, ui_w, ui_h, ui_w) ? 1 : 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    SDL_Window *win = SDL_CreateWindow(page == PAGE_UNINSTALL ? APP_NAME " - Remove" : APP_NAME " Setup", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 1120, 630, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *tex = NULL;
    int tw = 0, th = 0;
    bool running = true;
    Uint64 t0 = SDL_GetPerformanceCounter();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                if (page == PAGE_INSTALL && child) TerminateProcess(child, 1);
                running = false;
            } else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN) {
                int ww, wh, mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                SDL_GetWindowSize(win, &ww, &wh);
                mouse_x = (mx * (float)tw / ww - ui_ox) / ui_s;
                mouse_y = (my * (float)th / wh - ui_oy) / ui_s;
                if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
                    for (int k = 0; k < nbuttons; k++)
                        if (hovering(&buttons[k])) { if (!act(buttons[k].id)) running = false; break; }
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode key = e.key.keysym.sym;
                if (key == SDLK_TAB || key == SDLK_RIGHT || key == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_UP) {
                    int at = 0, dir = key == SDLK_LEFT || key == SDLK_UP || (key == SDLK_TAB && (SDL_GetModState() & KMOD_SHIFT)) ? -1 : 1;
                    for (int k = 0; k < nbuttons; k++) if (buttons[k].id == focus) at = k;
                    if (nbuttons) focus = buttons[(at + dir + nbuttons) % nbuttons].id;
                } else if (key == SDLK_RETURN || key == SDLK_SPACE) {
                    int id = focus;
                    bool there = false;
                    for (int k = 0; k < nbuttons; k++) there |= buttons[k].id == id;
                    if (!there) id = page == PAGE_DONE ? B_FINISH : page == PAGE_UNINSTALL ? B_REMOVE : B_NEXT;
                    if (!act(id)) running = false;
                } else if (key == SDLK_ESCAPE && page != PAGE_INSTALL) running = false;
            } else if (e.type == SDL_DROPFILE) {    /* a ROM dropped on the window */
                if (page == PAGE_ROM) take_rom(e.drop.file);
                SDL_free(e.drop.file);
            }
        }
        SDL_LockMutex(lock);
        if (page == PAGE_INSTALL && prog.done) page = prog.failed ? PAGE_FAILED : PAGE_DONE;
        SDL_UnlockMutex(lock);
        int w, h;
        SDL_GetRendererOutputSize(ren, &w, &h);
        ui_begin(w, h);
        draw_page((float)(SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency());
        if (!tex || tw != ui_w || th != ui_h) {
            if (tex) SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, ui_w, ui_h);
            tw = ui_w; th = ui_h;
        }
        SDL_UpdateTexture(tex, NULL, ui_px, ui_w * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }
    SDL_Quit();
    CoUninitialize();
    return 0;
}
