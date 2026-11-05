// RM690B0 600x450 AMOLED over QSPI (esp_lcd SPI panel IO, the SH8601-style
// command framing Waveshare's examples use), landscape. GPL-3.0-or-later.
#pragma once
#include <stdint.h>

bool displayInit();
/** Sends rows [y0, y1) of a 600-pixel-wide RGB565 frame buffer (y0, y1 even). */
void displayFlush(const uint16_t *fb, int y0, int y1);
void displayBrightness(uint8_t level);
void displaySleep(bool sleep);
