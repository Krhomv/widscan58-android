// What the scanner has sent: the display spectrum of each LO segment (with
// max hold and an average), telemetry, and when data last arrived.
// Fed by the link task from StreamParser; the UI reads it under a lock.
// GPL-3.0-or-later.
#pragma once
#include <stdint.h>

struct Segment {
    bool used;
    uint32_t loKhz;
    int32_t startOffHz;  // centre of bin 0, from the LO
    uint32_t binHz;
    uint16_t n;
    uint32_t updatedMs;
    static const int MAX_BINS = 512;
    uint8_t live[MAX_BINS];  // spectrum codes: dBFS per bin = code / 2 - 127.5
    float maxHold[MAX_BINS]; // in code units, decaying back towards live
    float avg[MAX_BINS];     // exponential average, linear power
};

class Scanner {
public:
    static const int MAX_SEGMENTS = 8;
    static const uint32_t STALE_MS = 3000;  // a segment not updated for this long is dropped (replanned)
    static constexpr float HOLD_TAU_S = 20.0f;  // max hold falls back to live with this time constant (~95% in a minute)

    Segment seg[MAX_SEGMENTS];
    uint32_t lastFrameMs = 0;  // 0: never
    uint32_t spectra = 0;
    float rotationHz = 0;
    uint32_t drops = 0;
    bool running = true;
    char info[48] = "";

    void onFrame(uint8_t type, const uint8_t *p, uint16_t len, uint32_t nowMs);
    void onLine(const char *line, uint32_t nowMs);
    void resetHolds();

    /** The band on `cols` columns from f0 to f1 MHz, each from the segment
     *  whose LO is nearest (the analog filter rolls off at segment edges), as
     *  the web UI's panorama: live and max hold are the strongest bin in the
     *  column (dBFS), avgLin the mean linear power. NaN where no segment
     *  reaches. */
    void composite(float f0Mhz, float f1Mhz, int cols, float *live, float *maxHold, float *avgLin, uint32_t nowMs) const;
};
