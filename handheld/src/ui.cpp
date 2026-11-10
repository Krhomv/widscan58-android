// GPL-3.0-or-later.
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "platforms.h"

// Band shown: E4 to E8, the analog chart's edges.
static const float F0 = 5640, F1 = 5950;

// Layout (see the mockup in README.md).
static const int STATUS_H = 28;
static const int SPEC_T = 34, SPEC_B = 172;  // spectrum plot rows
static const int AXIS_Y = 186;              // MHz labels' baseline
static const int WF_T = 192;                // waterfall top (Ui::WF_ROWS tall)
static const int ROWS_T = 266, ROW_H = 27;  // channel rows
static const int LEGEND_Y = 442;

RegionRows regionRows(Region r) {
    switch (r) {
        case R_STATUS: return {0, STATUS_H};
        case R_PLOT: return {STATUS_H, 262};
        default: return {262, Ui::H};
    }
}

// Colours.
static const uint16_t BG = 0;
static const uint16_t INK = rgb(232, 234, 237);
static const uint16_t INK2 = rgb(154, 160, 166);
static const uint16_t MUTED = rgb(107, 113, 120);
static const uint16_t GRID = rgb(28, 31, 36);
static const uint16_t NOSCAN = rgb(20, 22, 25);
static const uint16_t BLUE = rgb(74, 163, 255);
static const uint16_t AMBER = rgb(214, 165, 0);
static const uint16_t GOOD = rgb(22, 163, 74), WARN = rgb(234, 179, 8), SERIOUS = rgb(240, 120, 80),
                      CRITICAL = rgb(220, 38, 38), GREY = rgb(42, 45, 49);

// Occupancy levels, as the Android app's Channels tab (dB above the floor).
static uint16_t levelColor(float db) {
    if (isnan(db)) return GREY;
    if (db < 4) return GOOD;
    if (db < 9) return WARN;
    if (db < 16) return SERIOUS;
    return CRITICAL;
}

float Ui::fx(float mhz) const { return LX + (mhz - F0) / (F1 - F0) * PW; }

static float percentile10(const float *v, int n) {
    static float tmp[Ui::PW];
    int m = 0;
    for (int j = 0; j < n; j++)
        if (!isnan(v[j])) tmp[m++] = v[j];
    if (!m) return NAN;
    // Partial selection sort is enough for the 10th percentile.
    int k = m / 10;
    for (int i = 0; i <= k; i++)
        for (int j = i + 1; j < m; j++)
            if (tmp[j] < tmp[i]) {
                float t = tmp[i];
                tmp[i] = tmp[j];
                tmp[j] = t;
            }
    return tmp[k];
}

