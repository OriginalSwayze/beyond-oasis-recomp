/* Built-in cheats: RAM values held every frame, like a Pro Action Replay, plus one
 * recompiler patch (playerDamage in config/patches.json) for infinite health.
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

void cheats_frame(void) {
    if (strcmp(user_spec, settings.cheat_codes)) {
        memcpy(user_spec, settings.cheat_codes, sizeof user_spec);
        user_count = 0;
        parse(user_spec, user_codes, &user_count, 64);
    }
    bool player = peek16(0x19E8) == 2;
    patch_no_damage = settings.cheat_health;
    if (player && settings.cheat_health) fill(0x1A70, 0x1A72);
    if (player && settings.cheat_spirit) fill(0x1A74, 0x1A76);
    if (player && settings.cheat_weapons) hold_weapons();
    else memset(weapon_id, 0, sizeof weapon_id);
    apply(user_codes, user_count);
    apply(codes, code_count);
}
