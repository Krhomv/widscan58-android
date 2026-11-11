// widscan58's host protocol (firmware/src/proto/frames.h in CapnBry/widscan58):
// ASCII reply lines interleaved with binary frames
//   0xA5 'R' type version | u16 len | payload | u32 crc32 (zlib), little-endian.
// Splits the byte stream from the scanner into lines and CRC-checked frames.
// GPL-3.0-or-later.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    FRAME_METRICS = 1,
    FRAME_SPECTRUM = 2,
    FRAME_TELEMETRY = 3,
    FRAME_RAWIQ = 4,
    FRAME_LOG = 5,
    FRAME_LEVELS = 6,
};

uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0);

class StreamParser {
public:
    typedef void (*FrameFn)(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len);
    typedef void (*LineFn)(void *ctx, const char *line);
    StreamParser(FrameFn onFrame, LineFn onLine, void *ctx) : onFrame(onFrame), onLine(onLine), ctx(ctx) {}
    void push(const uint8_t *data, size_t n);

    uint32_t frames = 0, crcErrors = 0, skipped = 0;

private:
    static const size_t CAP = 4096;  // the largest frame we use is ~300 bytes; RAWIQ is ~2 KB
    uint8_t buf[CAP];
    size_t len = 0;
    FrameFn onFrame;
    LineFn onLine;
    void *ctx;
    size_t parse();
};

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
