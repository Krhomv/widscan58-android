// Waveshare ESP32-S3-Touch-AMOLED-2.41 (V2): pins and the parts around the
// display (from Waveshare's V2 schematic and examples). GPL-3.0-or-later.
#pragma once
#include <stdint.h>

// Power: the board stays on from the battery only while BAT_CONTROL is high
// (the power key turns it on; hold it until the screen lights).
#define PIN_BAT_CONTROL 16
#define PIN_POWER_KEY 15  // active low
#define PIN_BAT_ADC 17    // ADC2 channel 6, battery voltage / 3

// I2C: TCA9554 I/O expander (panel and touch resets), FT6336 touch, RTC, IMU.
#define PIN_I2C_SDA 47
#define PIN_I2C_SCL 48
#define TCA9554_ADDR 0x20
#define EXIO_PANEL_RESET 0
#define EXIO_TOUCH_RESET 1
#define FT6336_ADDR 0x38
#define PIN_TOUCH_INT 3
// The picture turned 180 degrees: USB-C at the top (the panel's native
// landscape has it at the bottom). Display and touch both follow it.
#define UPSIDE_DOWN 1

// RM690B0 AMOLED on QSPI.
#define PIN_LCD_CS 9
#define PIN_LCD_CLK 10
#define PIN_LCD_D0 11
#define PIN_LCD_D1 12
#define PIN_LCD_D2 13
#define PIN_LCD_D3 14

// The 4-pin UART connector (U0TXD / U0RXD): to the ESP32-C5.
#define PIN_LINK_TX 43
#define PIN_LINK_RX 44
#define LINK_BAUD 921600
#define C5_LED_GPIO 27  // the scanner's status LED (GPIO 27 LED saved on the C5); dark while powered off

void boardPowerHold();
void boardPowerOff();
void boardI2cInit();
void exioWrite(int pin, bool high);
/** Battery voltage in volts (from the divider; reads the charger's output on USB). */
float batteryVolts();
bool powerKeyDown();