void Ui::update(const Scanner &sc, uint32_t nowMs) {
    sc.composite(F0, F1, PW, live, maxH, avgLin, nowMs);
    rotationHz = sc.rotationHz;
    scanning = sc.running;
    if (sc.drops != lastDrops) {
        if (lastDrops && sc.drops > lastDrops) dropsAt = nowMs;
        lastDrops = sc.drops;
    }

    // The 10th percentile of linear power is that of dB: one log per frame.
    float fl = percentile10(avgLin, PW);
    if (!isnan(fl)) fl = 10.0f * log10f(fl + 1e-30f);
    if (!isnan(fl)) floorDb = isnan(floorDb) ? fl : floorDb + 0.05f * (fl - floorDb);
    if (!isnan(floorDb)) {
        // Keep the floor 8-18 dB above the bottom, moving in 10 dB steps.
        if (floorDb < bottomDb + 8 || floorDb > bottomDb + 18) bottomDb = floorf((floorDb - 8) / 10) * 10;
    }

    // Waterfall: about 30 rows a second.
    if (nowMs - wfLastMs >= 33) {
        wfLastMs = nowMs;
        wfHead = (wfHead + WF_ROWS - 1) % WF_ROWS;
        for (int x = 0; x < PW; x++) {
            float d = live[x];
            int code = isnan(d) ? 0 : (int)lroundf((d + 127.5f) * 2);
            wf[wfHead][x] = (uint8_t)(code < 1 ? (isnan(d) ? 0 : 1) : code > 255 ? 255 : code);
        }
    }

    // Channel levels: mean power over the channel from the average trace.
    const float colMhz = (F1 - F0) / PW;
    for (int r = 0; r < Settings::ROWS; r++) {
        const Platform &p = PLATFORMS[set.rows[r]];
        for (int j = 0; j < p.count; j++) {
            float lo = p.ch[j].mhz - p.bw / 2.0f, hi = p.ch[j].mhz + p.bw / 2.0f;
            int a = (int)ceilf((lo - F0) / colMhz - 0.5f), b = (int)floorf((hi - F0) / colMhz - 0.5f);
            if (a < 0) a = 0;
            if (b > PW - 1) b = PW - 1;
            float sum = 0;
            int got = 0, total = b - a + 1;
            for (int x = a; x <= b; x++)
                if (!isnan(avgLin[x])) {
                    sum += avgLin[x];
                    got++;
                }
            lev[r][j].db = total > 0 && got * 2 >= total && !isnan(floorDb) ? 10.0f * log10f(sum / got) - floorDb : NAN;
        }
    }
}

int Ui::draw(int mask, uint32_t nowMs) {
    if (mask & R_STATUS) drawStatus(nowMs);
    if (mask & R_PLOT) drawPlot(nowMs);
    if (mask & R_ROWS) drawRows();
    return mask;
}

void Ui::drawStatus(uint32_t nowMs) {
    c.fill(0, 0, W, STATUS_H, BG);
    int x = 8 + c.text(font_title, 8, 19, "Widscan58", INK);
    x += 8;
    uint16_t dot = link == LINK_OK ? (scanning ? GOOD : WARN) : link == LINK_WAITING ? WARN : CRITICAL;
    c.circle(x + 3, 14, 4, dot);
    x += 13;
    char s[64];
    if (link == LINK_OK)
        snprintf(s, sizeof(s), scanning ? "%.0f sweeps/s" : "scan stopped", rotationHz);
    else
        snprintf(s, sizeof(s), "%s", link == LINK_WAITING ? "connecting" : "no scanner");
    x += c.text(font_small, x, 19, s, INK2);
    if (link == LINK_OK && dropsAt && nowMs - dropsAt < 3000) c.text(font_small, x + 8, 19, "link overflow", SERIOUS);

    // Battery, right.
    int bx = W - 44;
    if (batteryPct >= 0) {
        uint16_t bc = batteryPct <= 15 ? CRITICAL : INK;
        c.fill(bx, 7, 28, 1, bc);
        c.fill(bx, 20, 28, 1, bc);
        c.fill(bx, 7, 1, 14, bc);
        c.fill(bx + 27, 7, 1, 14, bc);
        c.fill(bx + 28, 11, 2, 6, bc);
        int fw = (24 * batteryPct + 50) / 100;
        c.fill(bx + 2, 9, fw, 10, batteryPct <= 15 ? CRITICAL : GOOD);
        snprintf(s, sizeof(s), usbPower ? "USB" : "%d%%", batteryPct);
        c.text(font_small, bx - 6, 19, s, INK2, RIGHT);
        bx -= 6 + Canvas::textWidth(font_small, s);
    }

    // Clearest channel of the first row's platform.
    const Platform &p = PLATFORMS[set.rows[0]];
    int best = -1;
    for (int j = 0; j < p.count; j++)
        if (!isnan(lev[0][j].db) && (best < 0 || lev[0][j].db < lev[0][best].db)) best = j;
    if (best >= 0 && link == LINK_OK) {
        snprintf(s, sizeof(s), "%s %u", p.ch[best].name, p.ch[best].mhz);
        int w = c.text(font_label, bx - 16, 19, s, INK, RIGHT);
        snprintf(s, sizeof(s), "Clearest %s:", p.label);
        c.text(font_small, bx - 22 - w, 19, s, INK2, RIGHT);
    }
}

