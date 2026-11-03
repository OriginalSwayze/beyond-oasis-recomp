/* I/O chip: version register and two 3-button control pads. */
#include "genesis.h"
#include "game.h"

uint16_t pad_state[2];

IoState io;
#define data_reg io.data
#define ctrl_reg io.ctrl

/* Pin levels the pad drives (active low). TH (bit 6) selects which buttons appear. */
static uint8_t pad_pins(int port) {
    uint16_t p = pad_state[port];
    uint8_t pressed;
    if (data_reg[port] & 0x40)
        pressed = (uint8_t)(p & 0x3F);                                          /* C B Right Left Down Up */
    else
        pressed = (uint8_t)((p & 0x03) | 0x0C | (p & PAD_A ? 0x10 : 0) | (p & PAD_START ? 0x20 : 0)); /* Left/Right read low */
    return (uint8_t)(~pressed & 0x3F) | 0x40;
}

uint8_t io_read8(uint32_t a) {
    switch (a & 0x1F) {
    case 0x01:                            /* no expansion unit, no TMSS; the cartridge's region */
        return game->region == REGION_EU ? 0xE0 : game->region == REGION_JP ? 0x20 : 0xA0;
    case 0x03: case 0x05: {
        int port = ((a & 0x1F) - 3) >> 1;
        uint8_t in = pad_pins(port);
        /* Output pins read back what was written; input pins read the pad. */
        return (uint8_t)((data_reg[port] & (ctrl_reg[port] | 0x80)) | (in & ~ctrl_reg[port] & 0x7F));
    }
    case 0x07: return data_reg[2];
    case 0x09: case 0x0B: case 0x0D: return ctrl_reg[((a & 0x1F) - 9) >> 1];
    }
    return 0x00;
}

void io_write8(uint32_t a, uint8_t v) {
    switch (a & 0x1F) {
    case 0x03: case 0x05: case 0x07: data_reg[((a & 0x1F) - 3) >> 1] = v; break;
    case 0x09: case 0x0B: case 0x0D: ctrl_reg[((a & 0x1F) - 9) >> 1] = v; break;
    }
}
