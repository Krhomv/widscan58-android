// GPL-3.0-or-later.
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

static uint8_t exioState = 0;

void boardPowerHold() {
    pinMode(PIN_BAT_CONTROL, OUTPUT);
    digitalWrite(PIN_BAT_CONTROL, HIGH);
    pinMode(PIN_POWER_KEY, INPUT_PULLUP);
}

void boardPowerOff() { digitalWrite(PIN_BAT_CONTROL, LOW); }

static void tcaWrite(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(reg);
    Wire.write(v);
    Wire.endTransmission();
}

void boardI2cInit() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    tcaWrite(0x01, exioState);  // outputs low
    // Panel reset, touch reset and EXIO5 as outputs, as Waveshare's examples.
    tcaWrite(0x03, (uint8_t) ~((1 << EXIO_PANEL_RESET) | (1 << EXIO_TOUCH_RESET) | (1 << 5)));
}

void exioWrite(int pin, bool high) {
    if (high) exioState |= 1 << pin;
    else exioState &= ~(1 << pin);
    tcaWrite(0x01, exioState);
}

float batteryVolts() {
    uint32_t mv = 0;
    for (int k = 0; k < 32; k++) mv += analogReadMilliVolts(PIN_BAT_ADC);
    return mv / 32 * 3 / 1000.0f;
}

bool powerKeyDown() { return digitalRead(PIN_POWER_KEY) == LOW; }
