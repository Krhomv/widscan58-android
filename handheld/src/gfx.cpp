// GPL-3.0-or-later.
#include "gfx.h"

uint16_t mix(uint16_t a, uint16_t b, uint8_t alpha) {
    if (alpha == 255) return a;
    if (alpha == 0) return b;
    uint32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    uint32_t br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    uint32_t r = (ar * alpha + br * (255 - alpha) + 127) / 255;
    uint32_t g = (ag * alpha + bg * (255 - alpha) + 127) / 255;
    uint32_t bl = (ab * alpha + bb * (255 - alpha) + 127) / 255;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

void Canvas::fill(int x, int y, int fw, int fh, uint16_t c) {
    if (x < 0) fw += x, x = 0;
    if (y < 0) fh += y, y = 0;
    if (x + fw > w) fw = w - x;
    if (y + fh > h) fh = h - y;
    if (fw <= 0 || fh <= 0) return;
    for (int j = 0; j < fh; j++) {
        uint16_t *p = px + (y + j) * w + x;
        for (int i = 0; i < fw; i++) p[i] = c;
    }
}

void Canvas::blendFill(int x, int y, int fw, int fh, uint16_t c, uint8_t alpha) {
    if (x < 0) fw += x, x = 0;
    if (y < 0) fh += y, y = 0;
    if (x + fw > w) fw = w - x;
    if (y + fh > h) fh = h - y;
    for (int j = 0; j < fh; j++) {
        uint16_t *p = px + (y + j) * w + x;
        for (int i = 0; i < fw; i++) p[i] = mix(c, p[i], alpha);
    }
}

void Canvas::span(int x, int y0, int y1, uint16_t c) {
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    fill(x, y0, 1, y1 - y0 + 1, c);
}

void Canvas::dashedV(int x, int y0, int y1, uint16_t c, int on, int off) {
    for (int y = y0; y <= y1; y += on + off) fill(x, y, 1, (y + on - 1 > y1 ? y1 - y + 1 : on), c);
}

void Canvas::blendPixel(int x, int y, uint16_t c, uint8_t alpha) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = mix(c, px[y * w + x], alpha);
}

// Rounded corners: per corner row, the inset and the coverage of the edge pixel.
static void cornerRow(int r, int j, int *inset, uint8_t *edge) {
    // Distance from the corner circle's centre to this row's middle.
    float dy = r - j - 0.5f;
    float dx = 0;
    {
        float d2 = (float)r * r - dy * dy;
        dx = d2 > 0 ? __builtin_sqrtf(d2) : 0;
    }
    float in = r - dx;  // first covered x within the corner
    int i = (int)in;
    *inset = i;
    *edge = (uint8_t)(255 * (1.0f - (in - i)));
}

void Canvas::roundRect(int x, int y, int rw, int rh, int r, uint16_t c) {
    if (r * 2 > rh) r = rh / 2;
    if (r * 2 > rw) r = rw / 2;
    for (int j = 0; j < rh; j++) {
        int inset = 0;
        uint8_t edge = 255;
        if (j < r) cornerRow(r, j, &inset, &edge);
        else if (j >= rh - r) cornerRow(r, rh - 1 - j, &inset, &edge);
        if (edge < 255) {
            blendPixel(x + inset, y + j, c, edge);
            blendPixel(x + rw - 1 - inset, y + j, c, edge);
            fill(x + inset + 1, y + j, rw - 2 * inset - 2, 1, c);
        } else {
            fill(x + inset, y + j, rw - 2 * inset, 1, c);
        }
    }
}

void Canvas::roundFrame(int x, int y, int rw, int rh, int r, uint16_t c) {
    // Two pixels wide: outer rounded rect minus an inner one, drawn as edges.
    for (int j = 0; j < rh; j++) {
        int inset = 0;
        uint8_t edge = 255;
        if (j < r) cornerRow(r, j, &inset, &edge);
        else if (j >= rh - r) cornerRow(r, rh - 1 - j, &inset, &edge);
        if (j < 2 || j >= rh - 2) {
            fill(x + inset, y + j, rw - 2 * inset, 1, c);
        } else {
            fill(x + inset, y + j, 2, 1, c);
            fill(x + rw - inset - 2, y + j, 2, 1, c);
        }
    }
}

void Canvas::circle(int cx, int cy, int r, uint16_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            float d = __builtin_sqrtf((float)(dx * dx + dy * dy)) - r + 0.5f;
            if (d <= 0) pixel(cx + dx, cy + dy, c);
            else if (d < 1) blendPixel(cx + dx, cy + dy, c, (uint8_t)(255 * (1 - d)));
        }
}

static const Glyph *find(const Font &f, uint32_t cp) {
    int lo = 0, hi = f.count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f.glyphs[m].code == cp) return &f.glyphs[m];
        if (f.glyphs[m].code < cp) lo = m + 1;
        else hi = m - 1;
    }
    return 0;
}

static uint32_t nextCp(const char *&s) {
    uint8_t b = (uint8_t)*s++;
    if (b < 0x80) return b;
    int more = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
    uint32_t cp = b & (0x3f >> more);
    while (more-- && (*s & 0xc0) == 0x80) cp = cp << 6 | (*s++ & 0x3f);
    return cp;
}

int Canvas::textWidth(const Font &f, const char *s) {
    int wsum = 0;
    while (*s) {
        const Glyph *g = find(f, nextCp(s));
        if (g) wsum += g->advance;
    }
    return wsum;
}

int Canvas::text(const Font &f, int x, int y, const char *s, uint16_t c, Align a) {
    int tw = textWidth(f, s);
    if (a == CENTER) x -= tw / 2;
    else if (a == RIGHT) x -= tw;
    while (*s) {
        const Glyph *g = find(f, nextCp(s));
        if (!g) continue;
        const uint8_t *bits = f.bits + g->offset;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int k = j * g->w + i;
                uint8_t v = (bits[k >> 1] >> ((k & 1) ? 0 : 4)) & 15;
                if (v) blendPixel(x + g->x + i, y + g->y + j, c, (uint8_t)(v * 17));
            }
        x += g->advance;
    }
    return tw;
}
