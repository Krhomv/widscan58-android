// GPL-3.0-or-later.
#include "scanner.h"
#include <math.h>
#include <string.h>
#include "proto.h"

static inline float codeDb(uint8_t c) { return c * 0.5f - 127.5f; }

void Scanner::onFrame(uint8_t type, const uint8_t *p, uint16_t len, uint32_t nowMs) {
    lastFrameMs = nowMs ? nowMs : 1;
    if (type == FRAME_SPECTRUM && len >= 20) {
        uint32_t lo = rd32(p + 4);
        int32_t start = (int32_t)rd32(p + 8);
        uint32_t bin = rd32(p + 12);
        uint16_t n = rd16(p + 16);
        if (n > Segment::MAX_BINS || len < 20 + n || !bin) return;
        const uint8_t *codes = p + 20;
        Segment *s = 0;
        for (auto &x : seg)
            if (x.used && x.loKhz == lo) s = &x;
        bool fresh = !s || s->n != n || s->binHz != bin || s->startOffHz != start;
        if (!s) {
            // A free slot, else the stalest.
            s = &seg[0];
            for (auto &x : seg) {
                if (!x.used) {
                    s = &x;
                    break;
                }
                if (x.updatedMs < s->updatedMs) s = &x;
            }
        }
        s->used = true;
        s->loKhz = lo;
        s->startOffHz = start;
        s->binHz = bin;
        s->n = n;
        s->updatedMs = nowMs;
        memcpy(s->live, codes, n);
        for (int j = 0; j < n; j++) {
            float lin = powf(10.0f, codeDb(codes[j]) / 10.0f);
            if (fresh) {
                s->maxHold[j] = codes[j];
                s->avg[j] = lin;
            } else {
                if (codes[j] > s->maxHold[j]) s->maxHold[j] = codes[j];
                s->avg[j] += 0.05f * (lin - s->avg[j]);
            }
        }
        spectra++;
    } else if (type == FRAME_TELEMETRY && len >= 59) {
        rotationHz = rd32(p + 12) / 1000.0f;
        drops = rd32(p + 50);
        running = p[58] != 0;
    }
}

void Scanner::onLine(const char *line, uint32_t nowMs) {
    (void)nowMs;
    if (!strncmp(line, "INFO ", 5)) {
        strncpy(info, line + 5, sizeof(info) - 1);
        info[sizeof(info) - 1] = 0;
    }
}

void Scanner::resetHolds() {
    for (auto &s : seg)
        if (s.used) {
            memcpy(s.maxHold, s.live, s.n);
            for (int j = 0; j < s.n; j++) s.avg[j] = powf(10.0f, codeDb(s.live[j]) / 10.0f);
        }
}

void Scanner::composite(float f0, float f1, int cols, float *live, float *maxHold, float *avg, uint32_t nowMs) const {
    const float colMhz = (f1 - f0) / cols;
    for (int x = 0; x < cols; x++) {
        live[x] = maxHold[x] = avg[x] = NAN;
        float fa = f0 + x * colMhz, fb = fa + colMhz, fc = fa + colMhz / 2;
        float bestDist = INFINITY;
        const Segment *best = 0;
        int ka = 0, kb = 0;
        for (const auto &s : seg) {
            if (!s.used || (int32_t)(nowMs - s.updatedMs) > (int32_t)STALE_MS) continue;
            float lo = s.loKhz / 1000.0f;
            float start = lo + s.startOffHz / 1e6f, bin = s.binHz / 1e6f;
            if (fc < start - bin / 2 || fc > start + (s.n - 0.5f) * bin) continue;
            // Bins whose centre falls in the column; at least the nearest one.
            int a = (int)ceilf((fa - start) / bin - 0.5f), b = (int)floorf((fb - start) / bin - 0.5f);
            if (b < a) a = b = (int)lroundf((fc - start) / bin);
            if (a < 0) a = 0;
            if (b >= s.n) b = s.n - 1;
            if (a > b) continue;
            float d = fabsf(fc - lo);
            if (d < bestDist) {
                bestDist = d;
                best = &s;
                ka = a;
                kb = b;
            }
        }
        if (!best) continue;
        uint8_t l = 0, m = 0;
        float sum = 0;
        for (int k = ka; k <= kb; k++) {
            if (best->live[k] > l) l = best->live[k];
            if (best->maxHold[k] > m) m = best->maxHold[k];
            sum += best->avg[k];
        }
        live[x] = codeDb(l);
        maxHold[x] = codeDb(m);
        avg[x] = 10.0f * log10f(sum / (kb - ka + 1) + 1e-30f);
    }
}
