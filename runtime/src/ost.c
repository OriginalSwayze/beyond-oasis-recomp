/* The sound driver lives at $060000: `jsr $060000` once a frame (from the game's V blank)
 * runs the music, `jsr $060004` with a command in d0 starts a song or sound effect.
 *
 * ost_start() parks the game: the 68000 loops on a `bra.s *` in work RAM with interrupts
 * masked, so the game's code never runs again and nothing else touches the Z80 or the
 * sound chips (the game running its attract demo underneath could leave the Z80 where the
 * driver waits for it forever). Each driver call is then made from outside: the CPU is
 * pointed at the driver with a return address in the cartridge header (never executed by
 * the game), where hand-written code starts the next queued call or goes back to the loop. */
#include "ost.h"

#include "recomp_rt.h"

#define DRIVER_UPDATE 0x060000
#define DRIVER_CMD    0x060004
#define RETURN_HERE   0x0001FC          /* in the cartridge header */
#define PARK          0xFFF000          /* the idle loop, in game RAM the driver does not use */
#define PARK_SP       0xFFFE00

typedef struct { uint32_t entry; uint16_t d0; } Call;

static Call queue[16];
static int queued;
static bool running, started;

static void start_next(void) {
    Call c = queue[0];
    queued--;
    for (int k = 0; k < queued; k++) queue[k] = queue[k + 1];
    push32(RETURN_HERE);
    D(0) = c.d0;
    REG_PC = c.entry;
    running = true;
}

static uint32_t back_from_driver(uint32_t pc) {
    (void)pc;
    USE_CYCLES(8);
    running = false;
    if (queued) start_next();
    else REG_PC = PARK;
    return REG_PC;
}

void ost_start(void) {
    machine_override(RETURN_HERE, back_from_driver);
    bus.ram[PARK & 0xFFFF] = 0x60;      /* bra.s * */
    bus.ram[(PARK & 0xFFFF) + 1] = 0xFE;
    m68ki_set_sr(0x2700);
    REG_SP = PARK_SP;
    REG_PC = PARK;
    m68ki_cpu.stopped = 0;
    queued = 0;
    running = false;
    started = true;
}

void ost_end(void) {
    if (!started) return;
    machine_override(RETURN_HERE, NULL);
    started = false;
}

static bool enqueue(uint32_t entry, uint16_t d0) {
    if (!started || queued >= (int)(sizeof queue / sizeof *queue)) return false;
    queue[queued++] = (Call){ entry, d0 };
    return true;
}

bool ost_command(uint16_t cmd) { return enqueue(DRIVER_CMD, cmd); }

void ost_frame(void) {
    enqueue(DRIVER_UPDATE, 0);
    if (!running && queued) start_next();
}
