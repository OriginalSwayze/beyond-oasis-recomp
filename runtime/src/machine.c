/* Frame/scanline scheduler, interrupts, and the hand-off between the Musashi
 * interpreter and recompiled or hand-written code. */
#include "genesis.h"
#include "m68k.h"
#include "m68kcpu.h"
#include "recomp_rt.h"
#include "motion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HBLANK_CYCLES 84                /* tail of each line spent in horizontal blank */

static int line_cycle_base;             /* cycles of earlier run() calls this line */
static int seg_done;                    /* cycles of earlier pieces within the current run() */
static int piece_start;                 /* cycle budget the current piece started with */
static uint64_t cpu_clock;              /* 68000 cycles completed before the current piece */

static uint64_t clock_now(void) { return cpu_clock + (uint64_t)(piece_start - m68ki_remaining_cycles); }
static int overrun;                     /* cycles the last run() went past its budget */

/* One bit per even ROM address that the interpreter executed. Written to
 * coverage.bin so the static tracer can learn code it could not find alone. */
uint8_t coverage[ROM_SIZE / 16];

/* Position in the line in real 68000 clocks (overclocked cycles count proportionally less). */
int machine_cycles_in_line(void) {
    return line_cycle_base + (seg_done + piece_start - m68ki_remaining_cycles) * 100 / machine_opts.overclock;
}

/* ---- Recompiled code dispatch ----------------------------------------------
 * recomp_fn[pc / 2] is the native function for a block starting at pc. Hand-written
 * C replacements (the decompilation) register here too and win over generated code. */
extern const RecompEntry *const recomp_tables[];
static RecompBlock recomp_fn[ROM_SIZE / 2];
MachineOptions machine_opts = { .recomp = true, .overclock = 100 };
uint64_t stat_wait_cycles, stat_total_cycles;
uint32_t stat_ticks, stat_late_ticks;

/* The main loop's waits. A game tick runs its logic, waits for the first V blank
 * ($8B72), draws, and waits for the tick countdown ($FF1654) to run out ($8ACA);
 * $8B56/$8B7C wait for the V blank's display work. Blocks may start at the branch too. */
static bool is_wait(uint32_t pc) {
    switch (pc) {
    case 0x8ACA: case 0x8AD0: case 0x8B72: case 0x8B7A: case 0x8B56: case 0x8B5E: case 0x8B7C: case 0x8B84: return true;
    default: return false;
    }
}

/* Called for every native block: counts spare time, ticks, and ticks that arrived at
 * the end-of-tick wait after the countdown had already run out (slowdown). */
static void note_block(uint32_t at, int cycles) {
    static uint32_t last;
    motion_block(at);
    if (is_wait(at)) stat_wait_cycles += (uint64_t)cycles;
    /* A tick arrives from the code just before the wait (not back from an interrupt). */
    if (at == 0x8ACA && last >= 0x8AA6 && last <= 0x8AC4) {
        stat_ticks++;
        if (bus.ram[0x1654] == 0 && bus.ram[0x1655] == 0) stat_late_ticks++;
    }
    last = at;
}
static bool irq_taken;                  /* an interrupt started since the last check */

static void recomp_init(void);
static uint8_t overridden[ROM_SIZE / 16];  /* one bit per even address replaced by machine_override() */

static RecompBlock generated_fn(uint32_t pc) {
    for (int t = 0; recomp_tables[t]; t++)
        for (const RecompEntry *e = recomp_tables[t]; e->fn; e++) if (e->pc == pc) return e->fn;
    return NULL;
}

/* Replaces the code at pc with hand-written C; NULL restores the generated code. */
void machine_override(uint32_t pc, RecompBlock fn) {
    recomp_init();
    recomp_fn[pc >> 1] = fn ? fn : generated_fn(pc);
    uint8_t bit = (uint8_t)(1u << ((pc >> 1) & 7));
    if (fn) overridden[pc >> 4] |= bit;
    else overridden[pc >> 4] &= (uint8_t)~bit;
}

static void recomp_init(void) {
    static bool done;
    if (done) return;
    done = true;
    for (int t = 0; recomp_tables[t]; t++)
        for (const RecompEntry *e = recomp_tables[t]; e->fn; e++) recomp_fn[e->pc >> 1] = e->fn;
}

int gen_int_ack(int level) {
    irq_taken = true;
    if (level == 6) vdp.vint_pending = false;
    else if (level == 4) vdp.hint_pending = false;
    /* Update the line without m68k_set_irq(): that would re-enter interrupt processing. */
    m68ki_cpu.int_level = (unsigned)vdp_irq_level() << 8;
    return M68K_INT_ACK_AUTOVECTOR;
}

/* Optional per-PC execution counts for debugging (--pc-profile). */
uint32_t *pc_profile;

/* ---- Verification: every recompiled block is checked against the interpreter.
 * The block runs natively, its result is saved, the machine state is rolled back,
 * and the interpreter runs the same code; the two results must match. */
