/* Z80 sound CPU: its memory map, bank window into 68000 space, and scheduling. */
#include "genesis.h"
#include "z80.h"

static z80 cpu;
static uint32_t bank;                   /* bits 15-23 of the 68000 address seen at $8000-$FFFF */
static double cycle_debt;               /* fractional Z80 cycles owed */

static uint8_t z_read(void *ud, uint16_t a) {
    (void)ud;
    if (a < 0x4000) return bus.zram[a & 0x1FFF];
    if (a < 0x6000) return ym_read();
    if (a >= 0x8000) return bus_read8(bank | (a & 0x7FFF));
    return 0xFF;
}

static void z_write(void *ud, uint16_t a, uint8_t v) {
    (void)ud;
    if (a < 0x4000) bus.zram[a & 0x1FFF] = v;
    else if (a < 0x6000) ym_write(a & 3, v);
    else if (a < 0x6100) bank = ((bank >> 1) | ((uint32_t)(v & 1) << 23)) & 0xFF8000; /* serial, 9 writes */
    else if (a == 0x7F11 || a == 0x7F13) psg_write(v);
    else if (a >= 0x8000) bus_write8(bank | (a & 0x7FFF), v);
}

static uint8_t z_in(z80 *z, uint8_t p) { (void)z; (void)p; return 0xFF; }
static void z_out(z80 *z, uint8_t p, uint8_t v) { (void)z; (void)p; (void)v; }

void z80_reset_cpu(void) {
    z80_init(&cpu);
    cpu.read_byte = z_read;
    cpu.write_byte = z_write;
    cpu.port_in = z_in;
    cpu.port_out = z_out;
    bank = 0;
    cycle_debt = 0;
}

/* Runs the Z80 for the time that `m68k_cycles` 68000 clocks take (Z80 = 68000 * 7/15). */
void z80_run(int m68k_cycles) {
    cycle_debt += m68k_cycles * 7.0 / 15.0;
    if (bus.z80_reset) { z80_reset_cpu(); return; }
    if (bus.z80_busreq) { cycle_debt = 0; return; } /* 68000 owns the bus: Z80 is halted */
    unsigned long target = cpu.cyc + (unsigned long)cycle_debt;
    cycle_debt -= (unsigned long)cycle_debt;
    while (cpu.cyc < target) z80_step(&cpu);
}

/* The VDP raises the Z80 interrupt at the start of V blank, for about one line. */
void z80_set_irq(bool on) {
    if (on) z80_gen_int(&cpu, 0xFF);
    else cpu.int_pending = false;
}
