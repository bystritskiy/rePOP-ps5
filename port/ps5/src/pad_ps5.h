/* reSOT PS5, MIT. Public libScePad data prefix, no proprietary SDK headers. */
#ifndef RESOT_PAD_PS5_H
#define RESOT_PAD_PS5_H
#include <stdint.h>
#include "input/xinput_xbox.h"
typedef struct ResotPadData {
    uint32_t buttons;
    uint8_t lx, ly, rx, ry, l2, r2;
    uint8_t reserved[66];
    uint8_t connected;
    uint8_t tail[179];
} ResotPadData;
void resot_ps5_translate_pad(const ResotPadData *src, XBOX_GAMEPAD *dst);
void resot_ps5_input_shutdown(void);
#endif
