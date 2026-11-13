/* HD texture packs: tile keys, dumping, loading. See texpack.h. */
#include "texpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis.h"
#include "png.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define make_dir(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

/* ---- Keys -------------------------------------------------------------------------------- */
static uint64_t tile_key(int tile, int pal) {
    uint64_t h = 1469598103934665603ull;           /* FNV-1a over pattern and colours */
    const uint8_t *p = vdp.vram + (tile & 0x7FF) * 32;
    for (int i = 0; i < 32; i++) { h ^= p[i]; h *= 1099511628211ull; }
    for (int i = 0; i < 16; i++) {
        uint16_t c = vdp.cram[(pal & 3) * 16 + i];
        h ^= (uint8_t)c; h *= 1099511628211ull;
        h ^= (uint8_t)(c >> 8); h *= 1099511628211ull;
    }
    return h;
}

/* ---- The loaded pack: key -> id, open addressing -------------------------------------- */
#define MAP_SIZE 131072                 /* a power of two, over twice the most images */
typedef struct { uint64_t key; uint16_t id; } Slot;
static Slot *map;
typedef struct { uint32_t *px; int size; } Image;
static Image *images;                   /* [id], id 0 unused */
static int image_count;

static uint16_t map_get(uint64_t key) {
    if (!map) return 0;
    for (uint32_t i = (uint32_t)key & (MAP_SIZE - 1);; i = (i + 1) & (MAP_SIZE - 1)) {
        if (!map[i].id) return 0;
        if (map[i].key == key) return map[i].id;
    }
}

static void map_put(uint64_t key, uint16_t id) {
    for (uint32_t i = (uint32_t)key & (MAP_SIZE - 1);; i = (i + 1) & (MAP_SIZE - 1))
        if (!map[i].id || map[i].key == key) { map[i].key = key; map[i].id = id; return; }
}

/* ---- Dumping ------------------------------------------------------------------------------ */
static bool dumping;
static char dump_dir[1100];
static uint64_t *dumped;                /* keys written, open addressing (0 = empty) */
static int dumped_count;
#define DUMP_SIZE 131072

static bool dumped_has(uint64_t key) {
    if (!key) key = 1;
    for (uint32_t i = (uint32_t)key & (DUMP_SIZE - 1);; i = (i + 1) & (DUMP_SIZE - 1)) {
        if (!dumped[i]) { if (dumped_count < DUMP_SIZE / 2) { dumped[i] = key; dumped_count++; } return false; }
        if (dumped[i] == key) return true;
    }
}

static void dump_tile(uint64_t key, int tile, int pal) {
    if (dumped_has(key)) return;
    uint32_t px[64];
    const uint8_t *p = vdp.vram + (tile & 0x7FF) * 32;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int c = (x & 1) ? (p[y * 4 + x / 2] & 0xF) : (p[y * 4 + x / 2] >> 4);
            px[y * 8 + x] = c ? 0xFF000000u | vdp_color_raw(vdp.cram[(pal & 3) * 16 + c]) : 0;
        }
    char path[1200];
    snprintf(path, sizeof path, "%s%016llx.png", dump_dir, (unsigned long long)key);
    FILE *f = fopen(path, "rb");                /* already there from an earlier run */
    if (f) { fclose(f); return; }
    png_write_rgba(path, px, 8, 8);
}

/* ---- Per-frame cache: tile and palette line -> id --------------------------------------- */
static uint32_t frame_stamp = 1;
static uint32_t cache_stamp[2048 * 4];
static uint16_t cache_id[2048 * 4];

void texpack_frame(void) { frame_stamp++; }

bool texpack_active(void) { return image_count > 0 || dumping; }
int texpack_count(void) { return image_count; }

