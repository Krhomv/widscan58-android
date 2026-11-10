// GPL-3.0-or-later.
#include "display.h"
#include <Arduino.h>
#include "board.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "freertos/semphr.h"

static const int W = 600, H = 450;
static const int Y_GAP = 16;     // the panel's first row in landscape (Waveshare's examples)
static const int CHUNK_ROWS = 16;

static esp_lcd_panel_io_handle_t io;
static uint16_t *chunk[2];
static SemaphoreHandle_t chunkFree;  // counts free chunk buffers

// QSPI framing: a write is opcode 0x02 (registers) or 0x32 (pixels, four lines),
// then the command in the middle byte of a 24-bit address.
static void cmd(uint8_t c, const uint8_t *data = nullptr, size_t n = 0) {
    esp_lcd_panel_io_tx_param(io, 0x02 << 24 | c << 8, data, n);
}

static bool IRAM_ATTR doneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(chunkFree, &woken);
    return woken == pdTRUE;
}

bool displayInit() {
    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_CLK;
    bus.data0_io_num = PIN_LCD_D0;
    bus.data1_io_num = PIN_LCD_D1;
    bus.data2_io_num = PIN_LCD_D2;
    bus.data3_io_num = PIN_LCD_D3;
    bus.max_transfer_sz = W * CHUNK_ROWS * 2;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    chunkFree = xSemaphoreCreateCounting(2, 2);
    esp_lcd_panel_io_spi_config_t cfg = {};
    cfg.cs_gpio_num = PIN_LCD_CS;
    cfg.dc_gpio_num = -1;
    cfg.spi_mode = 0;
    cfg.pclk_hz = 40 * 1000 * 1000;
    cfg.trans_queue_depth = 4;
    cfg.on_color_trans_done = doneCb;
    cfg.lcd_cmd_bits = 32;
    cfg.lcd_param_bits = 8;
    cfg.flags.quad_mode = true;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &cfg, &io) != ESP_OK) return false;
    for (auto &c : chunk) c = (uint16_t *)heap_caps_malloc(W * CHUNK_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!chunk[0] || !chunk[1]) return false;

    exioWrite(EXIO_PANEL_RESET, true);
    delay(20);
    exioWrite(EXIO_PANEL_RESET, false);
    delay(20);
    exioWrite(EXIO_PANEL_RESET, true);
    delay(120);

    // Waveshare's init sequence for this panel, rotated to landscape.
    cmd(0x01);  // software reset
    delay(80);
    const uint8_t madctl0 = 0x00, colmod = 0x55;
    cmd(0x36, &madctl0, 1);
    cmd(0x3a, &colmod, 1);
    struct {
        uint8_t c;
        uint8_t d[4];
        uint8_t n;
        uint16_t ms;
    } seq[] = {
        {0xfe, {0x20}, 1, 0},
        {0x26, {0x0a}, 1, 0},
        {0x24, {0x80}, 1, 0},
        {0xfe, {0x00}, 1, 0},
        {0x3a, {0x55}, 1, 0},  // RGB565
        {0xc2, {0x00}, 1, 10},
        {0x35, {0x00}, 0, 0},  // tearing effect line on
        {0x51, {0x00}, 1, 10}, // brightness 0 until the first frame is up
        {0x11, {0x00}, 0, 80}, // sleep out
        {0x2a, {0x00, 0x10, 0x00, 0xd1}, 4, 0},  // as Waveshare's; every flush sets its own window
        {0x2b, {0x00, 0x00, 0x00, 0x57}, 4, 0},
        {0x29, {0x00}, 0, 10}, // display on
        {0x36, {0x30}, 1, 0},  // landscape
    };
    for (auto &s : seq) {
        cmd(s.c, s.n ? s.d : nullptr, s.n);
        if (s.ms) delay(s.ms);
    }
    return true;
}

int displayBandRows() { return CHUNK_ROWS; }

uint16_t *displayBand() {
    xSemaphoreTake(chunkFree, portMAX_DELAY);
    static int next = 0;
    uint16_t *buf = chunk[next];
    next ^= 1;
    return buf;
}

static void sendChunk(uint16_t *buf, int y, int rows) {
        uint8_t ca[4] = {0, 0, (W - 1) >> 8, (W - 1) & 0xff};
        int r0 = y + Y_GAP, r1 = y + rows - 1 + Y_GAP;
        uint8_t ra[4] = {(uint8_t)(r0 >> 8), (uint8_t)r0, (uint8_t)(r1 >> 8), (uint8_t)r1};
        cmd(0x2a, ca, 4);
        cmd(0x2b, ra, 4);
        esp_lcd_panel_io_tx_color(io, 0x32 << 24 | 0x2c << 8, buf, W * rows * 2);
}

void displaySendBand(uint16_t *buf, int y, int rows) {
    // The panel takes RGB565 big-endian; two pixels per 32-bit word.
    uint32_t *p = (uint32_t *)buf;
    for (int i = 0; i < W * rows / 2; i++) {
        uint32_t v = p[i];
        p[i] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
    }
    sendChunk(buf, y, rows);
}

void displayFlush(const uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y += CHUNK_ROWS) {
        int rows = y1 - y < CHUNK_ROWS ? y1 - y : CHUNK_ROWS;
        uint16_t *buf = displayBand();
        // The panel takes RGB565 big-endian.
        const uint16_t *src = fb + y * W;
        for (int i = 0; i < W * rows; i++) buf[i] = (uint16_t)(src[i] << 8 | src[i] >> 8);
        sendChunk(buf, y, rows);
    }
}

void displayBrightness(uint8_t level) { cmd(0x51, &level, 1); }

void displaySleep(bool sleep) {
    cmd(sleep ? 0x28 : 0x29);
    cmd(sleep ? 0x10 : 0x11);
    delay(sleep ? 5 : 80);
}
