// Host test: feeds a simulated widscan58 stream through the parser, scanner
// model and UI, checks the parser, and writes the screen to a PPM image.
//   g++ -O2 -std=c++17 -Isrc test/host.cpp src/{proto,scanner,ui,gfx,fonts}.cpp -o /tmp/host && /tmp/host out.ppm
// GPL-3.0-or-later.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "platforms.h"
#include "proto.h"
#include "scanner.h"
#include "ui.h"

static std::vector<uint8_t> frame(uint8_t type, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> f = {0xa5, 'R', type, 1, (uint8_t)payload.size(), (uint8_t)(payload.size() >> 8)};
    f.insert(f.end(), payload.begin(), payload.end());
    uint32_t c = crc32(f.data(), f.size());
    for (int k = 0; k < 4; k++) f.push_back((uint8_t)(c >> (8 * k)));
    return f;
}
static void put32(std::vector<uint8_t> &v, uint32_t x) {
    for (int k = 0; k < 4; k++) v.push_back((uint8_t)(x >> (8 * k)));
}
static void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
}

static double rnd() { return rand() / (double)RAND_MAX; }

// Per-bin dBFS at f (MHz) for the simulated band.
static double band(double f, bool wifiOn) {
    double p = -96 + rnd() * 5;
    struct S { double f, a, bw; bool on; } sig[] = {{5658, -50, 9, true}, {5843, -56, 9, true}, {5757, -74, 9, wifiOn}, {5917, -82, 1.2, true}};
    for (auto &s : sig) {
        if (!s.on) continue;
        double d = (f - s.f) / s.bw, v;
        if (fabs(d) < 1.1) v = s.a - 22 * d * d + rnd() * 3;
        else v = s.a - 24 - 9 * fabs(d) + rnd() * 3;
        if (v > p) p = v;
    }
    return p;
}

static std::vector<uint8_t> spectrum(uint32_t loKhz, bool wifiOn) {
    std::vector<uint8_t> v;
    const int n = 256;
    const uint32_t bin = 312500;
    put32(v, 0);
    put32(v, loKhz);
    put32(v, (uint32_t)(int32_t)(-40000000 + bin / 2));
    put32(v, bin);
    put16(v, n);
    v.push_back(1);
    v.push_back(0);
    for (int k = 0; k < n; k++) {
        double f = loKhz / 1000.0 + (-40e6 + bin / 2.0 + k * (double)bin) / 1e6;
        double off = fabs(f - loKhz / 1000.0);
        double db = band(f, wifiOn) - (off > 28 ? (off - 28) * 1.5 : 0);  // filter skirt
        int code = (int)lround((db + 127.5) * 2);
        v.push_back((uint8_t)(code < 0 ? 0 : code > 255 ? 255 : code));
    }
    return v;
}

static std::vector<uint8_t> telemetry() {
    std::vector<uint8_t> v(59, 0);
    uint32_t mhz = 62400;
    memcpy(&v[12], &mhz, 4);
    v[58] = 1;
    return v;
}

struct Sink {
    Scanner *sc;
    uint32_t now;
    int lines;
};

