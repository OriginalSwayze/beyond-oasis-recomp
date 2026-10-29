/* Save state slots on disk (savestate.c). */
#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    bool exists;
    time_t saved_at;
    int w, h;                           /* thumbnail size; 0 when there is none */
    uint32_t *thumb;                    /* 0x00RRGGBB */
} SlotInfo;

void savestate_init(const char *base_dir);          /* files go in <base_dir>states/ */
/* Saves the machine to `slot`, with a thumbnail of the frame fb (w x h, pitch in pixels). */
bool savestate_save(int slot, const uint32_t *fb, int w, int h, int pitch);
bool savestate_load(int slot);
bool savestate_info(int slot, SlotInfo *info);      /* false if the slot is empty */
void savestate_info_free(SlotInfo *info);

#endif
