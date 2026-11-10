// GPL-3.0-or-later.
#include "display.h"
#include <Arduino.h>
#include "board.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"

static const int W = 600, H = 450;
static const int Y_GAP = 16;     // the panel's first row in landscape (Waveshare's examples)
static const int CHUNK_ROWS = 16;

static spi_device_handle_t dev;

// QSPI framing: a write is opcode 0x02 (registers, one line) or 0x32 (pixels,
// data on four lines), then the command in the middle byte of a 24-bit address.
//
// Pixels go out as queued transactions, two band buffers in turn, each with
// its own window commands queued ahead of it: the bus never waits on the CPU
// and the CPU only waits when it wants a buffer that is still being sent.
struct Slot {
    spi_transaction_t t[3];  // column window, row window, pixels
    uint16_t *buf;
    bool busy;
};
static Slot slots[2];
static int nextSlot;
static int inflight;  // queued transactions not yet collected

static void collectOne() {
    spi_transaction_t *r;
    spi_device_get_trans_result(dev, &r, portMAX_DELAY);
    inflight--;
    if (r->user) ((Slot *)r->user)->busy = false;
}

static void collectAll() {
    while (inflight) collectOne();
}

static void cmd(uint8_t c, const uint8_t *data = nullptr, size_t n = 0) {
    collectAll();
    spi_transaction_t t = {};
    t.cmd = 0x02;
    t.addr = (uint32_t)c << 8;
    if (n <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        for (size_t i = 0; i < n; i++) t.tx_data[i] = data[i];
    } else {
        t.tx_buffer = data;
    }
    t.length = n * 8;
    spi_device_polling_transmit(dev, &t);
}

static void windowTrans(spi_transaction_t &t, uint8_t c, int a, int b) {
    t = {};
    t.flags = SPI_TRANS_USE_TXDATA;
    t.cmd = 0x02;
    t.addr = (uint32_t)c << 8;
    t.tx_data[0] = (uint8_t)(a >> 8);
    t.tx_data[1] = (uint8_t)a;
    t.tx_data[2] = (uint8_t)(b >> 8);
    t.tx_data[3] = (uint8_t)b;
    t.length = 32;
}

bool displayInit() {
    spi_bus_config_t bus = {};
    bus.sclk_io_num = PIN_LCD_CLK;
    bus.data0_io_num = PIN_LCD_D0;
    bus.data1_io_num = PIN_LCD_D1;
    bus.data2_io_num = PIN_LCD_D2;
    bus.data3_io_num = PIN_LCD_D3;
    bus.max_transfer_sz = W * CHUNK_ROWS * 2;
    bus.flags = SPICOMMON_BUSFLAG_QUAD;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    spi_device_interface_config_t dc = {};
    dc.command_bits = 8;
    dc.address_bits = 24;
    dc.mode = 0;
    dc.clock_speed_hz = 40 * 1000 * 1000;
    dc.spics_io_num = PIN_LCD_CS;
    dc.queue_size = 8;
    dc.flags = SPI_DEVICE_HALFDUPLEX;
    if (spi_bus_add_device(SPI2_HOST, &dc, &dev) != ESP_OK) return false;
    for (auto &sl : slots) {
        sl.buf = (uint16_t *)heap_caps_malloc(W * CHUNK_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!sl.buf) return false;
    }

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
    Slot &sl = slots[nextSlot];
    while (sl.busy) collectOne();
    return sl.buf;
}

/** Queues the next slot's buffer (from displayBand) as frame rows y.. */
static void queueBand(int y, int rows) {
    Slot &sl = slots[nextSlot];
    nextSlot ^= 1;
    windowTrans(sl.t[0], 0x2a, 0, W - 1);
    windowTrans(sl.t[1], 0x2b, y + Y_GAP, y + rows - 1 + Y_GAP);
    spi_transaction_t &p = sl.t[2];
    p = {};
    p.flags = SPI_TRANS_MODE_QIO;
    p.cmd = 0x32;
    p.addr = 0x2c << 8;
    p.tx_buffer = sl.buf;
    p.length = W * rows * 16;
    p.user = &sl;
    sl.busy = true;
    for (auto &t : sl.t) {
        spi_device_queue_trans(dev, &t, portMAX_DELAY);
        inflight++;
    }
}

void displaySendBand(uint16_t *buf, int y, int rows) {
    // The panel takes RGB565 big-endian; two pixels per 32-bit word.
    uint32_t *p = (uint32_t *)buf;
    for (int i = 0; i < W * rows / 2; i++) {
        uint32_t v = p[i];
        p[i] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
    }
    queueBand(y, rows);
}

void displayFlush(const uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y += CHUNK_ROWS) {
        int rows = y1 - y < CHUNK_ROWS ? y1 - y : CHUNK_ROWS;
        uint16_t *buf = displayBand();
        // The panel takes RGB565 big-endian.
        const uint16_t *src = fb + y * W;
        for (int i = 0; i < W * rows; i++) buf[i] = (uint16_t)(src[i] << 8 | src[i] >> 8);
        queueBand(y, rows);
    }
}

void displayBrightness(uint8_t level) { cmd(0x51, &level, 1); }

void displaySleep(bool sleep) {
    cmd(sleep ? 0x28 : 0x29);
    cmd(sleep ? 0x10 : 0x11);
    delay(sleep ? 5 : 80);
}
