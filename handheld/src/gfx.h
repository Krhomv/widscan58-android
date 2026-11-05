// A small RGB565 canvas: rectangles, lines and anti-aliased text, drawn into
// a frame buffer that display.cpp sends to the panel. Plain C++ so the host
// test can render the same frames. GPL-3.0-or-later.
#pragma once
#include <stdint.h>
#include "fonts.h"

static inline constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

/** a over b, a's weight alpha / 255. */
uint16_t mix(uint16_t a, uint16_t b, uint8_t alpha);

enum Align { LEFT, CENTER, RIGHT };

class Canvas {
public:
    Canvas(uint16_t *px, int w, int h) : px(px), w(w), h(h) {}
    uint16_t *px;
    const int w, h;

    void fill(int x, int y, int fw, int fh, uint16_t c);
    void blendFill(int x, int y, int fw, int fh, uint16_t c, uint8_t alpha);
    void hline(int x, int y, int len, uint16_t c) { fill(x, y, len, 1, c); }
    void vline(int x, int y, int len, uint16_t c) { fill(x, y, 1, len, c); }
    /** Vertical span between y0 and y1 inclusive, in either order. */
    void span(int x, int y0, int y1, uint16_t c);
    void dashedV(int x, int y0, int y1, uint16_t c, int on, int off);
    void roundRect(int x, int y, int rw, int rh, int r, uint16_t c);
    void roundFrame(int x, int y, int rw, int rh, int r, uint16_t c);
    void pixel(int x, int y, uint16_t c) {
        if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = c;
    }
    void blendPixel(int x, int y, uint16_t c, uint8_t alpha);
    void circle(int cx, int cy, int r, uint16_t c);

    /** Draws UTF-8 text with its baseline at y; returns the advance width. */
    int text(const Font &f, int x, int y, const char *s, uint16_t c, Align a = LEFT);
    static int textWidth(const Font &f, const char *s);
};