uint16_t texpack_tile(int tile, int pal) {
    int k = (tile & 0x7FF) * 4 + (pal & 3);
    if (cache_stamp[k] == frame_stamp) return cache_id[k];
    uint64_t key = tile_key(tile, pal);
    if (dumping) dump_tile(key, tile, pal);
    uint16_t id = map_get(key);
    cache_stamp[k] = frame_stamp;
    cache_id[k] = id;
    return id;
}

const uint32_t *texpack_texels(uint16_t id, int *size) {
    if (!id || id > image_count) return NULL;
    *size = images[id].size;
    return images[id].px;
}

/* ---- Loading -------------------------------------------------------------------------------- */
static void unload(void) {
    for (int i = 1; i <= image_count; i++) free(images[i].px);
    free(images); images = NULL;
    free(map); map = NULL;
    image_count = 0;
    frame_stamp++;
}

static bool hex_name(const char *name, uint64_t *key) {
    if (strlen(name) != 20 || strcmp(name + 16, ".png")) return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = name[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint64_t)d;
    }
    *key = v;
    return true;
}

static void load_one(const char *dir, const char *name) {
    uint64_t key;
    if (!hex_name(name, &key) || image_count >= 65535 || image_count >= MAP_SIZE / 2) return;
    char path[1300];
    snprintf(path, sizeof path, "%s%s", dir, name);
    int w = 0, h = 0;
    uint32_t *px = png_read(path, &w, &h);
    if (!px) return;
    if (w != h || w % 8 || w < 8 || w > 128) { free(px); return; }   /* square, a multiple of 8 */
    Image *grown = realloc(images, sizeof(Image) * (size_t)(image_count + 2));
    if (!grown) { free(px); return; }
    images = grown;
    images[++image_count] = (Image){ px, w };
    map_put(key, (uint16_t)image_count);
}

int texpack_load(const char *base, const char *name) {
    unload();
    if (!name || !name[0]) return 0;
    char dir[1100];
    snprintf(dir, sizeof dir, "%stexture_packs/%s/", base ? base : "", name);
    map = calloc(MAP_SIZE, sizeof(Slot));
    if (!map) return 0;
#ifdef _WIN32
    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%s*.png", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h != -1) {
        do load_one(dir, fd.name); while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        for (struct dirent *e; (e = readdir(d));) load_one(dir, e->d_name);
        closedir(d);
    }
#endif
    if (!image_count) unload();
    return image_count;
}

void texpack_dump(const char *base, bool on) {
    dumping = on;
    if (!on) return;
    char dir[1100];
    snprintf(dir, sizeof dir, "%stexture_packs", base ? base : "");
    make_dir(dir);
    snprintf(dump_dir, sizeof dump_dir, "%stexture_packs/dump/", base ? base : "");
    snprintf(dir, sizeof dir, "%stexture_packs/dump", base ? base : "");
    make_dir(dir);
    if (!dumped) dumped = calloc(DUMP_SIZE, sizeof *dumped);
    if (!dumped) dumping = false;
    frame_stamp++;                      /* look at every tile again */
}

int texpack_list(const char *base, char names[][64], int max) {
    int n = 0;
#ifdef _WIN32
    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%stexture_packs/*", base ? base : "");
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h == -1) return 0;
    do {
        if (!(fd.attrib & _A_SUBDIR) || fd.name[0] == '.' || !strcmp(fd.name, "dump") || strlen(fd.name) >= 64) continue;
        snprintf(names[n++], 64, "%s", fd.name);
    } while (n < max && _findnext(h, &fd) == 0);
    _findclose(h);
#else
    char dir[1100];
    snprintf(dir, sizeof dir, "%stexture_packs", base ? base : "");
    DIR *d = opendir(dir);
    if (!d) return 0;
    for (struct dirent *e; n < max && (e = readdir(d));)
        if (e->d_type == DT_DIR && e->d_name[0] != '.' && strcmp(e->d_name, "dump") && strlen(e->d_name) < 64) snprintf(names[n++], 64, "%s", e->d_name);
    closedir(d);
#endif
    return n;
}
