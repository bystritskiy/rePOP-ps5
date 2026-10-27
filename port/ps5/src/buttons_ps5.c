/* reSOT PS5, MIT. PlayStation button prompts in the user's own game data.
 *
 * Text names a button as \p5\<code>: a glyph of font 5 (FONTDESC 0x8d007577)
 * in the retail atlas texture 0x0b00450a, 512x256 A8R8G8B8 (read from the
 * disc, 2026-10-27). When the renderer decodes that texture, the glyphs of
 * the DualSense map (port/ps5/README.md) are redrawn in the decoded copy:
 * A/B/X/Y as Cross/Circle/Square/Triangle, White/Black as L1/R1, the
 * triggers as L2/R2, Start as OPTIONS and Back as TOUCH PAD. The GPU builds
 * the mip chain from it. Guest memory and the game files are not changed.
 * The atlas is recognised by the FNV-1a hash of its pixels on the disc. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 512
#define H 256
#define SS 4
#define ATLAS_FNV 0x72f72d83u

typedef struct { uint8_t r, g, b; } Rgb;
static const Rgb DARK = {24, 24, 28}, RIM = {150, 150, 158}, WHITE = {236, 236, 236};

/* FONTDESC rectangles in texels, (left, top, right, bottom), from the disc. */
static const struct { char code; int x0, y0, x1, y1; } rects[] = {
    {'A', 5, 152, 28, 177}, {'B', 5, 118, 30, 142}, {'C', 6, 80, 29, 105},
    {'D', 5, 44, 30, 70}, {'l', 7, 11, 29, 32}, {'r', 43, 10, 66, 32},
    {'L', 42, 103, 62, 124}, {'R', 46, 125, 66, 148},
    {'S', 37, 151, 69, 173}, {'s', 37, 185, 67, 208}};

