/* Minimal PNG files: a writer (stored deflate blocks, so screenshots open anywhere) and a
 * reader for texture packs (inflate with fixed and dynamic Huffman codes; 8-bit grey, RGB,
 * palette, grey + alpha and RGBA images, not interlaced). */
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
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

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

/* Writes rows already laid out as PNG scanlines (a filter byte, then the pixels). */
static int write_raw(const char *path, const uint8_t *raw, size_t raw_n, int w, int h, int colour_type) {
    size_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(raw_n + blocks * 5 + 6), *o = z;
    if (!z) return -1;
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
        ihdr[8] = 8; ihdr[9] = (uint8_t)colour_type; ihdr[10] = ihdr[11] = ihdr[12] = 0;
        fwrite(sig, 1, 8, f);
        png_chunk(f, "IHDR", ihdr, 13);
        png_chunk(f, "IDAT", z, (uint32_t)(o - z));
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    }
    free(z);
    return f ? 0 : -1;
}

int png_write(const char *path, const uint32_t *px, int w, int h, int pitch) {
    size_t row = (size_t)w * 3 + 1, raw_n = row * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * pitch + x];
            *r++ = (uint8_t)(p >> 16); *r++ = (uint8_t)(p >> 8); *r++ = (uint8_t)p;
        }
    }
    int result = write_raw(path, raw, raw_n, w, h, 2);
    free(raw);
    return result;
}

int png_write_rgba(const char *path, const uint32_t *px, int w, int h) {
    size_t row = (size_t)w * 4 + 1, raw_n = row * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * w + x];
            *r++ = (uint8_t)(p >> 16); *r++ = (uint8_t)(p >> 8); *r++ = (uint8_t)p; *r++ = (uint8_t)(p >> 24);
        }
    }
    int result = write_raw(path, raw, raw_n, w, h, 6);
    free(raw);
    return result;
}

/* ---- Inflate (RFC 1951) --------------------------------------------------------------- */
typedef struct { const uint8_t *p, *end; uint32_t bits; int nbits; bool bad; } Bits;
typedef struct { uint16_t count[16], symbol[288]; } Huff;

static int bit(Bits *b, int n) {
    while (b->nbits < n) {
        if (b->p >= b->end) { b->bad = true; return 0; }
        b->bits |= (uint32_t)*b->p++ << b->nbits;
        b->nbits += 8;
    }
    int v = (int)(b->bits & ((1u << n) - 1));
    b->bits >>= n;
    b->nbits -= n;
    return v;
}