static void colormap(float t, uint8_t *r, uint8_t *g, uint8_t *b) {
    static const uint8_t st[][3] = {{0, 0, 0}, {20, 30, 110}, {0, 140, 200}, {60, 200, 90}, {250, 220, 40}, {255, 80, 40}};
    const int n = sizeof(st) / sizeof(st[0]);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float x = t * (n - 1);
    int k = (int)x;
    if (k > n - 2) k = n - 2;
    float u = x - k;
    *r = (uint8_t)(st[k][0] + (st[k + 1][0] - st[k][0]) * u);
    *g = (uint8_t)(st[k][1] + (st[k + 1][1] - st[k][1]) * u);
    *b = (uint8_t)(st[k][2] + (st[k + 1][2] - st[k][2]) * u);
}

void Ui::drawPlot(uint32_t nowMs) {
    RegionRows rr = regionRows(R_PLOT);
    c.fill(0, rr.y0, W, rr.y1 - rr.y0, BG);
    const float topDb = bottomDb + 70;
    auto dy = [&](float db) {
        float y = SPEC_T + (topDb - db) / (topDb - bottomDb) * (SPEC_B - SPEC_T);
        return (int)lroundf(y < SPEC_T ? SPEC_T : y > SPEC_B ? SPEC_B : y);
    };
    char s[48];

    // Trace heights first: the framebuffer is in PSRAM, so everything below
    // walks it row by row (a column walk misses the cache on every pixel).
    static int16_t yl[PW], ym[PW];
    for (int x = 0; x < PW; x++) {
        if (isnan(live[x])) {
            yl[x] = ym[x] = -1;
        } else {
            yl[x] = (int16_t)dy(live[x]);
            ym[x] = (int16_t)dy(maxH[x]);
        }
    }
    // Columns no LO reaches.
    for (int y = SPEC_T; y <= SPEC_B; y++) {
        uint16_t *row = c.px + y * W + LX;
        for (int x = 0; x < PW; x++)
            if (yl[x] < 0) row[x] = NOSCAN;
    }
    // Grid and scale.
    for (int db = (int)bottomDb + 10; db < topDb; db += 10) {
        int y = dy((float)db);
        c.hline(LX, y, PW, GRID);
        if (((db % 20) + 20) % 20 == 0) {
            snprintf(s, sizeof(s), "%d", db);
            c.text(font_small, LX - 6, y + 4, s, INK2, RIGHT);
        }
    }
    c.text(font_small, 8, dy(bottomDb + 35) + 4, "dBFS", MUTED);
    for (int f = 5650; f <= 5950; f += 50) {
        int x = (int)lroundf(fx((float)f));
        if (x < LX + PW) c.vline(x, SPEC_T, SPEC_B - SPEC_T + 1, GRID);
        snprintf(s, sizeof(s), "%d", f);
        if (x < LX + PW - 10) c.text(font_small, x, AXIS_Y, s, INK2, CENTER);
    }
    c.text(font_small, 8, AXIS_Y, "MHz", MUTED);

    // Live trace: a fading fill under a line, then the max hold line.
    const uint16_t fillC = rgb(56, 152, 255);
    for (int y = SPEC_T; y <= SPEC_B; y++) {
        float t = (float)(y - SPEC_T) / (SPEC_B - SPEC_T);
        uint8_t a = (uint8_t)(150 - 140 * t);
        uint16_t *row = c.px + y * W + LX;
        for (int x = 0; x < PW; x++)
            if (yl[x] >= 0 && y > yl[x]) row[x] = mix(fillC, row[x], a);
    }
    int prevL = -1, prevM = -1;
    for (int x = 0; x < PW; x++) {
        if (yl[x] < 0) {
            prevL = prevM = -1;
            continue;
        }
        c.span(LX + x, prevL < 0 ? yl[x] : prevL, yl[x], BLUE);
        c.span(LX + x, prevM < 0 ? ym[x] : prevM, ym[x], AMBER);
        prevL = yl[x];
        prevM = ym[x];
    }

    // Selected channel.
    if (selRow >= 0) {
        const Platform &p = PLATFORMS[set.rows[selRow]];
        const Channel &ch = p.ch[selCh];
        int x = (int)lroundf(fx(ch.mhz));
        int half = (int)lroundf(p.bw / 2.0f / (F1 - F0) * PW);
        c.blendFill(x - half, SPEC_T, 2 * half, SPEC_B - SPEC_T + 1, INK, 28);
        c.dashedV(x, SPEC_T, SPEC_B, INK, 3, 3);
        float db = lev[selRow][selCh].db;
        if (isnan(db)) snprintf(s, sizeof(s), "%s %s %u  not scanned", p.label, ch.name, ch.mhz);
        else snprintf(s, sizeof(s), "%s %s %u  %+.1f dB", p.label, ch.name, ch.mhz, db);
        int w = Canvas::textWidth(font_label, s);
        int tx = x + 8 + w > LX + PW ? x - 8 - w : x + 8;
        c.fill(tx - 3, SPEC_T + 2, w + 6, 16, BG);
        c.text(font_label, tx, SPEC_T + 14, s, INK);
    }

    // Waterfall.
    static uint16_t lut[256];
    float base = isnan(floorDb) ? -95 : floorDb - 3;
    for (int k = 1; k < 256; k++) {
        uint8_t r, g, b;
        colormap((k * 0.5f - 127.5f - base) / 45.0f, &r, &g, &b);
        lut[k] = rgb(r, g, b);
    }
    lut[0] = NOSCAN;
    for (int j = 0; j < WF_ROWS; j++) {
        const uint8_t *row = wf[(wfHead + j) % WF_ROWS];
        uint16_t *p = c.px + (WF_T + j) * W + LX;
        for (int x = 0; x < PW; x++) p[x] = lut[row[x]];
    }
    c.text(font_small, 8, WF_T + 12, "Waterfall", MUTED);
    c.text(font_small, 8, WF_T + 26, "last 2 s", MUTED);

    // Messages over the spectrum.
    const char *msg = 0;
    if (link == LINK_WAITING) msg = "Waiting for the scanner\xe2\x80\xa6";
    else if (link == LINK_LOST) msg = "No data from the scanner. Check the wiring, then tap here.";
    else if (!scanning) msg = "The scanner is stopped.";
    if (msg) {
        int w = Canvas::textWidth(font_label, msg);
        int y = (SPEC_T + SPEC_B) / 2;
        c.roundRect(LX + PW / 2 - w / 2 - 12, y - 16, w + 24, 28, 6, rgb(32, 35, 40));
        c.text(font_label, LX + PW / 2, y + 3, msg, INK, CENTER);
    }
    if (nowMs < toastUntil) {
        int w = Canvas::textWidth(font_small, toast);
        c.roundRect(LX + PW - w - 18, SPEC_T + 4, w + 12, 18, 4, rgb(32, 35, 40));
        c.text(font_small, LX + PW - 12, SPEC_T + 17, toast, INK, RIGHT);
    }
}

