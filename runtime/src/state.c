/* Machine snapshots for save states and rewind.
 *
 * A snapshot is taken between frames and holds everything that decides what the game
 * does next: 68000 and Z80 registers, work/Z80/save RAM, the VDP, I/O, the sound chips,
 * and the scheduler's cycle counters. Loading it and running on with the same pad input
 * gives exactly the frames that followed when it was taken.
 *
 * The layout is raw structs, so a snapshot only loads into a build with the same layout;
 * the header carries a fingerprint of the struct sizes and rejects anything else. */
#include "genesis.h"
#include "m68kcpu.h"
#include "motion.h"
#include "fx.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC   0x5453424Fu      /* "OBST" */
#define STATE_VERSION 1u

void state_io(StateIO *s, void *p, size_t n) {
    if (!s->ok) return;
    if (s->pos + n > s->cap) { s->ok = false; return; }
    if (s->saving) memcpy(s->buf + s->pos, p, n);
    else memcpy(p, s->buf + s->pos, n);
    s->pos += n;
}

#define IO(s, v) state_io((s), &(v), sizeof(v))

static uint32_t layout_fingerprint(void) {
    uint32_t h = 2166136261u;
    const size_t sizes[] = { sizeof(Bus), sizeof(Vdp), sizeof(IoState), offsetof(m68ki_cpu_core, cyc_instruction),
                             SOUND_OUT_MAX };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) h = (h ^ (uint32_t)sizes[i]) * 16777619u;
    return h;
}

static void cpu_state(StateIO *s) {
    /* Everything before the cycle tables is plain data; the tables and callbacks after it
     * are pointers into this process and stay as they are. */
    state_io(s, &m68ki_cpu, offsetof(m68ki_cpu_core, cyc_instruction));
}

static void bus_state(StateIO *s) {
    IO(s, bus.ram); IO(s, bus.zram); IO(s, bus.sram);
    IO(s, bus.sram_mapped); IO(s, bus.z80_busreq); IO(s, bus.z80_reset);
}

static bool state_io_all(StateIO *s) {
    uint32_t magic = STATE_MAGIC, version = STATE_VERSION, layout = layout_fingerprint();
    IO(s, magic); IO(s, version); IO(s, layout);
    if (!s->saving && (magic != STATE_MAGIC || version != STATE_VERSION || layout != layout_fingerprint())) return false;
    cpu_state(s);
    bus_state(s);
    IO(s, vdp);
    IO(s, io);
    z80_state(s);
    sound_state(s);
    machine_state(s);
    return s->ok;
}

size_t state_save(uint8_t *buf, size_t cap) {
    StateIO s = { buf, 0, cap, true, true };
    return state_io_all(&s) ? s.pos : 0;
}

/* Loads a snapshot; on any error the machine is left exactly as it was. */
bool state_load(const uint8_t *buf, size_t len) {
    static uint8_t *backup;
    if (!backup && !(backup = malloc(STATE_MAX))) return false;
    size_t backup_len = state_save(backup, STATE_MAX);
    uint8_t old_sram[SRAM_SIZE];
    memcpy(old_sram, bus.sram, SRAM_SIZE);
    StateIO s = { (uint8_t *)buf, 0, len, false, true };
    if (!state_io_all(&s) || s.pos != len) {
        StateIO r = { backup, 0, backup_len, false, true };
        state_io_all(&r);
        return false;
    }
    if (memcmp(old_sram, bus.sram, SRAM_SIZE)) bus.sram_dirty = true; /* the loaded save RAM goes to disk */
    motion_reset();                     /* the ticks seen before are not this game's past */
    fx_clear();
    return true;
}
