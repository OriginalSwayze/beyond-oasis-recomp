/* Helpers used by the generated code in generated/recomp_*.c.
 * Generated code operates directly on Musashi's CPU state, so control can pass
 * between recompiled blocks and the interpreter at any instruction boundary.
 * Flags use Musashi's encoding: N/V in bit 7, C/X in bit 8, Z set when FLAG_Z == 0. */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H

#include "m68kcpu.h"
#include "genesis.h"

extern unsigned char m68ki_cycles[][0x10000];
#define CYC(op) USE_CYCLES(m68ki_cycles[0][op])

#define D(n) REG_D[n]
#define BAIL 0x80000000u                /* returned with a PC: interpret this instruction */
#define A(n) REG_A[n]

/* Runs native code starting at pc (a block start or any instruction inside it). */
typedef uint32_t (*RecompBlock)(uint32_t pc);
typedef struct { uint32_t pc; RecompBlock fn; } RecompEntry;

/* Patched instructions (config/patches.json); 0 keeps the original behavior. */
extern uint32_t patch_cull_left, patch_cull_widen;
extern int patch_cam_margin;
extern int patch_no_damage;             /* infinite health cheat (cheats.c) */
extern int patch_attack;                /* attack power cheat: 0 normal, 1 double, 2 quadruple, 3 one-hit kills */
extern int patch_speed;                 /* move speed cheat: the player's movement per tick in percent (100 = normal) */

/* Replaces the code at pc with hand-written C (machine.c); NULL restores the generated code. */
void machine_override(uint32_t pc, RecompBlock fn);

/* ---- Memory (work RAM fast path) ------------------------------------------ */
static inline uint32_t rd8(uint32_t a) {
    a &= 0xFFFFFF;
    return a >= 0xE00000 ? bus.ram[a & 0xFFFF] : bus_read8(a);
}
static inline uint32_t rd16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a >= 0xE00000) { a &= 0xFFFF; return (uint32_t)bus.ram[a] << 8 | bus.ram[a + 1]; }
    return bus_read16(a);
}
static inline uint32_t rd32(uint32_t a) { return rd16(a) << 16 | rd16(a + 2); }
static inline void wr8(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xE00000) bus.ram[a & 0xFFFF] = (uint8_t)v;
    else bus_write8(a, (uint8_t)v);
}
static inline void wr16(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xE00000) { a &= 0xFFFF; bus.ram[a] = (uint8_t)(v >> 8); bus.ram[a + 1] = (uint8_t)v; }
    else bus_write16(a, (uint16_t)v);
}
static inline void wr32(uint32_t a, uint32_t v) { wr16(a, v >> 16); wr16(a + 2, v); }

static inline uint32_t sx8(uint32_t v)  { return (uint32_t)(int32_t)(int8_t)v; }
static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

/* The damage an enemy takes (playerDamage's counterpart, config/patches.json): the game's,
 * or more with the attack power cheat; a one-hit kill takes all the HP left. Capped so the
 * game's signed test on the result still sees it go below zero. */
static inline uint32_t enemy_damage(uint32_t damage, uint32_t hp) {
    switch (patch_attack) {
    case 1: damage *= 2; break;
    case 2: damage *= 4; break;
    case 3: return hp ? hp : damage;
    default: break;
    }
    return damage > 0x7FFF ? 0x7FFF : damage;
}

/* The movement of an object for this tick (16.16), scaled by the move speed cheat when
 * it is the player's (the object at $FF19E8). */
static inline uint32_t player_move(uint32_t object, uint32_t move) {
    if (patch_speed == 100 || (object & 0xFFFFFF) != 0xFF19E8) return move;
    return (uint32_t)(int32_t)((int64_t)(int32_t)move * patch_speed / 100);
}

/* Pixels the camera keeps from each map edge: the widescreen extra width, or less
 * when the map is too narrow to keep that much on both sides. */
