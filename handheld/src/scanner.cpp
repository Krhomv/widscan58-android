// GPL-3.0-or-later.
#include "scanner.h"
#include <math.h>
#include <string.h>
#include "proto.h"

static inline float codeDb(uint8_t c) { return c * 0.5f - 127.5f; }

// Linear power of each spectrum code: powf is slow on the S3 (no double FPU).
static const float *linTable() {
    static float t[256];
    static bool done;
    if (!done) {
        for (int k = 0; k < 256; k++) t[k] = powf(10.0f, codeDb((uint8_t)k) / 10.0f);
        done = true;
    }
    return t;
}

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
        // Time since this segment's last update, for the max hold decay.
        float dt = s && !fresh ? (int32_t)(nowMs - s->updatedMs) / 1000.0f : 0;
        if (dt < 0) dt = 0;
        float k = dt / HOLD_TAU_S;
        if (k > 1) k = 1;
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
        if (eqN != n || eqBinHz != bin || eqStartOffHz != start) seedEq(start, bin, n);
        s->used = true;
        s->loKhz = lo;
        s->startOffHz = start;
        s->binHz = bin;
        s->n = n;
        s->updatedMs = nowMs;
        memcpy(s->live, codes, n);
        const float *lt = linTable();
        for (int j = 0; j < n; j++) {
            float lin = lt[codes[j]];
            if (fresh) {
                s->maxHold[j] = codes[j];
                s->avg[j] = lin;
            } else {
                float h = s->maxHold[j];
                h -= k * (h - codes[j]);
                s->maxHold[j] = codes[j] > h ? codes[j] : h;
                s->avg[j] += 0.05f * (lin - s->avg[j]);
            }
        }
        spectra++;
        if ((int32_t)(spectra - eqLearnAt) >= 0) {
            learnEq(nowMs);
            eqLearnAt = spectra + 32;  // ~4 passes a second at ~125 spectra/s
        }
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
            for (int j = 0; j < s.n; j++) {
                s.maxHold[j] = s.live[j];
                s.avg[j] = linTable()[s.live[j]];
            }
        }
}

// Measured response (dB) at |offset| MHz from the LO: widscan58 PLAN.md,
// 2026-10-21 (same noise shape at 5677/5750/5800/5899 MHz with the default
// ADC path).
static float seedResponseDb(float offMhz) {
    static const float pt[][2] = {{0, 0}, {20, 0}, {22, -2.5f}, {28, -3}, {34, -11}, {38, -12}, {40, -14}};
    const int n = sizeof(pt) / sizeof(pt[0]);
    float a = fabsf(offMhz);
    if (a >= pt[n - 1][0]) return pt[n - 1][1];
    for (int k = 1; k < n; k++)
        if (a <= pt[k][0]) return pt[k - 1][1] + (pt[k][1] - pt[k - 1][1]) * (a - pt[k - 1][0]) / (pt[k][0] - pt[k - 1][0]);
    return 0;
}

static float clampEq(float v) { return v < -3 ? -3 : v > Scanner::EQ_MAX_DB ? Scanner::EQ_MAX_DB : v; }

void Scanner::seedEq(int32_t startOffHz, uint32_t binHz, uint16_t n) {
    eqStartOffHz = startOffHz;
    eqBinHz = binHz;
    eqN = n;
    for (int k = 0; k < n; k++) {
        eq[k] = clampEq(-seedResponseDb((startOffHz + (float)k * binHz) / 1e6f));
        eqLin[k] = powf(10.0f, eq[k] / 10.0f);
    }
}

