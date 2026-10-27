/* reSOT PS5, MIT. The Xbox's Black/White buttons use R1/L1. */
#include "pad_ps5.h"
#include <string.h>
#include <stddef.h>
_Static_assert(offsetof(ResotPadData, connected) == 76, "libScePad connected ABI");
static int16_t axis(uint8_t value, int invert)
{
    int d = (int)value - 128;
    if (d >= -8 && d <= 8) return 0;
    int magnitude = d > 0 ? (d - 8) * 32767 / 119 : (d + 8) * 32768 / 120;
    if (invert) magnitude = -magnitude;
    if (magnitude > 32767) magnitude = 32767;
    if (magnitude < -32768) magnitude = -32768;
    return (int16_t)magnitude;
}
void resot_ps5_translate_pad(const ResotPadData *s, XBOX_GAMEPAD *d)
{
    memset(d, 0, sizeof(*d));
    if (!s || !s->connected) return;
    static const uint32_t ps[] = {0x10,0x40,0x80,0x20,0x08,0x100000,0x02,0x04};
    static const uint16_t xb[] = {XBOX_GAMEPAD_DPAD_UP,XBOX_GAMEPAD_DPAD_DOWN,
        XBOX_GAMEPAD_DPAD_LEFT,XBOX_GAMEPAD_DPAD_RIGHT,XBOX_GAMEPAD_START,
        XBOX_GAMEPAD_BACK,XBOX_GAMEPAD_LEFT_THUMB,XBOX_GAMEPAD_RIGHT_THUMB};
    for (unsigned i = 0; i < sizeof(ps)/sizeof(*ps); ++i)
        if (s->buttons & ps[i]) d->wButtons |= xb[i];
    static const uint32_t face[] = {0x4000,0x2000,0x8000,0x1000,0x800,0x400};
    for (unsigned i = 0; i < sizeof(face)/sizeof(*face); ++i)
        d->bAnalogButtons[i] = (s->buttons & face[i]) ? 255 : 0;
    d->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = s->l2;
    d->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = s->r2;
    d->sThumbLX = axis(s->lx, 0); d->sThumbLY = axis(s->ly, 1);
    d->sThumbRX = axis(s->rx, 0); d->sThumbRY = axis(s->ry, 1);
}
