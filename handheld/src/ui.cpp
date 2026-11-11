// GPL-3.0-or-later.
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "platforms.h"

// Band shown: E4 to E8, the analog chart's edges.
static const float BAND0 = 5640, BAND1 = 5950;  // the axis at most; it shrinks to what the scanner covers

// Layout (see the mockup in README.md).
static const int STATUS_H = 28;
static const int SPEC_T = 34, SPEC_B = 236;  // spectrum plot rows
static const int AXIS_Y = 250;              // MHz labels' baseline
static const int WF_T = 256;                // waterfall top (Ui::WF_ROWS tall)
static const int ROWS_T = 360, ROW_H = 30;  // channel rows
static const int BOX_H = 25;

RegionRows regionRows(Region r) {
    switch (r) {
        case R_STATUS: return {0, STATUS_H};
        case R_PLOT: return {STATUS_H, 358};
        default: return {358, Ui::H};
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

float Ui::fx(float mhz) const { return LX + (mhz - f0) / (f1 - f0) * PW; }

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
    // The axis spans what the scanner's segments cover (within the band), so
    // no columns are left empty at the edges.
    float lo = 1e9f, hi = 0;
    for (const auto &g : sc.seg) {
        if (!g.used || (int32_t)(nowMs - g.updatedMs) > (int32_t)Scanner::STALE_MS) continue;
        float start = g.loKhz / 1000.0f + g.startOffHz / 1e6f, bin = g.binHz / 1e6f;
        if (start - bin / 2 < lo) lo = start - bin / 2;
        if (start + (g.n - 0.5f) * bin > hi) hi = start + (g.n - 0.5f) * bin;
    }
    if (hi > lo) {
        float a = floorf(lo < BAND0 ? BAND0 : lo), b = ceilf(hi > BAND1 ? BAND1 : hi);
        if (b - a > 100 && (fabsf(a - f0) >= 2 || fabsf(b - f1) >= 2)) {
            f0 = a;
            f1 = b;
            memset(wf, WF_EMPTY, sizeof(wf));  // its columns were on the old axis
        }
    }
    sc.composite(f0, f1, PW, live, maxH, avgLin, nowMs);
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
    const float colMhz = (f1 - f0) / PW;
    for (int r = 0; r < Settings::ROWS; r++) {
        const Platform &p = PLATFORMS[set.rows[r]];
        for (int j = 0; j < p.count; j++) {
            float lo = p.ch[j].mhz - p.bw / 2.0f, hi = p.ch[j].mhz + p.bw / 2.0f;
            int a = (int)ceilf((lo - f0) / colMhz - 0.5f), b = (int)floorf((hi - f0) / colMhz - 0.5f);
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
    if (mask & R_PLOT) {
        preparePlot();
        drawPlot(nowMs);
    }
    if (mask & R_ROWS) drawRows();
    return mask;
}

// A lightning bolt, BOLT_W x BOLT_H at scale 1.
static const char *const BOLT[] = {
    "    ####", "   #### ", "   ###  ", "  ###   ", "  ###   ", " #######", "####### ",
    "   ###  ", "   ##   ", "  ##    ", "  #     ", " #      ", " #      ",
};
static const int BOLT_W = 8, BOLT_H = (int)(sizeof(BOLT) / sizeof(BOLT[0]));
static void drawBolt(Canvas &c, int x0, int y0, int scale, uint16_t color) {
    for (int r = 0; r < BOLT_H; r++)
        for (int k = 0; k < BOLT_W; k++)
            if (BOLT[r][k] == '#') c.fill(x0 + k * scale, y0 + r * scale, scale, scale, color);
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
        uint16_t bc = batteryPct <= 15 && !charging ? CRITICAL : INK;
        c.fill(bx, 7, 28, 1, bc);
        c.fill(bx, 20, 28, 1, bc);
        c.fill(bx, 7, 1, 14, bc);
        c.fill(bx + 27, 7, 1, 14, bc);
        c.fill(bx + 28, 11, 2, 6, bc);
        int pct = batteryPct;
        if (charging) {
            // Fill rising from the last level read on battery to full, over
            // and over, as phones do: the voltage can't tell the charge.
            int step = (int)(nowMs / 400 % 5);
            pct += (100 - pct) * step / 4;
        }
        int fw = (24 * pct + 50) / 100;
        c.fill(bx + 2, 9, fw, 10, !charging && batteryPct <= 15 ? CRITICAL : GOOD);
        if (usbPower) snprintf(s, sizeof(s), "USB");
        else if (full) snprintf(s, sizeof(s), "%.2f V  Charged", batteryV);
        else if (charging) snprintf(s, sizeof(s), "%.2f V", batteryV);
        else snprintf(s, sizeof(s), "%.2f V  %d%%", batteryV, batteryPct);
        if (charging || full) {
            // A lightning bolt left of the cell.
            int x0 = bx - 4 - BOLT_W;
            drawBolt(c, x0, 14 - BOLT_H / 2, 1, AMBER);
            bx = x0;
        }
        c.text(font_small, bx - 6, 19, s, INK2, RIGHT);
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

void Ui::preparePlot() {
    const float topDb = bottomDb + 70;
    for (int x = 0; x < PW; x++) {
        if (isnan(live[x])) {
            yl[x] = ym[x] = -1;
        } else {
            float a = SPEC_T + (topDb - live[x]) / 70 * (SPEC_B - SPEC_T);
            float b = SPEC_T + (topDb - maxH[x]) / 70 * (SPEC_B - SPEC_T);
            yl[x] = (int16_t)lroundf(a < SPEC_T ? SPEC_T : a > SPEC_B ? SPEC_B : a);
            ym[x] = (int16_t)lroundf(b < SPEC_T ? SPEC_T : b > SPEC_B ? SPEC_B : b);
        }
    }
    float base = isnan(floorDb) ? -95 : floorDb - 3;
    for (int k = 1; k < 256; k++) {
        uint8_t r, g, b;
        colormap((k * 0.5f - 127.5f - base) / 45.0f, &r, &g, &b);
        wfLut[k] = rgb(r, g, b);
    }
    wfLut[0] = NOSCAN;
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
    // The spectrum rows in this band.
    const int sy0 = SPEC_T > c.cy0 ? SPEC_T : c.cy0, sy1 = SPEC_B < c.cy1 - 1 ? SPEC_B : c.cy1 - 1;

    // Columns no LO reaches.
    for (int y = sy0; y <= sy1; y++) {
        uint16_t *row = c.px + y * W + LX;
        for (int x = 0; x < PW; x++)
            if (yl[x] < 0) row[x] = NOSCAN;
    }
    // Grid and scale.
    for (int db = (int)bottomDb + 10; db < topDb; db += 10) {
        int y = dy((float)db);
        c.hline(LX, y, PW, GRID);
    }
    for (int f = 5650; f <= 5950; f += 50) {
        int x = (int)lroundf(fx((float)f));
        if (x < LX || x >= LX + PW) continue;
        c.vline(x, SPEC_T, SPEC_B - SPEC_T + 1, GRID);
        snprintf(s, sizeof(s), "%d", f);
        if (x >= LX + 16 && x < LX + PW - 16) c.text(font_small, x, AXIS_Y, s, INK2, CENTER);
    }

    // Live trace: a fading fill under a line, then the max hold line.
    const uint16_t fillC = rgb(56, 152, 255);
    for (int y = sy0; y <= sy1; y++) {
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
        int half = (int)lroundf(p.bw / 2.0f / (f1 - f0) * PW);
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
    for (int j = 0; j < WF_ROWS; j++) {
        if (WF_T + j < c.cy0 || WF_T + j >= c.cy1) continue;
        const uint8_t *row = wf[(wfHead + j) % WF_ROWS];
        uint16_t *p = c.px + (WF_T + j) * W + LX;
        for (int x = 0; x < PW; x++) p[x] = wfLut[row[x]];
    }

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
    float pxPerMhz = (float)PW / (f1 - f0);
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
        int by = y + (ROW_H - BOX_H) / 2;
        for (int j = 0; j < p.count; j++) {
            int x, w;
            boxGeometry(r, j, &x, &w);
            float db = lev[r][j].db;
            uint16_t col = levelColor(db);
            c.roundRect(x, by, w, BOX_H, 4, col);
            if (r == selRow && j == selCh) c.roundFrame(x - 2, by - 2, w + 4, BOX_H + 4, 5, INK);
            uint16_t tc = col == WARN || col == SERIOUS ? rgb(17, 17, 17) : col == GREY ? INK2 : rgb(255, 255, 255);
            c.text(font_label, x + w / 2, by + BOX_H / 2 + 5, p.ch[j].name, tc, CENTER);
        }
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

/** A lightning bolt bw x bh at x0, y0, anti-aliased (4x4 samples a pixel). */
static void drawBigBolt(Canvas &c, int x0, int y0, int bw, int bh, uint16_t color) {
    static const float P[][2] = {{0.62f, 0}, {0.08f, 0.58f}, {0.44f, 0.58f}, {0.30f, 1}, {0.92f, 0.38f}, {0.55f, 0.38f}, {0.78f, 0}};
    const int n = sizeof(P) / sizeof(P[0]);
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = (x + (sx + 0.5f) / 4) / bw, py = (y + (sy + 0.5f) / 4) / bh;
                    bool in = false;
                    for (int i = 0, j = n - 1; i < n; j = i++)
                        if ((P[i][1] > py) != (P[j][1] > py) &&
                            px < (P[j][0] - P[i][0]) * (py - P[i][1]) / (P[j][1] - P[i][1]) + P[i][0])
                            in = !in;
                    hits += in;
                }
            if (hits) c.blendPixel(x0 + x, y0 + y, color, (uint8_t)(hits * 255 / 16));
        }
}

void Ui::drawCharging(uint32_t nowMs, int dx, int dy) {
    c.fill(0, 0, W, H, BG);
    // A big cell, centred (shifted by dx, dy against OLED wear).
    const int bw = 240, bh = 110, t = 6;
    int x = (W - bw) / 2 + dx, y = 120 + dy;
    uint16_t frame = INK;
    c.fill(x, y, bw, t, frame);
    c.fill(x, y + bh - t, bw, t, frame);
    c.fill(x, y, t, bh, frame);
    c.fill(x + bw - t, y, t, bh, frame);
    c.fill(x + bw, y + bh / 2 - 22, 12, 44, frame);
    int inner = bw - 2 * t - 8;
    int pct = full ? 100 : batteryPct < 0 ? 0 : batteryPct;
    if (charging && !full) {
        int step = (int)(nowMs / 400 % 5);
        pct += (100 - pct) * step / 4;
    }
    if (!usbPower) c.fill(x + t + 4, y + t + 4, inner * pct / 100, bh - 2 * t - 8, GOOD);
    if (charging || full || usbPower) drawBigBolt(c, x + bw / 2 - 30, y + 14, 60, bh - 28, usbPower ? AMBER : INK);
    char s[48];
    const char *title = usbPower ? "USB power, no cell" : full ? "Charged" : charging ? "Charging" : "On battery";
    c.text(font_big, W / 2 + dx, y + bh + 70, title, INK, CENTER);
    if (!usbPower) {
        snprintf(s, sizeof(s), "%.2f V", batteryV);
        c.text(font_label, W / 2 + dx, y + bh + 110, s, INK2, CENTER);
    }
    c.text(font_small, W / 2 + dx, y + bh + 160, "Press power to scan", INK2, CENTER);
}

// ---- The rows menu (a long press on the channel rows) ----

static const int MENU_TOP = 64, MENU_COLS = 2, MENU_GAP = 8, MENU_BTN_Y = 396, MENU_BTN_H = 44;

void Ui::openMenu() {
    menuMask = 0;
    for (int r = 0; r < Settings::ROWS; r++) menuMask |= 1u << set.rows[r];
    menuOpen = true;
}

static int popcount(uint32_t m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

static void menuCell(int k, int *x, int *y, int *w, int *h) {
    const int rows = (PLATFORM_COUNT + MENU_COLS - 1) / MENU_COLS;
    *w = (Ui::W - 2 * 12 - (MENU_COLS - 1) * MENU_GAP) / MENU_COLS;
    *h = (MENU_BTN_Y - 16 - MENU_TOP - (rows - 1) * MENU_GAP) / rows;
    *x = 12 + (k % MENU_COLS) * (*w + MENU_GAP);
    *y = MENU_TOP + (k / MENU_COLS) * (*h + MENU_GAP);
}

void Ui::drawMenu() {
    c.fill(0, 0, W, H, BG);
    int n = popcount(menuMask);
    c.text(font_title, 12, 30, "Channel rows", INK);
    char s[48];
    if (n == Settings::ROWS) snprintf(s, sizeof(s), "%d selected", n);
    else snprintf(s, sizeof(s), "Pick %d (%d selected)", Settings::ROWS, n);
    c.text(font_small, 12, 50, s, INK2);
    for (int k = 0; k < PLATFORM_COUNT; k++) {
        int x, y, w, h;
        menuCell(k, &x, &y, &w, &h);
        bool on = menuMask & (1u << k);
        c.roundRect(x, y, w, h, 6, on ? GOOD : rgb(32, 35, 40));
        c.text(font_label, x + w / 2, y + h / 2 + 5, PLATFORMS[k].label, on ? rgb(255, 255, 255) : INK2, CENTER);
    }
    const int bw = (W - 2 * 12 - MENU_GAP) / 2;
    c.roundRect(12, MENU_BTN_Y, bw, MENU_BTN_H, 6, rgb(32, 35, 40));
    c.text(font_label, 12 + bw / 2, MENU_BTN_Y + MENU_BTN_H / 2 + 5, "Cancel", INK, CENTER);
    bool ok = n == Settings::ROWS;
    c.roundRect(12 + bw + MENU_GAP, MENU_BTN_Y, bw, MENU_BTN_H, 6, ok ? rgb(56, 152, 255) : rgb(32, 35, 40));
    c.text(font_label, 12 + bw + MENU_GAP + bw / 2, MENU_BTN_Y + MENU_BTN_H / 2 + 5, "Done", ok ? rgb(255, 255, 255) : MUTED, CENTER);
}

bool Ui::menuTap(int x, int y, bool *settingsChanged) {
    *settingsChanged = false;
    if (y >= MENU_BTN_Y - 4) {
        const int bw = (W - 2 * 12 - MENU_GAP) / 2;
        if (x < 12 + bw + MENU_GAP / 2) {
            menuOpen = false;  // Cancel
        } else if (popcount(menuMask) == Settings::ROWS) {
            // Rows in the platforms' order.
            int r = 0;
            for (int k = 0; k < PLATFORM_COUNT && r < Settings::ROWS; k++)
                if (menuMask & (1u << k)) set.rows[r++] = (uint8_t)k;
            selRow = selCh = -1;
            menuOpen = false;
            *settingsChanged = true;
        }
        return true;
    }
    for (int k = 0; k < PLATFORM_COUNT; k++) {
        int cx, cy, w, h;
        menuCell(k, &cx, &cy, &w, &h);
        if (x >= cx - MENU_GAP / 2 && x < cx + w + MENU_GAP / 2 && y >= cy - MENU_GAP / 2 && y < cy + h + MENU_GAP / 2) {
            uint32_t bit = 1u << k;
            if (menuMask & bit) menuMask &= ~bit;
            else if (popcount(menuMask) < Settings::ROWS) menuMask |= bit;
            return true;
        }
    }
    return false;
}
