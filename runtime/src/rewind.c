/* Rewind: a snapshot every frame, kept as the newest full snapshot plus a stack of
 * backward deltas. Entry n turns snapshot n back into snapshot n-1: the XOR of the two,
 * run-length encoded (consecutive frames differ in a few KB out of ~160 KB), and the
 * pad input frame n-1 was played with, so stepping back can redraw that frame exactly.
 * The oldest entries are dropped to stay within the memory budget. */
#include "genesis.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;                      /* encoded delta */
    uint32_t len;
    uint16_t pad;                       /* pad_state[0] of the older frame */
} Entry;

#define MAX_ENTRIES 65536               /* about 18 minutes at 60 fps; the byte budget binds first */

static Entry ring[MAX_ENTRIES];
static int first, count;                /* oldest entry, number of entries */
static size_t used, budget;
static uint8_t *head, *scratch;         /* newest snapshot, workspace */
static size_t head_len;
static uint16_t head_pad;               /* pad input of the frame that starts at head */
static bool resumed;                    /* head is the machine's state after stepping back */

static void put_varint(uint8_t **o, uint32_t v) {
    while (v >= 0x80) { *(*o)++ = (uint8_t)(v | 0x80); v >>= 7; }
    *(*o)++ = (uint8_t)v;
}

static uint32_t get_varint(const uint8_t **p) {
    uint32_t v = 0;
    for (int shift = 0;; shift += 7) {
        uint8_t b = *(*p)++;
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
    }
}

/* Encodes a ^ b as (zero run, literal run, literal bytes) groups. Returns the length. */
static size_t encode(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *out) {
    uint8_t *o = out;
    size_t i = 0;
    while (i < n) {
        size_t z = i;
        while (z < n && a[z] == b[z]) z++;
        size_t lit = z;
        /* A literal run ends at 8 equal bytes in a row (or the end). */
        for (size_t same = 0; lit < n; lit++) {
            if (a[lit] == b[lit]) { if (++same == 8) { lit -= 7; break; } }
            else same = 0;
        }
        if (lit > n) lit = n;
        put_varint(&o, (uint32_t)(z - i));
        put_varint(&o, (uint32_t)(lit - z));
        for (size_t k = z; k < lit; k++) *o++ = a[k] ^ b[k];
        i = lit;
    }
    return (size_t)(o - out);
}

static void decode_into(uint8_t *dst, size_t n, const uint8_t *p, size_t len) {
    const uint8_t *end = p + len;
    size_t i = 0;
    while (p < end && i <= n) {
        i += get_varint(&p);
        uint32_t lit = get_varint(&p);
        for (uint32_t k = 0; k < lit && i < n; k++) dst[i++] ^= *p++;
    }
}

static void drop_oldest(void) {
    Entry *e = &ring[first];
    used -= e->len;
    free(e->data);
    e->data = NULL;
    first = (first + 1) % MAX_ENTRIES;
    count--;
}

void rewind_reset(size_t budget_bytes) {
    while (count) drop_oldest();
    budget = budget_bytes;
    head_len = 0;
    resumed = false;
    if (!head) head = malloc(STATE_MAX);
    if (!scratch) scratch = malloc(STATE_MAX * 2);
}

/* Call once per frame, before running it, with the pad input it will use. */
void rewind_push(uint16_t pad) {
    if (!budget || !head || !scratch) return;
    uint8_t *cur = scratch;
    size_t len = state_save(cur, STATE_MAX);
    if (!len) return;
    if (resumed || !head_len || len != head_len) {
        /* First frame, or continuing from a frame reached by stepping back: just record. */
        if (len != head_len) while (count) drop_oldest();
        resumed = false;
    } else {
        uint8_t *enc = scratch + STATE_MAX;
        size_t n = encode(head, cur, len, enc);
        Entry e = { malloc(n ? n : 1), (uint32_t)n, head_pad };
        if (e.data) {
            memcpy(e.data, enc, n);
            if (count == MAX_ENTRIES) drop_oldest();
            ring[(first + count) % MAX_ENTRIES] = e;
            count++;
            used += n;
            while (used > budget && count > 1) drop_oldest();
        }
    }
    memcpy(head, cur, len);
    head_len = len;
    head_pad = pad;
}

/* Steps back one frame: the machine is put in the state before that frame, and *pad is
 * the input it was played with (for redrawing it). False when there is nothing left. */
bool rewind_step(uint16_t *pad) {
    if (!count) return false;
    int last = (first + count - 1) % MAX_ENTRIES;
    Entry *e = &ring[last];
    decode_into(head, head_len, e->data, e->len);
    head_pad = e->pad;
    used -= e->len;
    free(e->data);
    e->data = NULL;
    count--;
    resumed = true;
    *pad = head_pad;
    return state_load(head, head_len);
}

/* Puts the machine back at the current rewind position (after redrawing a frame). */
void rewind_restore_head(void) {
    if (head_len) state_load(head, head_len);
}

int rewind_frames(void) { return count; }
size_t rewind_bytes(void) { return used; }
