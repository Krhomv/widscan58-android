// The 600x450 screen: status bar, band spectrum with max hold, waterfall, and
// a row of channel boxes per FPV platform coloured by occupancy. Drawing and
// touch only; main.cpp owns the hardware. GPL-3.0-or-later.
#pragma once
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "gfx.h"
#include "scanner.h"

struct Settings {
    static const int ROWS = 3;
    uint8_t rows[ROWS];  // indexes into PLATFORMS
    uint8_t brightness;  // index into BRIGHTNESS
};
static const uint8_t BRIGHTNESS[] = {255, 150, 70, 25};
static const int BRIGHTNESS_LEVELS = sizeof(BRIGHTNESS);

enum LinkState { LINK_WAITING, LINK_OK, LINK_LOST };

// Screen regions, flushed separately. Even rows: the panel's windows start
// and end on even coordinates.
enum Region { R_STATUS = 1, R_PLOT = 2, R_ROWS = 4 };
struct RegionRows {
    int y0, y1;  // [y0, y1)
};
RegionRows regionRows(Region r);

class Ui {
public:
    static const int W = 600, H = 450;
    static const int LX = 74, PW = 520;  // plot left and width
    Ui(Canvas &c, Settings &s) : c(c), set(s) { memset(wf, WF_EMPTY, sizeof(wf)); }

    /** Takes what it needs from the scanner (call with it locked). */
    void update(const Scanner &sc, uint32_t nowMs);
    /** Draws the regions in mask; returns them. */
    int draw(int mask, uint32_t nowMs);
    /** The plot in bands: preparePlot once per frame, then drawPlot for each
     *  band the canvas is set to (Canvas::setBand). */
    void preparePlot();
    void drawPlot(uint32_t nowMs);

    /** A tap at x, y; returns the regions to redraw. *settingsChanged when
     *  settings must be saved, *resetHolds / *retry for the caller to act on. */
    int tap(int x, int y, uint32_t nowMs, bool *settingsChanged, bool *resetHolds, bool *retry);

    // Fed by the caller.
    LinkState link = LINK_WAITING;
    int batteryPct = -1;  // -1: unknown
    float batteryV = 0;   // the cell, as read (high while charging)
    bool usbPower = false;
    bool charging = false, full = false;  // on USB, with a cell

private:
    Canvas &c;
    Settings &set;

    float live[PW], maxH[PW], avgLin[PW];  // dBFS, dBFS, linear power
    float floorDb = NAN;    // band noise floor, smoothed
    float bottomDb = -100;  // spectrum scale
    float rotationHz = 0;
    uint32_t drops = 0, dropsAt = 0, lastDrops = 0;
    bool scanning = true;

    static const int WF_ROWS = 98;
    static const uint8_t WF_EMPTY = 0;  // drawn black, as before any data
    uint8_t wf[WF_ROWS][PW];  // newest at wfHead; 0 = not scanned, else spectrum code
    int wfHead = 0;
    uint32_t wfLastMs = 0;

    struct Level {
        float db;  // above the floor; NAN when not scanned
    };
    Level lev[Settings::ROWS][10];
    int selRow = -1, selCh = -1;
    uint32_t toastUntil = 0;
    const char *toast = "";

    float fx(float mhz) const;
    void drawStatus(uint32_t nowMs);
    void drawRows();
    int16_t yl[PW], ym[PW];  // trace rows, -1 where not scanned
    uint16_t wfLut[256];
    void boxGeometry(int row, int ch, int *x, int *w) const;
};