static inline uint32_t cam_margin(void) {
    int max = (int16_t)rd16(0xFF177A), m = patch_cam_margin;
    if (m > max / 2) m = max > 0 ? max / 2 : 0;
    return (uint32_t)m;
}

/* ---- Flag helpers, generated per operand size ---------------------------- */
#define SIZED(N, MASK, SIGN)                                                              \
static inline void logic##N(uint32_t r) {                                                 \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r & MASK; FLAG_V = 0; FLAG_C = 0;            \
}                                                                                         \
static inline uint32_t add##N(uint32_t s, uint32_t d) {                                   \
    s &= MASK; d &= MASK;                                                                 \
    uint64_t w = (uint64_t)s + d; uint32_t r = (uint32_t)w & MASK;                        \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r;                                           \
    FLAG_V = ((s ^ r) & (d ^ r) & SIGN) ? 0x80 : 0;                                       \
    FLAG_X = FLAG_C = (w > MASK) ? 0x100 : 0;                                             \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t sub##N(uint32_t s, uint32_t d) {  /* d - s */                      \
    s &= MASK; d &= MASK; uint32_t r = (d - s) & MASK;                                    \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r;                                           \
    FLAG_V = ((s ^ d) & (r ^ d) & SIGN) ? 0x80 : 0;                                       \
    FLAG_X = FLAG_C = (s > d) ? 0x100 : 0;                                                \
    return r;                                                                             \
}                                                                                         \
static inline void cmp##N(uint32_t s, uint32_t d) {                                       \
    uint32_t x = FLAG_X; sub##N(s, d); FLAG_X = x;                                        \
}                                                                                         \
static inline uint32_t addx##N(uint32_t s, uint32_t d) {                                  \
    s &= MASK; d &= MASK; uint32_t x = (FLAG_X >> 8) & 1;                                 \
    uint64_t w = (uint64_t)s + d + x; uint32_t r = (uint32_t)w & MASK;                    \
    FLAG_N = (r & SIGN) ? 0x80 : 0; if (r) FLAG_Z = r;  /* Z only ever cleared */         \
    FLAG_V = ((s ^ r) & (d ^ r) & SIGN) ? 0x80 : 0;                                       \
    FLAG_X = FLAG_C = (w > MASK) ? 0x100 : 0;                                             \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t subx##N(uint32_t s, uint32_t d) {                                  \
    s &= MASK; d &= MASK; uint32_t x = (FLAG_X >> 8) & 1;                                 \
    uint32_t r = (d - s - x) & MASK;                                                      \
    FLAG_N = (r & SIGN) ? 0x80 : 0; if (r) FLAG_Z = r;                                    \
    FLAG_V = ((s ^ d) & (r ^ d) & SIGN) ? 0x80 : 0;                                       \
    FLAG_X = FLAG_C = ((uint64_t)s + x > d) ? 0x100 : 0;                                  \
    return r;                                                                             \
}                                                                                         \
/* Shifts and rotates: count already reduced (register count is mod 64). */               \
static inline uint32_t asl##N(uint32_t d, uint32_t n) {                                   \
    d &= MASK; uint32_t r = d, v = 0, c = 0;                                              \
    for (uint32_t i = 0; i < n; i++) {                                                    \
        c = r & SIGN; r = (r << 1) & MASK;                                                \
        if ((r & SIGN) != (c ? SIGN : 0)) v = 1;                                          \
    }                                                                                     \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r; FLAG_V = v ? 0x80 : 0;                    \
    if (n) FLAG_X = FLAG_C = c ? 0x100 : 0; else FLAG_C = 0;                              \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t asr##N(uint32_t d, uint32_t n) {                                   \
    d &= MASK; uint32_t r = d, c = 0;                                                     \
    for (uint32_t i = 0; i < n; i++) { c = r & 1; r = (r >> 1) | (r & SIGN); }            \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r; FLAG_V = 0;                               \
    if (n) FLAG_X = FLAG_C = c ? 0x100 : 0; else FLAG_C = 0;                              \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t lsl##N(uint32_t d, uint32_t n) {                                   \
    d &= MASK; uint32_t r = d, c = 0;                                                     \
    for (uint32_t i = 0; i < n; i++) { c = r & SIGN; r = (r << 1) & MASK; }               \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r; FLAG_V = 0;                               \
    if (n) FLAG_X = FLAG_C = c ? 0x100 : 0; else FLAG_C = 0;                              \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t lsr##N(uint32_t d, uint32_t n) {                                   \
    d &= MASK; uint32_t r = d, c = 0;                                                     \
    for (uint32_t i = 0; i < n; i++) { c = r & 1; r >>= 1; }                              \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r; FLAG_V = 0;                               \
    if (n) FLAG_X = FLAG_C = c ? 0x100 : 0; else FLAG_C = 0;                              \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t rol##N(uint32_t d, uint32_t n) {                                   \
    d &= MASK; uint32_t r = d, c = 0;                                                     \
    for (uint32_t i = 0; i < n; i++) { c = r & SIGN; r = ((r << 1) | (c ? 1 : 0)) & MASK; } \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r; FLAG_V = 0; FLAG_C = c ? 0x100 : 0;       \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t ror##N(uint32_t d, uint32_t n) {                                   \
    d &= MASK; uint32_t r = d, c = 0;                                                     \
    for (uint32_t i = 0; i < n; i++) { c = r & 1; r = (r >> 1) | (c ? SIGN : 0); }        \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r; FLAG_V = 0; FLAG_C = c ? 0x100 : 0;       \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t roxl##N(uint32_t d, uint32_t n) {                                  \
    d &= MASK; uint32_t r = d, x = (FLAG_X >> 8) & 1;                                     \
    for (uint32_t i = 0; i < n; i++) { uint32_t c = (r & SIGN) ? 1 : 0; r = ((r << 1) | x) & MASK; x = c; } \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r; FLAG_V = 0;                               \
    FLAG_X = FLAG_C = x ? 0x100 : 0;                                                      \
    return r;                                                                             \
}                                                                                         \
static inline uint32_t roxr##N(uint32_t d, uint32_t n) {                                  \
    d &= MASK; uint32_t r = d, x = (FLAG_X >> 8) & 1;                                     \
    for (uint32_t i = 0; i < n; i++) { uint32_t c = r & 1; r = (r >> 1) | (x ? SIGN : 0); x = c; } \
    FLAG_N = (r & SIGN) ? 0x80 : 0; FLAG_Z = r; FLAG_V = 0;                               \
    FLAG_X = FLAG_C = x ? 0x100 : 0;                                                      \
    return r;                                                                             \
}

SIZED(b, 0xFFu, 0x80u)
SIZED(w, 0xFFFFu, 0x8000u)
SIZED(l, 0xFFFFFFFFu, 0x80000000u)

/* Writes the low part of a data register, keeping the rest. */
static inline void setb(uint32_t *r, uint32_t v) { *r = (*r & 0xFFFFFF00u) | (v & 0xFF); }
static inline void setw(uint32_t *r, uint32_t v) { *r = (*r & 0xFFFF0000u) | (v & 0xFFFF); }

/* Data-dependent multiply timing, as Musashi computes it for the 68000. */
static inline int mulu_cycles(uint32_t x) { int c = 0; for (x &= 0xFFFF; x; x >>= 1) if (x & 1) c += 2; return c; }
static inline int muls_cycles(uint32_t x) {
    int c = 0; uint32_t f = 0;
    for (uint32_t y = sx16(x); y; y >>= 1) if ((y & 1) != f) { c += 2; f = 1 - f; }
    return c;
}

static inline void push32(uint32_t v) { REG_SP -= 4; wr32(REG_SP, v); }
static inline uint32_t pop32(void) { uint32_t v = rd32(REG_SP); REG_SP += 4; return v; }

#endif