static float medianOf(float *v, int n) {  // partial selection sort; n is small
    int m = (n - 1) / 2;  // the lower middle for even n: a signal pulls up, never down
    for (int i = 0; i <= m; i++)
        for (int j = i + 1; j < n; j++)
            if (v[j] < v[i]) {
                float t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
    return v[m];
}

void Scanner::learnEq(uint32_t nowMs) {
    const Segment *use[MAX_SEGMENTS];
    int ns = 0;
    for (const auto &s : seg)
        if (s.used && (int32_t)(nowMs - s.updatedMs) <= (int32_t)STALE_MS && s.n == eqN && s.binHz == eqBinHz &&
            s.startOffHz == eqStartOffHz)
            use[ns++] = &s;
    if (ns < 3 || !eqN) return;  // too few for a median that ignores signals
    // Noise floor per bin offset: the median across segments of each
    // segment's averaged power, in dB.
    static float floorDb[Segment::MAX_BINS];
    float v[MAX_SEGMENTS];
    for (int k = 0; k < eqN; k++) {
        for (int j = 0; j < ns; j++) v[j] = 10.0f * log10f(use[j]->avg[k] + 1e-30f);
        floorDb[k] = medianOf(v, ns);
    }
    // Reference: the median over the flat middle (+-15 MHz, DC excluded).
    static float mid[Segment::MAX_BINS];
    int nm = 0;
    for (int k = 0; k < eqN; k++) {
        float off = fabsf((eqStartOffHz + (float)k * eqBinHz) / 1e6f);
        if (off < 15 && off > 1) mid[nm++] = floorDb[k];
    }
    if (nm < 8) return;
    float ref = medianOf(mid, nm);
    // Target EQ, smoothed by a 9-bin running median (spurs, a lone signal).
    const int H = 4;
    float w[2 * H + 1];
    for (int k = 0; k < eqN; k++) {
        int c = 0;
        for (int d = -H; d <= H; d++) {
            int i = k + d;
            if (i >= 0 && i < eqN) w[c++] = floorDb[i];
        }
        float target = clampEq(ref - medianOf(w, c));
        eq[k] += 0.05f * (target - eq[k]);  // ~5 s at 4 passes a second
        eqLin[k] = powf(10.0f, eq[k] / 10.0f);
    }
    eqUpdates++;
}

// Crossfade weight of a segment's bin at `off` MHz from its LO: full in the
// middle, fading to nothing at Nyquist, a little less right at DC (the DC bin
// is masked by the scanner and the neighbours fill in).
static float blendWeight(float offMhz) {
    float a = fabsf(offMhz) / 40.0f;
    if (a >= 1) return 0;
    float w = (1 - a) * (1 - a);
    if (fabsf(offMhz) < 0.6f) w *= 0.25f;
    return w;
}

void Scanner::composite(float f0, float f1, int cols, float *live, float *maxHold, float *avgLin, uint32_t nowMs) const {
    const float colMhz = (f1 - f0) / cols;
    for (int x = 0; x < cols; x++) {
        live[x] = maxHold[x] = avgLin[x] = NAN;
        float fa = f0 + x * colMhz, fb = fa + colMhz, fc = fa + colMhz / 2;
        float wSum = 0, lSum = 0, mSum = 0, aSum = 0;
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
            float w = blendWeight(fc - lo);
            if (w <= 0) w = 1e-6f;  // a lone segment at its very edge still shows
            const bool eqd = eqN == s.n && eqBinHz == s.binHz && eqStartOffHz == s.startOffHz;
            float l = -1e9f, m = -1e9f, sum = 0;
            for (int k = a; k <= b; k++) {
                float e = eqd ? eq[k] : 0;
                float ld = codeDb(s.live[k]) + e, md = s.maxHold[k] * 0.5f - 127.5f + e;
                if (ld > l) l = ld;
                if (md > m) m = md;
                sum += s.avg[k] * (eqd ? eqLin[k] : 1);
            }
            wSum += w;
            lSum += w * l;
            mSum += w * m;
            aSum += w * sum / (b - a + 1);
        }
        if (wSum <= 0) continue;
        live[x] = lSum / wSum;
        maxHold[x] = mSum / wSum;
        avgLin[x] = aSum / wSum;
    }
}