void Ui::boxGeometry(int row, int ch, int *x, int *w) const {
    const Platform &p = PLATFORMS[set.rows[row]];
    float pxPerMhz = (float)PW / (F1 - F0);
    // As wide as the channel, but no wider than the gap to its neighbours.
    float width = p.bw * pxPerMhz;
    for (int j = 0; j < p.count; j++)
        if (j != ch) {
            float d = fabsf((float)p.ch[j].mhz - p.ch[ch].mhz) * pxPerMhz;
            if (d < width) width = d;
        }
    int bw = (int)width - 3;
    if (bw < 18) bw = 18;
    *w = bw;
    *x = (int)lroundf(fx(p.ch[ch].mhz) - bw / 2.0f);
}

void Ui::drawRows() {
    RegionRows rr = regionRows(R_ROWS);
    c.fill(0, rr.y0, W, rr.y1 - rr.y0, BG);
    for (int r = 0; r < Settings::ROWS; r++) {
        const Platform &p = PLATFORMS[set.rows[r]];
        int y = ROWS_T + r * ROW_H;
        c.text(font_label, 8, y + ROW_H / 2 + 4, p.label, INK);
        for (int j = 0; j < p.count; j++) {
            int x, w;
            boxGeometry(r, j, &x, &w);
            float db = lev[r][j].db;
            uint16_t col = levelColor(db);
            c.roundRect(x, y + 3, w, ROW_H - 6, 4, col);
            if (r == selRow && j == selCh) c.roundFrame(x - 2, y + 1, w + 4, ROW_H - 2, 5, INK);
            uint16_t tc = col == WARN || col == SERIOUS ? rgb(17, 17, 17) : col == GREY ? INK2 : rgb(255, 255, 255);
            c.text(font_label, x + w / 2, y + ROW_H / 2 + 4, p.ch[j].name, tc, CENTER);
        }
    }
    // Legend.
    static const struct {
        uint16_t c;
        const char *t;
    } L[] = {{GOOD, "Clear <4 dB"}, {WARN, "Light 4\xe2\x80\x93" "9"}, {SERIOUS, "Busy 9\xe2\x80\x93" "16"},
             {CRITICAL, "Occupied \xe2\x89\xa5" "16"}, {GREY, "Not scanned"}};
    int x = 8;
    for (const auto &l : L) {
        c.roundRect(x, LEGEND_Y - 9, 10, 10, 2, l.c);
        x += 15 + c.text(font_small, x + 15, LEGEND_Y, l.t, INK2) + 16;
    }
}

