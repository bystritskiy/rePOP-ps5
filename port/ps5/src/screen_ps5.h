/* reSOT PS5, MIT. Static CPU text screens (src/video_ps5.c). */
#ifndef RESOT_SCREEN_PS5_H
#define RESOT_SCREEN_PS5_H
#include <stdint.h>
typedef struct ResotScreenLine {
    unsigned y, scale;   /* top pixel row; 5x7 glyphs drawn scale times larger */
    uint32_t color;      /* 0xAARRGGBB */
    const char *text;
} ResotScreenLine;
/* Opens VideoOut and shows the lines on a 1920x1080 screen until exit;
 * returns the first failing call's code or the flip's. Not for use once the
 * game renderer owns VideoOut. */
int resot_ps5_text_screen(const ResotScreenLine *lines, unsigned count);
#endif