static void huff_build(Huff *h, const uint8_t *lengths, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(Bits *b, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bit(b, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
        if (b->bad) return -1;
    }
    b->bad = true;
    return -1;
}

typedef struct { uint8_t *data; size_t len, cap; } Out;

static bool out_put(Out *o, uint8_t v) {
    if (o->len == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 65536;
        uint8_t *d = realloc(o->data, cap);
        if (!d) return false;
        o->data = d; o->cap = cap;
    }
    o->data[o->len++] = v;
    return true;
}

static bool inflate_block(Bits *b, Out *o, const Huff *lit, const Huff *dist) {
    static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = huff_decode(b, lit);
        if (b->bad || sym < 0) return false;
        if (sym < 256) { if (!out_put(o, (uint8_t)sym)) return false; continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = len_base[sym] + bit(b, len_extra[sym]);
        int ds = huff_decode(b, dist);
        if (b->bad || ds < 0 || ds >= 30) return false;
        size_t d = dist_base[ds] + (size_t)bit(b, dist_extra[ds]);
        if (d > o->len) return false;
        for (int i = 0; i < len; i++) if (!out_put(o, o->data[o->len - d])) return false;
    }
}

static bool inflate_all(const uint8_t *src, size_t n, Out *o) {
    if (n < 2) return false;
    Bits b = { src + 2, src + n, 0, 0, false };   /* skip the zlib header */
    int last;
    do {
        last = bit(&b, 1);
        int type = bit(&b, 2);
        if (b.bad) return false;
        if (type == 0) {
            b.bits = 0; b.nbits = 0;            /* to the byte boundary */
            if (b.end - b.p < 4) return false;
            size_t len = b.p[0] | (size_t)b.p[1] << 8;
            b.p += 4;
            if ((size_t)(b.end - b.p) < len) return false;
            for (size_t i = 0; i < len; i++) if (!out_put(o, b.p[i])) return false;
            b.p += len;
        } else if (type == 1) {
            static Huff lit, dist;
            static bool made;
            if (!made) {
                uint8_t l[288];
                for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                huff_build(&lit, l, 288);
                for (int i = 0; i < 30; i++) l[i] = 5;
                huff_build(&dist, l, 30);
                made = true;
            }
            if (!inflate_block(&b, o, &lit, &dist)) return false;
        } else if (type == 2) {
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int hlit = bit(&b, 5) + 257, hdist = bit(&b, 5) + 1, hclen = bit(&b, 4) + 4;
            uint8_t lengths[320] = { 0 }, cl[19] = { 0 };
            for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)bit(&b, 3);
            Huff clh, lit, dist;
            huff_build(&clh, cl, 19);
            for (int i = 0; i < hlit + hdist;) {
                int sym = huff_decode(&b, &clh);
                if (b.bad || sym < 0) return false;
                if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; }
                int rep = 0, val = 0;
                if (sym == 16) { if (!i) return false; val = lengths[i - 1]; rep = 3 + bit(&b, 2); }
                else if (sym == 17) rep = 3 + bit(&b, 3);
                else rep = 11 + bit(&b, 7);
                if (i + rep > hlit + hdist) return false;
                while (rep--) lengths[i++] = (uint8_t)val;
            }
            huff_build(&lit, lengths, hlit);
            huff_build(&dist, lengths + hlit, hdist);
            if (!inflate_block(&b, o, &lit, &dist)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

/* ---- Reader ------------------------------------------------------------------------- */
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

uint32_t *png_read(const char *path, int *w_out, int *h_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = size > 8 ? malloc((size_t)size) : NULL;
    if (!file || fread(file, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(file); return NULL; }
    fclose(f);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint32_t *result = NULL;
    uint8_t *idat = NULL;
    size_t idat_n = 0;
    int w = 0, h = 0, depth = 0, type = 0, interlace = 0;
    uint32_t palette[256] = { 0 };
    for (int i = 0; i < 256; i++) palette[i] = 0xFF000000u;
    Out out = { 0 };
    if (memcmp(file, sig, 8)) goto done;
    for (const uint8_t *p = file + 8; p + 12 <= file + size;) {
        uint32_t n = get32(p);
        const uint8_t *data = p + 8;
        if (data + n + 4 > file + size) goto done;
        if (!memcmp(p + 4, "IHDR", 4) && n >= 13) {
            w = (int)get32(data); h = (int)get32(data + 4); depth = data[8]; type = data[9]; interlace = data[12];
        } else if (!memcmp(p + 4, "PLTE", 4)) {
            for (uint32_t i = 0; i < n / 3 && i < 256; i++)
                palette[i] = 0xFF000000u | (uint32_t)data[i * 3] << 16 | (uint32_t)data[i * 3 + 1] << 8 | data[i * 3 + 2];
        } else if (!memcmp(p + 4, "tRNS", 4) && type == 3) {
            for (uint32_t i = 0; i < n && i < 256; i++) palette[i] = (palette[i] & 0xFFFFFF) | (uint32_t)data[i] << 24;
        } else if (!memcmp(p + 4, "IDAT", 4)) {
            uint8_t *grown = realloc(idat, idat_n + n);
            if (!grown) goto done;
            idat = grown;
            memcpy(idat + idat_n, data, n);
            idat_n += n;
        } else if (!memcmp(p + 4, "IEND", 4)) {
            break;
        }
        p = data + n + 4;
    }
    int channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || depth != 8 || !channels || interlace || !idat) goto done;
    if (!inflate_all(idat, idat_n, &out)) goto done;
    size_t stride = (size_t)w * channels;
    if (out.len < (stride + 1) * (size_t)h) goto done;
    /* Undo the filters, row by row, in place. */
    for (int y = 0; y < h; y++) {
        uint8_t *row = out.data + (stride + 1) * (size_t)y, *cur = row + 1;
        const uint8_t *prev = y ? out.data + (stride + 1) * (size_t)(y - 1) + 1 : NULL;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)channels ? cur[i - channels] : 0, b = prev ? prev[i] : 0, c = prev && i >= (size_t)channels ? prev[i - channels] : 0;
            switch (row[0]) {
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            default: break;
            }
        }
    }
    result = malloc((size_t)w * h * 4);
    if (!result) goto done;
    for (int y = 0; y < h; y++) {
        const uint8_t *s = out.data + (stride + 1) * (size_t)y + 1;
        for (int x = 0; x < w; x++, s += channels) {
            uint32_t v;
            switch (type) {
            case 0: v = 0xFF000000u | (uint32_t)s[0] * 0x010101u; break;
            case 2: v = 0xFF000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2]; break;
            case 3: v = palette[s[0]]; break;
            case 4: v = (uint32_t)s[1] << 24 | (uint32_t)s[0] * 0x010101u; break;
            default: v = (uint32_t)s[3] << 24 | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2]; break;
            }
            result[(size_t)y * w + x] = v;
        }
    }
    *w_out = w; *h_out = h;
done:
    free(file); free(idat); free(out.data);
    return result;
}
