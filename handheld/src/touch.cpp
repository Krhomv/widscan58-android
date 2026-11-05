// GPL-3.0-or-later.
#include "touch.h"
#include <Arduino.h>
#include <Wire.h>
#include "board.h"

void touchInit() {
    exioWrite(EXIO_TOUCH_RESET, true);
    delay(20);
    exioWrite(EXIO_TOUCH_RESET, false);
    delay(20);
    exioWrite(EXIO_TOUCH_RESET, true);
    delay(120);
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
}

bool touchRead(int *x, int *y) {
    Wire.beginTransmission(FT6336_ADDR);
    Wire.write(0x02);  // TD_STATUS, then P1_XH, P1_XL, P1_YH, P1_YL
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)FT6336_ADDR, (uint8_t)5) != 5) return false;
    uint8_t d[5];
    for (auto &b : d) b = Wire.read();
    int n = d[0] & 0x0f;
    if (n < 1 || n > 2) return false;
    int rx = (d[1] & 0x0f) << 8 | d[2];  // portrait: 0..449
    int ry = (d[3] & 0x0f) << 8 | d[4];  // portrait: 0..599
    // Landscape, as Waveshare's examples: mirror y, then swap.
    *x = 599 - ry;
    *y = rx;
    return true;
}