typedef struct {
    m68ki_cpu_core cpu;
    int cycles;
    uint8_t ram[0x10000], zram[0x2000];
    bool zbusreq, zreset;
    uint64_t clock;
    Vdp vdp;
    IoState io;
} Snapshot;

static Snapshot *snap_before, *snap_native;
static bool verify_pending;
static uint32_t verify_start, verify_exit;
static int verify_steps;
uint32_t verify_checked, verify_failed;
uint64_t stat_native_cycles, stat_interp_cycles; /* where 68000 time was spent */

static void snap_take(Snapshot *s) {
    s->cpu = m68ki_cpu; s->cycles = m68ki_remaining_cycles; s->clock = clock_now();
    memcpy(s->ram, bus.ram, sizeof s->ram); memcpy(s->zram, bus.zram, sizeof s->zram);
    s->zbusreq = bus.z80_busreq; s->zreset = bus.z80_reset;
    s->vdp = vdp;
    s->io = io;
}
static void snap_restore(const Snapshot *s) {
    m68ki_cpu = s->cpu; m68ki_remaining_cycles = s->cycles;
    memcpy(bus.ram, s->ram, sizeof s->ram); memcpy(bus.zram, s->zram, sizeof s->zram);
    bus.z80_busreq = s->zbusreq; bus.z80_reset = s->zreset;
    vdp = s->vdp;
    io = s->io;
}

static void verify_report(const char *what, uint32_t a, uint32_t b) {
    static uint8_t reported[ROM_SIZE / 2];
    verify_failed++;
    if (reported[verify_start >> 1]) return;
    reported[verify_start >> 1] = 1;
    fprintf(stderr, "MISMATCH block $%06X: %s native=%08X interpreter=%08X\n", verify_start, what, a, b);
}

static void verify_compare(uint32_t pc) {
    const m68ki_cpu_core *n = &snap_native->cpu;
    verify_checked++;
    if (pc != verify_exit) { verify_report("exit pc", verify_exit, pc); return; }
    int native_used = snap_before->cycles - snap_native->cycles;
    int interp_used = (int)(clock_now() - snap_before->clock);
    if (native_used != interp_used) { verify_report("cycles", (uint32_t)native_used, (uint32_t)interp_used); return; }
    for (int r = 0; r < 16; r++)
        if (n->dar[r] != m68ki_cpu.dar[r]) { verify_report(r < 8 ? "d reg" : "a reg", n->dar[r], m68ki_cpu.dar[r]); return; }
#define FL(f, m) if (((n->f) & (m)) != ((m68ki_cpu.f) & (m))) { verify_report(#f, n->f, m68ki_cpu.f); return; }
    FL(x_flag, 0x100) FL(n_flag, 0x80) FL(v_flag, 0x80) FL(c_flag, 0x100)
#undef FL
    if (!n->not_z_flag != !m68ki_cpu.not_z_flag) { verify_report("z flag", n->not_z_flag, m68ki_cpu.not_z_flag); return; }
    for (uint32_t i = 0; i < sizeof bus.ram; i++)
        if (snap_native->ram[i] != bus.ram[i]) { verify_report("ram byte", 0xFF0000 + i, (uint32_t)snap_native->ram[i] << 8 | bus.ram[i]); return; }
    if (memcmp(snap_native->vdp.vram, vdp.vram, sizeof vdp.vram) || memcmp(snap_native->vdp.cram, vdp.cram, sizeof vdp.cram) ||
        memcmp(snap_native->vdp.vsram, vdp.vsram, sizeof vdp.vsram) || memcmp(snap_native->vdp.reg, vdp.reg, sizeof vdp.reg))
        verify_report("vdp state", 0, 0);
}

static void verify_hook(uint32_t pc) {
    if (verify_pending) {
        if (irq_taken || ++verify_steps > 256) verify_pending = false; /* interrupted: nothing to compare */
        else if (pc == verify_exit) { verify_pending = false; verify_compare(pc); }
        else return;
    }
    RecompBlock fn = pc < ROM_SIZE ? recomp_fn[pc >> 1] : NULL;
    if (!fn || (overridden[pc >> 4] >> ((pc >> 1) & 7) & 1)) return; /* overrides change behavior on purpose */
    if (!snap_before) { snap_before = malloc(sizeof *snap_before); snap_native = malloc(sizeof *snap_native); }
    snap_take(snap_before);
    irq_taken = false;
    uint32_t ret = fn(pc), exit = ret & 0xFFFFFF;
    bool usable = !(ret & BAIL) && !irq_taken; /* no bail, and no interrupt taken by an SR change */
    if (usable) snap_take(snap_native);
    snap_restore(snap_before);
    if (!usable) return;
    verify_pending = true;
    verify_start = pc;
    verify_exit = exit;
    verify_steps = 0;
    irq_taken = false;
}

void gen_instr_hook(unsigned int pc) {
    if (pc < ROM_SIZE) {
        coverage[pc >> 4] |= (uint8_t)(1u << ((pc >> 1) & 7));
        if (pc_profile) pc_profile[pc >> 1]++;
    }
    if (machine_opts.verify) verify_hook(pc);
}

