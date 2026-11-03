/* Save state slots: states/slotN.state (the snapshot) and states/slotN.thumb (a half-size
 * picture of the screen when it was saved, for the in-game menu). Each version of the game
 * has its own folder (states-fr, ...: game_file_name). */
#include "savestate.h"
#include "genesis.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#define make_dir(p) mkdir(p, 0755)
#endif

#define THUMB_MAGIC 0x424D4854u         /* "THMB" */

static char dir[1100];

void savestate_init(const char *base_dir) {
    snprintf(dir, sizeof dir, "%sstates", base_dir);
}

static void slot_path(char *out, size_t n, int slot, const char *ext) {
    char own[1200];
    game_file_name(own, sizeof own, dir);
    make_dir(own);
    snprintf(out, n, "%s/slot%d.%s", own, slot, ext);
}

bool savestate_save(int slot, const uint32_t *fb, int w, int h, int pitch) {
    static uint8_t buf[STATE_MAX];
    size_t len = state_save(buf, sizeof buf);
    char p[1200];
    slot_path(p, sizeof p, slot, "state");
    FILE *f = len ? fopen(p, "wb") : NULL;
    if (!f) return false;
    bool ok = fwrite(buf, 1, len, f) == len;
    fclose(f);
    if (!ok || !fb) return ok;

    /* Thumbnail: average 2x2 blocks. */
    uint32_t tw = (uint32_t)w / 2, th = (uint32_t)h / 2, magic = THUMB_MAGIC;
    uint32_t *t = malloc((size_t)tw * th * 4);
    for (uint32_t y = 0; y < th; y++)
        for (uint32_t x = 0; x < tw; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t px = fb[(y * 2 + (k >> 1)) * (uint32_t)pitch + x * 2 + (k & 1)];
                r += (px >> 16) & 255; g += (px >> 8) & 255; b += px & 255;
            }
            t[y * tw + x] = (r / 4) << 16 | (g / 4) << 8 | (b / 4);
        }
    slot_path(p, sizeof p, slot, "thumb");
    f = fopen(p, "wb");
    if (f) {
        fwrite(&magic, 4, 1, f); fwrite(&tw, 4, 1, f); fwrite(&th, 4, 1, f);
        fwrite(t, 4, (size_t)tw * th, f);
        fclose(f);
    }
    free(t);
    return true;
}

bool savestate_load(int slot) {
    static uint8_t buf[STATE_MAX];
    char p[1200];
    slot_path(p, sizeof p, slot, "state");
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return state_load(buf, len);
}

bool savestate_info(int slot, SlotInfo *info) {
    memset(info, 0, sizeof *info);
    char p[1200];
    struct stat st;
    slot_path(p, sizeof p, slot, "state");
    if (stat(p, &st) != 0) return false;
    info->exists = true;
    info->saved_at = st.st_mtime;
    slot_path(p, sizeof p, slot, "thumb");
    FILE *f = fopen(p, "rb");
    if (!f) return true;
    uint32_t magic = 0, w = 0, h = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == THUMB_MAGIC && fread(&w, 4, 1, f) == 1 && fread(&h, 4, 1, f) == 1 &&
        w > 0 && h > 0 && w <= 256 && h <= 128) {
        info->thumb = malloc((size_t)w * h * 4);
        if (fread(info->thumb, 4, (size_t)w * h, f) == (size_t)w * h) { info->w = (int)w; info->h = (int)h; }
        else { free(info->thumb); info->thumb = NULL; }
    }
    fclose(f);
    return true;
}

void savestate_info_free(SlotInfo *info) { free(info->thumb); info->thumb = NULL; }