int main(int argc, char **argv) {
    // Parser: frames survive being split anywhere, garbage and bad CRCs are skipped.
    {
        Scanner sc;
        Sink sink{&sc, 1, 0};
        StreamParser p([](void *c, uint8_t t, const uint8_t *pl, uint16_t n) { auto *s = (Sink *)c; s->sc->onFrame(t, pl, n, s->now); },
                       [](void *c, const char *) { ((Sink *)c)->lines++; }, &sink);
        std::vector<uint8_t> st;
        const char *junk = "\x12\xff garbage\xa5\x00 more\n";
        st.insert(st.end(), junk, junk + strlen(junk));
        auto good = frame(FRAME_SPECTRUM, spectrum(5676000, true));
        auto bad = good;
        bad[30] ^= 1;
        st.insert(st.end(), bad.begin(), bad.end());
        const char *ok = "OK\nINFO widscan58|esp32c5|1|x\n";
        st.insert(st.end(), ok, ok + strlen(ok));
        st.insert(st.end(), good.begin(), good.end());
        for (size_t i = 0; i < st.size(); i += 7) p.push(st.data() + i, st.size() - i < 7 ? st.size() - i : 7);
        assert(p.frames == 1);
        assert(p.crcErrors >= 1);
        assert(sink.lines == 2);
        assert(sc.spectra == 1);
        printf("parser ok: %u frames, %u crc errors, %u bytes skipped\n", p.frames, p.crcErrors, p.skipped);
    }

    // Max hold decays back to live: ~95% of the way in a minute, whatever the frame rate.
    {
        Scanner sc;
        auto hot = spectrum(5676000, true), quiet = hot;
        for (size_t k = 20; k < quiet.size(); k++) quiet[k] = 60;  // -97.5 dBFS everywhere
        sc.onFrame(FRAME_SPECTRUM, hot.data(), hot.size(), 1000);
        int j = 0;
        for (int k = 0; k < sc.seg[0].n; k++)
            if (hot[20 + k] > hot[20 + j]) j = k;
        assert(hot[20 + j] - 60 > 40);
        float start = sc.seg[0].maxHold[j] - quiet[20 + j];
        for (uint32_t t = 1030; t <= 1000 + 60000; t += 30) sc.onFrame(FRAME_SPECTRUM, quiet.data(), quiet.size(), t);
        float left = sc.seg[0].maxHold[j] - quiet[20 + j];
        assert(left < 0.1f * start && left > 0.02f * start);
        printf("hold decay ok: %.1f dB -> %.1f dB after 60 s\n", start / 2, left / 2);
    }

    static uint16_t fb[Ui::W * Ui::H];
    Canvas canvas(fb, Ui::W, Ui::H);
    Settings set;
    memcpy(set.rows, DEFAULT_ROWS, sizeof(set.rows));
    set.brightness = 0;
    Ui ui(canvas, set);
    Scanner sc;
    Sink sink{&sc, 1, 0};
    StreamParser p([](void *c, uint8_t t, const uint8_t *pl, uint16_t n) { auto *s = (Sink *)c; s->sc->onFrame(t, pl, n, s->now); },
                   [](void *c, const char *l) { ((Sink *)c)->sc->onLine(l, ((Sink *)c)->now); }, &sink);
    // Raceband in group mode: 4 LOs.
    const uint32_t los[] = {5676000, 5750000, 5824000, 5898000};
    int hop = 0;
    for (uint32_t t = 1; t < 4000; t += 4) {  // a spectrum every 4 ms, ~4 s
        sink.now = t;
        bool wifi = (t / 120) % 3 == 0;
        auto f = frame(FRAME_SPECTRUM, spectrum(los[hop++ % 4], wifi));
        p.push(f.data(), f.size());
        if (t % 250 == 1) {
            auto tf = frame(FRAME_TELEMETRY, telemetry());
            p.push(tf.data(), tf.size());
        }
        if (t % 32 == 1) ui.update(sc, t);
    }
    ui.link = LINK_OK;
    ui.batteryPct = 82;
    bool a, b, cc;
    ui.tap(436, 347 + 27 * 0 + 13, 3990, &a, &b, &cc);  // select R6 on the first row
    ui.update(sc, 4000);
    ui.draw(R_STATUS | R_PLOT | R_ROWS, 4000);

    // The plot drawn band by band (as on the board) must match the full draw.
    {
        RegionRows rr = regionRows(R_PLOT);
        static uint16_t band[Ui::W * 16];
        for (int y = rr.y0; y < rr.y1; y += 16) {
            int rows = rr.y1 - y < 16 ? rr.y1 - y : 16;
            for (auto &v : band) v = 0x1234;
            canvas.setBand(band, y, y + rows);
            ui.preparePlot();
            ui.drawPlot(4000);
            assert(!memcmp(band, fb + y * Ui::W, Ui::W * rows * 2));
        }
        canvas.setFull(fb);
        printf("bands ok\n");
    }

    FILE *out = fopen(argc > 1 ? argv[1] : "out.ppm", "wb");
    fprintf(out, "P6 %d %d 255\n", Ui::W, Ui::H);
    for (int i = 0; i < Ui::W * Ui::H; i++) {
        uint16_t v = fb[i];
        uint8_t px[3] = {(uint8_t)((v >> 11) * 255 / 31), (uint8_t)(((v >> 5) & 63) * 255 / 63), (uint8_t)((v & 31) * 255 / 31)};
        fwrite(px, 1, 3, out);
    }
    fclose(out);
    printf("rendered\n");
}