void machine_reset(void) {
    vdp_reset();
    memset(bus.ram, 0, sizeof bus.ram);
    bus.sram_mapped = false;
    bus.z80_reset = true;               /* the Z80 is held in reset at power on */
    bus.z80_busreq = false;
    z80_reset_cpu();
    sound_reset();
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    /* Musashi charges its reset time on the next execute() call; take it now so the
     * native path (which may not call execute() for a while) keeps identical timing. */
    m68k_execute(1);
    overrun = 0;
    recomp_init();
}

/* Scheduler counters (snapshots are taken between frames, so the per-line ones are idle). */
void machine_state(StateIO *s) {
    state_io(s, &cpu_clock, sizeof cpu_clock);
    state_io(s, &overrun, sizeof overrun);
}

/* Runs the 68000 for about `cycles` clocks. Native blocks and the interpreter
 * take turns: native code runs while there is a block for REG_PC; otherwise the
 * interpreter executes single instructions until native code is available again.
 * Blocks stop after any instruction that exhausts the budget, exactly where the
 * interpreter would, so both paths produce identical timing. */
static void run(int cycles) {
    /* Overclocked, the 68000 gets more cycles in the same time; the Z80, sound and the
     * line position below still advance by the real amount. */
    int cpu_cycles = machine_opts.overclock == 100 ? cycles : cycles * machine_opts.overclock / 100;
    int budget = cpu_cycles - overrun;
    seg_done = 0;
    if (budget > 0) {
        m68k_set_irq(vdp_irq_level());
        bool native = machine_opts.recomp && !machine_opts.verify;
        if (native) {
            /* m68k_set_irq() only records the level; execute() takes pending
             * interrupts on entry, so the native path does the same here. */
            piece_start = m68ki_remaining_cycles = budget;
            m68ki_check_interrupts();
            seg_done = piece_start - m68ki_remaining_cycles;
            cpu_clock += (uint64_t)seg_done;
            piece_start = m68ki_remaining_cycles = 0;
        }
        if (!native) {
            piece_start = budget;
            seg_done = m68k_execute(budget);
            cpu_clock += (uint64_t)seg_done;
            piece_start = m68ki_remaining_cycles = 0;
        }
        while (native && seg_done < budget) {
            uint32_t pc = REG_PC & 0xFFFFFF;
            RecompBlock fn = pc < ROM_SIZE && !m68ki_cpu.stopped ? recomp_fn[pc >> 1] : NULL;
            if (fn) {
                piece_start = m68ki_remaining_cycles = budget - seg_done;
                uint32_t ret = 0;
                while (fn && m68ki_remaining_cycles > 0) {
                    int before = m68ki_remaining_cycles;
                    uint32_t at = REG_PC & 0xFFFFFF;
                    ret = fn(at);
                    note_block(at, before - m68ki_remaining_cycles);
                    if (pc_profile) pc_profile[at >> 1] += (uint32_t)(before - m68ki_remaining_cycles);   /* cycles per block */
                    REG_PC = ret & 0xFFFFFF;
                    if (ret & BAIL) break;
                    fn = REG_PC < ROM_SIZE ? recomp_fn[REG_PC >> 1] : NULL;
                }
                seg_done += piece_start - m68ki_remaining_cycles;
                stat_native_cycles += (uint64_t)(piece_start - m68ki_remaining_cycles);
                cpu_clock += (uint64_t)(piece_start - m68ki_remaining_cycles);
                piece_start = m68ki_remaining_cycles = 0;
                if (!(ret & BAIL)) continue;
            }
            /* One interpreted instruction (also takes pending interrupts and handles STOP). */
            piece_start = 1;
            int used = m68k_execute(1);
            seg_done += used;
            stat_interp_cycles += (uint64_t)used;
            cpu_clock += (uint64_t)used;
            piece_start = m68ki_remaining_cycles = 0;
        }
        overrun = seg_done - budget;
        stat_total_cycles += (uint64_t)seg_done;
    } else {
        overrun -= cpu_cycles;
    }
    seg_done = 0;
    line_cycle_base += cycles;
    z80_run(cycles);
    sound_advance(cycles);
}

void machine_run_frame(uint32_t *fb, int pitch) {
    sound_out_count = 0;
    if (vdp_opts.extra_width) widescreen_frame();
    int height = vdp_screen_height();
    for (int line = 0; line < LINES_NTSC; line++) {
        vdp.line = line;
        line_cycle_base = 0;
        if (line < height && fb) vdp_render_line(line, fb + (size_t)line * pitch);
        if (line == height) { vdp.vint_pending = true; z80_set_irq(true); }
        if (line == height + 1) z80_set_irq(false);

        run(CYCLES_PER_LINE - HBLANK_CYCLES);

        /* H interrupt counter: counts down through the active lines, reloads in V blank. */
        if (line <= height) {
            if (--vdp.hint_counter < 0) {
                vdp.hint_counter = vdp.reg[10];
                vdp.hint_pending = true;
            }
        } else {
            vdp.hint_counter = vdp.reg[10];
        }
        run(HBLANK_CYCLES);
    }
    sound_end_frame();
}
