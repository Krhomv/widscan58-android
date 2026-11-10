// GPL-3.0-or-later.
#include "display.h"
#include <Arduino.h>
#include "board.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const int W = 600;
static const int Y_GAP = 16;     // the panel's first row in landscape (Waveshare's examples)
static const int CHUNK_ROWS = 16;
static const int SLOTS = 3;

// Everything on the SPI bus happens in one task on core 0 (its interrupts
// too), so drawing on core 1 never waits for the bus and the UART's
// interrupt on core 1 is not delayed by the SPI's.
//
// QSPI framing: a write is opcode 0x02 (registers, one line) or 0x32 (pixels,
// data on four lines), then the command in the middle byte of a 24-bit address.

static spi_device_handle_t dev;
static uint16_t *slots[SLOTS];
static int nextSlot;
static SemaphoreHandle_t freeSlots;  // band buffers the caller may draw into
static SemaphoreHandle_t jobDone;    // a synchronous job (command, init) finished
static QueueHandle_t jobs;
static bool initOk;

enum JobType : uint8_t { JOB_BAND, JOB_CMD, JOB_INIT };
struct Job {
    JobType type;
    bool swap;       // band: convert to big-endian first
    uint8_t c, n;    // command, data length
    uint8_t d[4];
    uint16_t delayMs;
    uint16_t *buf;
    int16_t y, rows;
};

static void busCmd(uint8_t c, const uint8_t *data, size_t n) {
    spi_transaction_t t = {};
    t.cmd = 0x02;
    t.addr = (uint32_t)c << 8;
    t.flags = SPI_TRANS_USE_TXDATA;
    for (size_t i = 0; i < n && i < 4; i++) t.tx_data[i] = data[i];
    t.length = n * 8;
    spi_device_transmit(dev, &t);
}

static void busWindow(uint8_t c, int a, int b) {
    uint8_t d[4] = {(uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(b >> 8), (uint8_t)b};
    busCmd(c, d, 4);
}

// Bands are queued, not waited for: the next band is byte-swapped while the
// previous one is on the bus. Each slot's three transactions (column window,
// row window, pixels) stay valid until collected.
static spi_transaction_t trans[SLOTS][3];
static int inflight;

static void collectOne() {
    spi_transaction_t *r;
    spi_device_get_trans_result(dev, &r, portMAX_DELAY);
    inflight--;
    if (r->user) xSemaphoreGive(freeSlots);  // a band's pixels: its buffer is free
}

static void collectAll() {
    while (inflight) collectOne();
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

static void busBand(uint16_t *buf, int y, int rows, bool swap) {
    if (swap) {
        // RGB565 big-endian; two pixels per 32-bit word.
        uint32_t *p = (uint32_t *)buf;
        for (int i = 0; i < W * rows / 2; i++) {
            uint32_t v = p[i];
            p[i] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
        }
    }
    int k = 0;
    while (k < SLOTS - 1 && slots[k] != buf) k++;
    spi_transaction_t *t = trans[k];
    windowTrans(t[0], 0x2a, 0, W - 1);
    windowTrans(t[1], 0x2b, y + Y_GAP, y + rows - 1 + Y_GAP);
    t[2] = {};
    t[2].flags = SPI_TRANS_MODE_QIO;
    t[2].cmd = 0x32;
    t[2].addr = 0x2c << 8;
    t[2].tx_buffer = buf;
    t[2].length = W * rows * 16;
    t[2].user = buf;
    for (int i = 0; i < 3; i++) {
        spi_device_queue_trans(dev, &t[i], portMAX_DELAY);
        inflight++;
    }
}

static void cmd(uint8_t c, const uint8_t *data = nullptr, size_t n = 0) { busCmd(c, data, n); }

static bool busInit() {
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
    dc.queue_size = SLOTS * 3 + 1;
    dc.flags = SPI_DEVICE_HALFDUPLEX;
    if (spi_bus_add_device(SPI2_HOST, &dc, &dev) != ESP_OK) return false;

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

static void displayTask(void *) {
    Job j;
    for (;;) {
        // Nothing to do but transfers in flight: collect them (frees buffers).
        if (inflight && !uxQueueMessagesWaiting(jobs)) {
            collectOne();
            continue;
        }
        xQueueReceive(jobs, &j, portMAX_DELAY);
        switch (j.type) {
            case JOB_INIT:
                initOk = busInit();
                xSemaphoreGive(jobDone);
                break;
            case JOB_BAND:
                busBand(j.buf, j.y, j.rows, j.swap);
                break;
            case JOB_CMD:
                collectAll();
                busCmd(j.c, j.d, j.n);
                if (j.delayMs) delay(j.delayMs);
                xSemaphoreGive(jobDone);
                break;
        }
    }
}

bool displayInit() {
    for (auto &b : slots) {
        b = (uint16_t *)heap_caps_malloc(W * CHUNK_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!b) return false;
    }
    freeSlots = xSemaphoreCreateCounting(SLOTS, SLOTS);
    jobDone = xSemaphoreCreateBinary();
    jobs = xQueueCreate(SLOTS + 2, sizeof(Job));
    xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 4, nullptr, 0);
    Job j = {};
    j.type = JOB_INIT;
    xQueueSend(jobs, &j, portMAX_DELAY);
    xSemaphoreTake(jobDone, portMAX_DELAY);
    return initOk;
}

/** A command, after every band already handed over; returns when sent. */
static void syncCmd(uint8_t c, const uint8_t *d, uint8_t n, uint16_t delayMs) {
    Job j = {};
    j.type = JOB_CMD;
    j.c = c;
    j.n = n;
    for (int i = 0; i < n && i < 4; i++) j.d[i] = d[i];
    j.delayMs = delayMs;
    xQueueSend(jobs, &j, portMAX_DELAY);
    xSemaphoreTake(jobDone, portMAX_DELAY);
}

int displayBandRows() { return CHUNK_ROWS; }

uint16_t *displayBand() {
    xSemaphoreTake(freeSlots, portMAX_DELAY);
    uint16_t *b = slots[nextSlot];
    nextSlot = (nextSlot + 1) % SLOTS;
    return b;
}

static void sendBand(uint16_t *buf, int y, int rows, bool swap) {
    Job j = {};
    j.type = JOB_BAND;
    j.swap = swap;
    j.buf = buf;
    j.y = (int16_t)y;
    j.rows = (int16_t)rows;
    xQueueSend(jobs, &j, portMAX_DELAY);
}

void displaySendBand(uint16_t *buf, int y, int rows) { sendBand(buf, y, rows, true); }

void displayFlush(const uint16_t *fb, int y0, int y1) {
    for (int y = y0; y < y1; y += CHUNK_ROWS) {
        int rows = y1 - y < CHUNK_ROWS ? y1 - y : CHUNK_ROWS;
        uint16_t *buf = displayBand();
        // The panel takes RGB565 big-endian.
        const uint16_t *src = fb + y * W;
        for (int i = 0; i < W * rows; i++) buf[i] = (uint16_t)(src[i] << 8 | src[i] >> 8);
        sendBand(buf, y, rows, false);
    }
}

void displayBrightness(uint8_t level) { syncCmd(0x51, &level, 1, 0); }

void displaySleep(bool sleep) {
    syncCmd(sleep ? 0x28 : 0x29, nullptr, 0, 0);
    syncCmd(sleep ? 0x10 : 0x11, nullptr, 0, sleep ? 5 : 80);
}
