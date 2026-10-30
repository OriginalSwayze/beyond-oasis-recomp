/* Motion from the game's own data. Beyond Oasis builds its sprite list in RAM once per
 * game tick (30 Hz): $FF13CC holds the entries (8 bytes each, as in the VDP's sprite
 * table), $FF188A their count. Objects add their sprites through three routines that
 * first work out the object's screen position (the anchor every piece is placed from):
 *   $AB4A, $AECC, $B0C4   a6 = object, d1.w = x + $80, a4.w = y - height + $80
 * and small effects (8-byte objects) through one that places a single piece:
 *   $A98E                 a6 = object, d2.w = x + 8, d3.w = y + 8
 * The object loop calls each object with a return address pushed, so the returns below
 * mark the end of one object's drawing. The V blank handler sends the finished list to
 * the VDP by DMA ($1FBE) and empties it; that is one tick.
 *
 * Every sprite added after an anchor and before its object's drawing ends belongs to
 * that object. Sprites added any other way (the HUD, menus, text) have no object and
 * never move between ticks. */
#include "motion.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_rt.h"

#define LIST     0x13CC                 /* sprite list in RAM */
#define COUNT    0x188A                 /* entries in it */
#define MAX_JUMP 48                     /* a bigger move in one tick is a jump: no gliding */

typedef struct {
    uint32_t object;                    /* object address */
    uint16_t kind;                      /* the object's first word (its type) when it drew */
    int      nth;                       /* this object's 1st, 2nd... anchor in the tick */
    int16_t  x, y;                      /* screen position the game placed it at */
} Anchor;

typedef struct {
    unsigned serial;                    /* 0 = empty */
    int      count;
    uint8_t  raw[80][8];                /* the entries as sent to the VDP */
    int8_t   anchor[80];                /* anchor of each entry, or -1 */
    Anchor   anchors[80];
    int      anchor_count;
} Tick;

static Tick ring[4];
static unsigned serial;

/* The list being built. */
static int8_t  build_anchor[80];
static Anchor  build_anchors[80];
static int     build_anchor_count;
static int     seen_count;
static bool    built;                   /* entries added since the last copy */
static int     pending = -1;            /* anchor for the sprites about to be added */

static int ram16(uint32_t a) { return bus.ram[a] << 8 | bus.ram[a + 1]; }

static void anchor_at(int16_t x, int16_t y) {
    if (build_anchor_count >= 80) { pending = -1; return; }
    uint32_t object = A(6) & 0xFFFFFF;
    int nth = 0;
    for (int k = 0; k < build_anchor_count; k++) nth += build_anchors[k].object == object;
    Anchor *a = &build_anchors[build_anchor_count];
    *a = (Anchor){ object, (uint16_t)ram16(object & 0xFFFF), nth, x, y };
    pending = build_anchor_count++;
}

static void take_tick(void) {
    Tick *t = &ring[serial % 4];
    int n = seen_count > 80 ? 80 : seen_count;
    t->serial = ++serial;
    t->count = n;
    memcpy(t->raw, bus.ram + LIST, (size_t)n * 8);
    memcpy(t->anchor, build_anchor, sizeof t->anchor);
    memcpy(t->anchors, build_anchors, sizeof t->anchors);
    t->anchor_count = build_anchor_count;
}

/* Called after each native block has run: the anchor registers are still as the block
 * found them (none of these blocks change them), and $A98E adds its piece itself. */
void motion_block(uint32_t pc) {
    switch (pc) {
    case 0xAB4A: case 0xAECC: case 0xB0C4: anchor_at((int16_t)D(1), (int16_t)A(4)); break;
    case 0xA98E: anchor_at((int16_t)D(2), (int16_t)D(3)); break;
    }
    int count = ram16(COUNT);
    if (count != seen_count) {
        if (count < seen_count) {       /* a new list */
            build_anchor_count = 0;
            for (int k = 0; k < 80; k++) build_anchor[k] = -1;
            seen_count = 0;
        }
        for (int k = seen_count; k < count && k < 80; k++) build_anchor[k] = (int8_t)pending;
        if (count > seen_count) built = true;
        seen_count = count;
    }
    switch (pc) {
    /* Back in the object loops: that object's drawing is over. */
    case 0xA1BA: case 0xA1D0: case 0xA1F8: case 0xA258: case 0xA270: case 0xA2A6: pending = -1; break;
    case 0x1FBE:                        /* V blank: about to send the list to the VDP */
        if (built) take_tick();
        built = false;
        build_anchor_count = 0;
        pending = -1;
        break;
    }
}

void motion_reset(void) {
    memset(ring, 0, sizeof ring);
    for (int k = 0; k < 80; k++) build_anchor[k] = -1;
    build_anchor_count = 0;
    seen_count = ram16(COUNT);
    built = false;
    pending = -1;
}

/* The entry as the VDP shows it: y, x and pattern match (the link may differ). */
static bool same_entry(const uint8_t *e, const HdSprite *s) {
    int y = ((e[0] << 8 | e[1]) & 0x3FF) - 128, x = ((e[6] << 8 | e[7]) & 0x1FF) - 128;
    return y == s->y && x == s->x && (e[4] << 8 | e[5]) == s->attr;
}

static const Tick *shown_tick(const HdFrame *f) {
    const Tick *best = NULL;
    for (int r = 0; r < 4; r++) {
        const Tick *t = &ring[r];
        if (!t->serial || (best && t->serial < best->serial)) continue;
        bool all = true;
        for (int i = 0; i < 80 && all; i++)
            if (f->sprites[i].drawn) all = i < t->count && same_entry(t->raw[i], &f->sprites[i]);
        if (all) best = t;
    }
    return best;
}

static const Tick *tick_numbered(unsigned s) {
    const Tick *t = &ring[(s - 1) % 4];
    return s && t->serial == s ? t : NULL;
}

unsigned motion_sprite_moves(const HdFrame *f, int dx[80], int dy[80], int *owned) {
    memset(dx, 0, 80 * sizeof *dx);
    memset(dy, 0, 80 * sizeof *dy);
    if (owned) *owned = 0;
    const Tick *now = shown_tick(f);
    if (!now) return 0;
    const Tick *was = tick_numbered(now->serial - 1);
    for (int i = 0; i < 80; i++) {
        if (!f->sprites[i].drawn || now->anchor[i] < 0) continue;
        if (owned) ++*owned;
        if (!was) continue;
        const Anchor *a = &now->anchors[now->anchor[i]];
        for (int k = 0; k < was->anchor_count; k++) {
            const Anchor *b = &was->anchors[k];
            if (b->object != a->object || b->nth != a->nth || b->kind != a->kind) continue;
            int mx = a->x - b->x, my = a->y - b->y;
            if (abs(mx) <= MAX_JUMP && abs(my) <= MAX_JUMP) { dx[i] = mx; dy[i] = my; }
            break;
        }
    }
    return now->serial;
}