/* 5x7 capitals for the labels, one row per byte, bit 4 = left column. */
static const struct { char c; uint8_t rows[7]; } capitals[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'C', {15, 16, 16, 16, 16, 16, 15}},
    {'D', {30, 17, 17, 17, 17, 17, 30}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
    {'I', {31, 4, 4, 4, 4, 4, 31}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'N', {17, 25, 21, 19, 17, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}}, {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
    {'2', {14, 17, 1, 2, 4, 8, 31}}};

static double segment(double px, double py, double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay;
    double t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypot(px - ax - t * dx, py - ay - t * dy);
}

/* A dark disc with a light rim and the coloured symbol. */
static int face(char code, int w, int h, double x, double y, Rgb *out)
{
    static const Rgb symbol[4] = {{124, 178, 232}, {255, 102, 102}, {230, 160, 218}, {64, 226, 160}};
    double cx = w / 2.0, cy = h / 2.0, r = (w < h ? w : h) / 2.0 - 0.5;
    double s = r * 0.48, stroke = r * 0.16 > 1.3 ? r * 0.16 : 1.3;
    double d = hypot(x - cx, y - cy);
    int hit;
    if (d > r) return 0;
    if (code == 'A')
        hit = fmin(segment(x, y, cx - s, cy - s, cx + s, cy + s),
                    segment(x, y, cx - s, cy + s, cx + s, cy - s)) <= stroke / 2;
    else if (code == 'B')
        hit = fabs(d - s * 0.95) <= stroke / 2;
    else if (code == 'C')
        hit = fabs(fmax(fabs(x - cx), fabs(y - cy)) - s * 0.82) <= stroke / 2;
    else {
        double px[3] = {cx, cx + s * 0.95, cx - s * 0.95}, py[3] = {cy - s, cy + s * 0.65, cy + s * 0.65};
        double m = 1e9;
        for (int i = 0; i < 3; i++) m = fmin(m, segment(x, y, px[i], py[i], px[(i + 1) % 3], py[(i + 1) % 3]));
        hit = m <= stroke / 2;
    }
    *out = hit ? symbol[code - 'A'] : d > r - 1.2 ? RIM : DARK;
    return 1;
}

static int capital(char c, int row, int col)
{
    for (size_t i = 0; i < sizeof capitals / sizeof capitals[0]; i++)
        if (capitals[i].c == c) return (capitals[i].rows[row] >> (4 - col)) & 1;
    return 0;
}

/* White capitals, optionally on a dark rounded badge; squeezed to fit. */
static int label(const char *const *lines, int count, int badge, int w, int h, double x, double y, Rgb *out)
{
    int widest = 0;
    for (int i = 0; i < count; i++) { int n = (int)strlen(lines[i]) * 6 - 1; if (n > widest) widest = n; }
    double top = (h - (count * 7 + (count - 1) * 2)) / 2.0;
    double sx = (w - 1) / (double)widest < 1 ? (w - 1) / (double)widest : 1;
    for (int i = 0; i < count; i++) {
        int width = (int)strlen(lines[i]) * 6 - 1;
        double left = (w - width * sx) / 2;
        int gx = (int)floor((x - left) / sx), gy = (int)floor(y - (top + i * 9));
        if (gy >= 0 && gy < 7 && gx >= 0 && gx < width && gx % 6 < 5 && capital(lines[i][gx / 6], gy, gx % 6)) {
            *out = WHITE;
            return 1;
        }
    }
    if (badge) {
        double rx = w / 2.0 - 0.5, ry = h / 2.0 - 0.5, rad = 4.0;
        double qx = fmax(fabs(x - w / 2.0) - (rx - rad), 0), qy = fmax(fabs(y - h / 2.0) - (ry - rad), 0);
        double q = hypot(qx, qy);
        if (q <= rad) {
            *out = q > rad - 1.0 || fabs(x - w / 2.0) > rx - 1 || fabs(y - h / 2.0) > ry - 1 ? RIM : DARK;
            return 1;
        }
    }
    return 0;
}

static int shade(char code, int w, int h, double x, double y, Rgb *out)
{
    static const char *const l1[] = {"L1"}, *const r1[] = {"R1"}, *const l2[] = {"L2"}, *const r2[] = {"R2"};
    static const char *const options[] = {"OPTIONS"}, *const touch[] = {"TOUCH", "PAD"};
    switch (code) {
    case 'A': case 'B': case 'C': case 'D': return face(code, w, h, x, y, out);
    case 'l': return label(l1, 1, 1, w, h, x, y, out);
    case 'r': return label(r1, 1, 1, w, h, x, y, out);
    case 'L': return label(l2, 1, 1, w, h, x, y, out);
    case 'R': return label(r2, 1, 1, w, h, x, y, out);
    case 'S': return label(options, 1, 0, w, h, x, y, out);
    default: return label(touch, 2, 0, w, h, x, y, out);
    }
}

static uint32_t fnv(const uint32_t *px)
{
    const uint8_t *b = (const uint8_t *)px;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)W * H * 4; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

void resot_ps5_redraw_buttons(uint32_t *px)
{
    for (size_t g = 0; g < sizeof rects / sizeof rects[0]; g++) {
        int x0 = rects[g].x0, y0 = rects[g].y0, w = rects[g].x1 - x0, h = rects[g].y1 - y0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned r = 0, gr = 0, b = 0, n = 0;
                for (int sy = 0; sy < SS; sy++)
                    for (int sx = 0; sx < SS; sx++) {
                        Rgb c;
                        if (shade(rects[g].code, w, h, x + (sx + 0.5) / SS, y + (sy + 0.5) / SS, &c)) {
                            r += c.r; gr += c.g; b += c.b; n++;
                        }
                    }
                Rgb c = n ? (Rgb){(uint8_t)(r / n), (uint8_t)(gr / n), (uint8_t)(b / n)} : DARK;
                uint32_t a = (uint32_t)lround(255.0 * n / (SS * SS));
                /* Stored upside down inside the rectangle. */
                px[(size_t)(y0 + h - 1 - y) * W + x0 + x] = (a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
            }
    }
}

/* Called by the renderer (nv2a_hw_native.inc) with every decoded 2D texture. */
void resot_ps5_texture_decoded(uint32_t *px, uint32_t width, uint32_t height)
{
    static int reported;
    if (width != W || height != H) return;
    uint32_t hash = fnv(px);
    if (hash == ATLAS_FNV) {
        resot_ps5_redraw_buttons(px);
        if (!reported++) fprintf(stderr, "[BUTTONS] PlayStation glyphs drawn into the button atlas\n");
    }
}
