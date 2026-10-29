/* Minimal PNG writer (stored deflate blocks), so screenshots open anywhere. */
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int png_write(const char *path, const uint32_t *px, int w, int h, int pitch) {
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * pitch + x];
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
