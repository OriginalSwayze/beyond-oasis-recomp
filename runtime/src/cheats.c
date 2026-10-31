/* Built-in cheats: RAM values held every frame, like a Pro Action Replay, plus recompiler
 * patches (config/patches.json): playerDamage for infinite health, hitDamage for the
 * attack power (double, quadruple, one-hit kills), playerSpeed for the move speed.
 *
 * RAM used (from the disassembly):
 *   $FF19E8         player object; word +$00 is 2 while the player exists
 *   $FF1A70/$FF1A72 max / current health  (player +$88/+$8A)
 *   $FF1A74/$FF1A76 max / current spirit  (player +$8C/+$8E; max is 0 before the first spirit)
 *   $FF0D80         8 weapon slots of (id, uses); uses $FF = unbreakable (the knife) */
#include "cheats.h"

#include <stdlib.h>
#include <string.h>

#include "genesis.h"
#include "settings.h"

int patch_no_damage;
int patch_attack;
int patch_speed = 100;
int patch_random;

typedef struct { uint16_t addr; uint16_t value; bool word; } Code;

static Code codes[64];                  /* --poke codes */
static int code_count;
static Code user_codes[64];             /* settings.cheat_codes */
static int user_count;
static char user_spec[sizeof settings.cheat_codes];
static uint8_t weapon_id[8], weapon_uses[8];

static int parse(const char *spec, Code *out, int *count, int cap) {
    int added = 0;
    while (*spec && *count < cap) {
        char *end;
        unsigned long a = strtoul(spec, &end, 16);
        if (*end != ':') break;
        const char *v = end + 1;
        unsigned long val = strtoul(v, &end, 16);
        if (end == v) break;
        out[(*count)++] = (Code){ (uint16_t)(a & 0xFFFF), (uint16_t)val, end - v > 2 };
        added++;
        while (*end == ' ') end++;
        spec = *end == ',' ? end + 1 : end;
        while (*spec == ' ') spec++;
    }
    return added;
}

int cheats_add_codes(const char *spec) { return parse(spec, codes, &code_count, 64); }
void cheats_clear_codes(void) { code_count = 0; }

static uint16_t peek16(uint16_t a) { return (uint16_t)(bus.ram[a] << 8 | bus.ram[(uint16_t)(a + 1)]); }
static void poke16(uint16_t a, uint16_t v) { bus.ram[a] = (uint8_t)(v >> 8); bus.ram[(uint16_t)(a + 1)] = (uint8_t)v; }

static void apply(const Code *c, int n) {
    for (int i = 0; i < n; i++) {
        if (c[i].word) poke16(c[i].addr, c[i].value);
        else bus.ram[c[i].addr] = (uint8_t)c[i].value;
    }
}

/* Hold a current value at its max, when the max looks like a real one. */
static void fill(uint16_t max_addr, uint16_t cur_addr) {
    uint16_t max = peek16(max_addr);
    if (max > 0 && max < 1000 && peek16(cur_addr) < max) poke16(cur_addr, max);
}

/* Weapons lose a use per hit (sub at $8530) and vanish at 0. Restore the uses they had,
 * so turning the cheat off leaves the real counts; a weapon on its last use gets 2. */
static void hold_weapons(void) {
    for (int k = 0; k < 8; k++) {
        uint8_t id = bus.ram[0x0D80 + 2 * k], uses = bus.ram[0x0D81 + 2 * k];
        bool breakable = id >= 2 && id <= 0x0C && uses != 0xFF;
        if (breakable && id == weapon_id[k] && uses < weapon_uses[k]) bus.ram[0x0D81 + 2 * k] = uses = weapon_uses[k];
        if (breakable && uses == 1) bus.ram[0x0D81 + 2 * k] = uses = 2;
        weapon_id[k] = id;
        weapon_uses[k] = uses;
    }
}

/* ---- Randomizer -------------------------------------------------------------------------- *
 * Item IDs (the game's own names): $01-$0C weapons (Knife, Sword, Broad, Death, Omega, Bow,
 * Fire, Metal, Bomb, Power, Hyper, Atm Bow), $0D-$10 keys, $11 HP Up, $12-$15 the spirit
 * stones, $16-$19 lasting upgrades, $1A-$1D the spirit calls, $1E-$29 the restoratives
 * (HP, SP, HP+SP: 1/4, 1/2, 3/4, Max), $2A Elixir, $31 L. Cube. Only restoratives (and, at
 * the higher setting, weapons) are swapped, each for one of its own kind: the keys, upgrades,
 * stones, calls, the Knife Ali starts with and the Bomb (which the way on may need) never
 * change, so the game can always be finished. */
static const uint8_t restoratives[] = { 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A };
static const uint8_t weapons[] = { 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0B, 0x0C };

static bool in(const uint8_t *set, size_t n, uint32_t id) { for (size_t i = 0; i < n; i++) if (set[i] == id) return true; return false; }

uint32_t random_drop_swap(uint32_t item, uint32_t place) {
    item &= 0xFFFF;
    uint32_t id = item >> 8;
    if (item >= 0x8000 || !id) return item;           /* the one-time table, or nothing */
    const uint8_t *set;
    size_t n;
    if (in(restoratives, sizeof restoratives, id)) { set = restoratives; n = sizeof restoratives; }
    else if (patch_random >= 2 && in(weapons, sizeof weapons, id)) { set = weapons; n = sizeof weapons; }
    else return item;
    /* The same seed, area, place and item always give the same: a chest keeps its prize. */
    uint32_t h = (uint32_t)settings.randomizer_seed * 2654435761u;
    for (uint16_t a = 0x16F6; a < 0x16FE; a++) h = (h ^ bus.ram[a]) * 16777619u;
    h = (h ^ place) * 16777619u;
    h = (h ^ item) * 16777619u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return (uint32_t)set[h % n] << 8 | (item & 0xFF);
}

void cheats_frame(void) {
    if (strcmp(user_spec, settings.cheat_codes)) {
        memcpy(user_spec, settings.cheat_codes, sizeof user_spec);
        user_count = 0;
        parse(user_spec, user_codes, &user_count, 64);
    }
    bool player = peek16(0x19E8) == 2;
    patch_no_damage = settings.cheat_health;
    patch_attack = settings.cheat_attack;
    patch_random = settings.randomizer;
    static const int speeds[3] = { 100, 150, 200 };
    patch_speed = speeds[settings.cheat_speed >= 0 && settings.cheat_speed < 3 ? settings.cheat_speed : 0];
    if (player && settings.cheat_health) fill(0x1A70, 0x1A72);
    if (player && settings.cheat_spirit) fill(0x1A74, 0x1A76);
    if (player && settings.cheat_weapons) hold_weapons();
    else memset(weapon_id, 0, sizeof weapon_id);
    apply(user_codes, user_count);
    apply(codes, code_count);
}
