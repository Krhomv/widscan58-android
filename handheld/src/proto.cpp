// GPL-3.0-or-later.
#include "proto.h"
#include <string.h>

uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static const uint8_t SYNC0 = 0xa5, SYNC1 = 'R';

void StreamParser::push(const uint8_t *data, size_t n) {
    while (n) {
        size_t take = CAP - len < n ? CAP - len : n;
        memcpy(buf + len, data, take);
        len += take;
        data += take;
        n -= take;
        size_t used = parse();
        if (!used && len == CAP) used = 1;  // can't happen with valid data: drop a byte and resync
        memmove(buf, buf + used, len - used);
        len -= used;
    }
}

/** Consumes what it can from buf; returns the bytes used. */
size_t StreamParser::parse() {
    size_t pos = 0;
    while (pos < len) {
        if (buf[pos] == SYNC0) {
            if (len - pos < 6) break;
            if (buf[pos + 1] != SYNC1) {
                pos++;
                skipped++;
                continue;
            }
            size_t plen = rd16(buf + pos + 4);
            size_t total = 6 + plen + 4;
            if (total > CAP) {  // not a frame we can hold (or not a frame at all)
                pos++;
                skipped++;
                continue;
            }
            if (len - pos < total) break;
            if (crc32(buf + pos, 6 + plen) != rd32(buf + pos + 6 + plen)) {
                crcErrors++;
                pos++;
                skipped++;
                continue;
            }
            frames++;
            onFrame(ctx, buf[pos + 2], buf + pos + 6, (uint16_t)plen);
            pos += total;
            continue;
        }
        // Text up to a newline, cut short by a sync byte (a frame interrupted it).
        size_t end = pos;
        while (end < len && buf[end] != '\n' && buf[end] != SYNC0) end++;
        if (end >= len) {
            if (len - pos > 512) {  // garbage without a newline (e.g. a boot banner at another baud rate)
                skipped += len - pos;
                pos = len;
            }
            break;
        }
        char line[160];
        size_t n = end - pos < sizeof(line) - 1 ? end - pos : sizeof(line) - 1;
        memcpy(line, buf + pos, n);
        if (n && line[n - 1] == '\r') n--;
        line[n] = 0;
        if (buf[end] == '\n') end++;
        // Replies start with an uppercase command word; anything else is the
        // tail of a frame we joined part way through.
        if (line[0] >= 'A' && line[0] <= 'Z') onLine(ctx, line);
        else skipped += n;
        pos = end;
    }
    return pos;
}