int Ui::tap(int x, int y, uint32_t nowMs, bool *settingsChanged, bool *resetHolds, bool *retry) {
    *settingsChanged = *resetHolds = *retry = false;
    if (y < STATUS_H) {
        set.brightness = (uint8_t)((set.brightness + 1) % BRIGHTNESS_LEVELS);
        *settingsChanged = true;
        return 0;
    }
    if (y < regionRows(R_PLOT).y1) {
        if (link == LINK_LOST) {
            *retry = true;
            return R_PLOT;
        }
        *resetHolds = true;
        toast = "Max hold reset";
        toastUntil = nowMs + 1200;
        return R_PLOT;
    }
    int r = (y - ROWS_T) / ROW_H;
    if (y < ROWS_T || r >= Settings::ROWS) return 0;
    if (x < LX) {
        // Next platform not already on screen.
        int p = set.rows[r];
        for (int k = 0; k < PLATFORM_COUNT; k++) {
            p = (p + 1) % PLATFORM_COUNT;
            bool shown = false;
            for (int j = 0; j < Settings::ROWS; j++)
                if (j != r && set.rows[j] == p) shown = true;
            if (!shown) break;
        }
        set.rows[r] = (uint8_t)p;
        if (selRow == r) selRow = selCh = -1;
        *settingsChanged = true;
        return R_ROWS | R_STATUS | R_PLOT;
    }
    // The nearest box in the row.
    const Platform &p = PLATFORMS[set.rows[r]];
    int best = -1, bestD = 1 << 30;
    for (int j = 0; j < p.count; j++) {
        int bx, bw;
        boxGeometry(r, j, &bx, &bw);
        int d = x < bx ? bx - x : x >= bx + bw ? x - bx - bw + 1 : 0;
        if (d < bestD) {
            bestD = d;
            best = j;
        }
    }
    if (best < 0 || bestD > 6) return 0;
    if (selRow == r && selCh == best) selRow = selCh = -1;
    else selRow = r, selCh = best;
    return R_ROWS | R_PLOT;
}
