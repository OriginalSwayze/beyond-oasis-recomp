/* 68000 memory map: cartridge ROM/SRAM, work RAM, Z80 area, I/O and VDP. */
#include "genesis.h"
#include <stdio.h>
#include <string.h>

Bus bus;

static const char ROM_SHA1[] = "2944910c07c02eace98c17d78d07bef7859d386a";

/* ---- SHA-1, only used to verify the user's ROM ---------------------------- */
static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_hex(const uint8_t *data, size_t len, char out[41]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 9 + 63) / 64) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int i = 0; i < 64; i++) {
            size_t p = off + i;
            if (p < len) blk[i] = data[p];
            else if (p == len) blk[i] = 0x80;
            else if (p >= total - 8) blk[i] = (uint8_t)(bits >> (8 * (total - 1 - p)));
            else blk[i] = 0;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[i * 4] << 24 | blk[i * 4 + 1] << 16 | blk[i * 4 + 2] << 8 | blk[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) sprintf(out + i * 8, "%08x", h[i]);
}

const char *bus_load_rom(const char *path) {
    static char msg[256];
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(msg, sizeof msg, "cannot open %s", path);
        return msg;
    }
    size_t n = fread(bus.rom, 1, ROM_SIZE, f);
    int extra = fgetc(f);
    fclose(f);
    if (n != ROM_SIZE || extra != EOF) return "wrong file size: this is not Beyond Oasis (USA)";
    char hex[41];
    sha1_hex(bus.rom, ROM_SIZE, hex);
    if (strcmp(hex, ROM_SHA1) != 0) {
        snprintf(msg, sizeof msg, "SHA-1 %s does not match Beyond Oasis (USA)", hex);
        return msg;
    }
    return NULL;
}

/* ---- Reads ------------------------------------------------------------------ */
static bool in_sram(uint32_t a) { return bus.sram_mapped && a >= 0x200000 && a < 0x200000 + SRAM_SIZE * 2; }

uint8_t bus_read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < 0x400000) {
        if (in_sram(a)) return (a & 1) ? bus.sram[(a - 0x200000) >> 1] : 0xFF;
        return a < ROM_SIZE ? bus.rom[a] : 0xFF;
    }
    if (a >= 0xE00000) return bus.ram[a & 0xFFFF];
    if (a >= 0xC00000 && a < 0xC00020) return vdp_read8(a);
    if (a >= 0xA00000 && a < 0xA10000) {
        if (a < 0xA04000) return bus.zram[a & 0x1FFF];
        if (a < 0xA06000) return ym_read();
        return 0xFF;
    }
    if (a >= 0xA10000 && a < 0xA10020) return io_read8(a);
    if (a == 0xA11100 || a == 0xA11101) {
        /* Bit 0 of the high byte reads 0 once the 68000 owns the Z80 bus. */
        uint8_t granted = bus.z80_busreq && !bus.z80_reset;
        return (a & 1) ? 0x00 : (granted ? 0x00 : 0x01);
    }
    return 0xFF;
}

uint16_t bus_read16(uint32_t a) {
    a &= 0xFFFFFE;
    if (a < ROM_SIZE && !in_sram(a)) return (uint16_t)(bus.rom[a] << 8 | bus.rom[a + 1]);
    if (a >= 0xE00000) return (uint16_t)(bus.ram[a & 0xFFFF] << 8 | bus.ram[(a & 0xFFFF) + 1]);
    if (a >= 0xC00000 && a < 0xC00020) return vdp_read16(a);
    if (a >= 0xA00000 && a < 0xA10000) {
        uint8_t v = bus_read8(a);
        return (uint16_t)(v << 8 | v); /* Z80 space is 8-bit: byte appears on both halves */
    }
    return (uint16_t)(bus_read8(a) << 8 | bus_read8(a + 1));
}

/* ---- Writes ----------------------------------------------------------------- */
void bus_write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xE00000) { bus.ram[a & 0xFFFF] = v; return; }
    if (in_sram(a)) {
        if (a & 1) { bus.sram[(a - 0x200000) >> 1] = v; bus.sram_dirty = true; }
        return;
    }
    if (a >= 0xC00000 && a < 0xC00020) { vdp_write8(a, v); return; }
    if (a >= 0xA00000 && a < 0xA10000) {
        if (a < 0xA04000) bus.zram[a & 0x1FFF] = v;
        else if (a < 0xA06000) ym_write(a & 3, v);
        else if (a == 0xA07F11) psg_write(v);
        return;
    }
    if (a >= 0xA10000 && a < 0xA10020) { io_write8(a, v); return; }
    if (a == 0xA11100) { bus.z80_busreq = v & 1; return; }
    if (a == 0xA11200) { bus.z80_reset = !(v & 1); return; }
    if (a == 0xA130F1) { bus.sram_mapped = v & 1; return; }
}

void bus_write16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    if (a >= 0xE00000) {
        bus.ram[a & 0xFFFF] = (uint8_t)(v >> 8);
        bus.ram[(a & 0xFFFF) + 1] = (uint8_t)v;
        return;
    }
    if (a >= 0xC00000 && a < 0xC00020) { vdp_write16(a, v); return; }
    if (a == 0xA11100 || a == 0xA11200) { bus_write8(a, (uint8_t)(v >> 8)); return; }
    if (a >= 0xA00000 && a < 0xA10000) { bus_write8(a, (uint8_t)(v >> 8)); return; }
    bus_write8(a, (uint8_t)(v >> 8));
    bus_write8(a + 1, (uint8_t)v);
}

/* ---- Musashi memory callbacks ---------------------------------------------- */
unsigned int m68k_read_memory_8(unsigned int a)  { return bus_read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return bus_read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return (unsigned)bus_read16(a) << 16 | bus_read16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v)  { bus_write8(a, (uint8_t)v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { bus_write16(a, (uint16_t)v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    bus_write16(a, (uint16_t)(v >> 16));
    bus_write16(a + 2, (uint16_t)v);
}
unsigned int m68k_read_disassembler_8(unsigned int a)  { return bus_read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return bus_read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
