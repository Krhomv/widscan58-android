// Widscan58 handheld: an ESP32-S3 AMOLED board showing what a widscan58
// ESP32-C5 scanner sees, read from the scanner's UART. The C5 scans; this
// board puts it in monitor mode, parses its frames and draws them.
// GPL-3.0-or-later.
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <new>
#include "board.h"
#include "display.h"
#include "esp_log.h"
#include "platforms.h"
#include "proto.h"
#include "scanner.h"
#include "touch.h"
#include "ui.h"

static HardwareSerial Link(1);
static SemaphoreHandle_t lock;  // the scanner model, between the link task and the UI
static Scanner scanner;
static Settings settings;
static Preferences prefs;
static uint16_t *fb;
static Canvas *canvas;
static Ui *ui;

// ---- Link: bytes from the C5 into the scanner model (its own task) ----

static void onFrame(void *, uint8_t type, const uint8_t *p, uint16_t len) { scanner.onFrame(type, p, len, millis()); }
static void onLine(void *, const char *line) { scanner.onLine(line, millis()); }
static StreamParser parser(onFrame, onLine, nullptr);

static void linkTask(void *) {
    static uint8_t buf[1024];
    for (;;) {
        int n = Link.available();
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        n = (int)Link.read(buf, n < (int)sizeof(buf) ? n : sizeof(buf));
        xSemaphoreTake(lock, portMAX_DELAY);
        parser.push(buf, n);
        xSemaphoreGive(lock);
    }
}

/** Puts the scanner in monitor mode (it boots in lap timer mode, silent) and
 *  makes this UART the port it answers on. One write: a lone byte after a
 *  pause could look like a RotorHazard probe. */
static void claimScanner() { Link.print("PROTOCOL MONITOR\nSCAN START\nINFO\n"); }
static void idleScanner() {
    Link.printf("PROTOCOL MONITOR\nSCAN STOP\nGPIO %d HIGH\n", C5_LED_GPIO);  // the LED is active low
    Link.flush();
}

// ---- Settings ----

static void loadSettings() {
    memcpy(settings.rows, DEFAULT_ROWS, sizeof(settings.rows));
    settings.brightness = 0;
    prefs.begin("handheld", false);
    uint8_t rows[Settings::ROWS];
    if (prefs.getBytes("rows", rows, sizeof(rows)) == sizeof(rows)) {
        bool ok = true;
        for (uint8_t r : rows) ok &= r < PLATFORM_COUNT;
        if (ok) memcpy(settings.rows, rows, sizeof(rows));
    }
    uint8_t b = prefs.getUChar("bright", 0);
    settings.brightness = b < BRIGHTNESS_LEVELS ? b : 0;
}

static void saveSettings() {
    prefs.putBytes("rows", settings.rows, sizeof(settings.rows));
    prefs.putUChar("bright", settings.brightness);
}

// ---- Battery ----

static int batteryPercent(float v) {
    // A typical 18650 discharge curve at light load, empty at 3.0 V (the C5
    // and the screen both run down to there).
    static const float V[] = {3.00f, 3.30f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.20f};
    static const int P[] = {0, 5, 12, 25, 45, 62, 77, 90, 100};
    if (v <= V[0]) return 0;
    for (int k = 1; k < 9; k++)
        if (v < V[k]) return P[k - 1] + (int)((v - V[k - 1]) / (V[k] - V[k - 1]) * (P[k] - P[k - 1]));
    return 100;
}

static void flushRegions(int mask) {
    for (Region r : {R_STATUS, R_PLOT, R_ROWS})
        if (mask & r) {
            RegionRows rr = regionRows(r);
            displayFlush(fb, rr.y0, rr.y1);
        }
}

static void message(const char *text) {
    canvas->fill(0, 0, Ui::W, Ui::H, 0);
    canvas->text(font_big, Ui::W / 2, Ui::H / 2 + 8, text, rgb(232, 234, 237), CENTER);
    displayFlush(fb, 0, Ui::H);
}

/** Power off from the battery. On USB power the board stays up: sleep the
 *  panel until the power key is pressed again. */
static void powerOff(const char *why) {
    message(why);
    delay(800);
    displayBrightness(0);
    // On USB the board stays powered after boardPowerOff, so idle the C5
    // too and turn its LED off (not saved: it comes back if the C5 reboots),
    // repeating while off in case a command is lost.
    idleScanner();
    boardPowerOff();
    delay(300);
    displaySleep(true);
    while (powerKeyDown()) delay(20);
    for (int t = 0; !powerKeyDown(); t++) {
        if (t % 100 == 99) idleScanner();
        delay(20);
    }
    displaySleep(false);
    displayBrightness(BRIGHTNESS[settings.brightness]);
    while (powerKeyDown()) delay(20);
    boardPowerHold();
    Link.printf("GPIO %d LED\n", C5_LED_GPIO);
    claimScanner();
}

void setup() {
    boardPowerHold();  // first: on battery the board is only powered while the key is held
    esp_log_level_set("*", ESP_LOG_NONE);
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);  // USB CDC: status lines for debugging
    Serial.setTxTimeoutMs(0);  // never wait for a PC that is not reading
    lock = xSemaphoreCreateMutex();

    Link.setRxBufferSize(16384);
    Link.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

    loadSettings();
    boardI2cInit();
    fb = (uint16_t *)heap_caps_malloc(Ui::W * Ui::H * 2, MALLOC_CAP_SPIRAM);
    if (!fb || !displayInit()) {
        Serial.println("display init failed");
        for (;;) delay(1000);
    }
    canvas = new Canvas(fb, Ui::W, Ui::H);
    // In internal RAM if it fits (large allocations default to PSRAM): the
    // plot reads its arrays for every band.
    void *uiMem = heap_caps_calloc(1, sizeof(Ui), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ui = uiMem ? new (uiMem) Ui(*canvas, settings) : new Ui(*canvas, settings);
    touchInit();
    message("Widscan58");
    displayBrightness(BRIGHTNESS[settings.brightness]);

    xTaskCreatePinnedToCore(linkTask, "link", 4096, nullptr, 5, nullptr, 0);
    analogReadResolution(12);
}

void loop() {
    static uint32_t lastDraw, lastStatus, lastRows, lastTouchPoll, lastBattery, lastActivity, lastLog;
    static uint32_t claimAt = 1500, claims, lowSince, keyDownAt;
    static bool touching, dimmed, keyArmed, screenOff;
    static int touchX, touchY, forced = R_STATUS | R_PLOT | R_ROWS;
    static unsigned frames;
    uint32_t now = millis();

    // Power key: a long press turns off; a short one turns the screen off and on.
    bool key = powerKeyDown();
    if (!key) {
        if (keyArmed && keyDownAt && now - keyDownAt < 1500) {
            screenOff = !screenOff;
            displayBrightness(screenOff ? 0 : BRIGHTNESS[settings.brightness]);
            lastActivity = now;
            dimmed = false;
        }
        keyArmed = true;  // the press that powered the board on doesn't count
        keyDownAt = 0;
    } else if (keyArmed) {
        if (!keyDownAt) keyDownAt = now;
        else if (now - keyDownAt > 2000) {
            powerOff("Power off");
            keyDownAt = 0;
            keyArmed = false;
            forced = R_STATUS | R_PLOT | R_ROWS;
            lastActivity = millis();
        }
    }

    // Touch: act on release, so a press can wake a dimmed screen without doing anything else.
    if (now - lastTouchPoll >= 20) {
        lastTouchPoll = now;
        int x, y;
        bool down = touchRead(&x, &y);
        if (down) {
            touchX = x;
            touchY = y;
        }
        if (touching && !down && !screenOff) {
            if (dimmed) {
                dimmed = false;
                displayBrightness(BRIGHTNESS[settings.brightness]);
            } else {
                bool changed, reset, retry;
                forced |= ui->tap(touchX, touchY, now, &changed, &reset, &retry);
                if (changed) {
                    saveSettings();
                    displayBrightness(BRIGHTNESS[settings.brightness]);
                }
                if (reset) {
                    xSemaphoreTake(lock, portMAX_DELAY);
                    scanner.resetHolds();
                    xSemaphoreGive(lock);
                }
                if (retry) {
                    claims = 0;
                    claimAt = now;
                }
            }
            lastActivity = now;
        }
        touching = down;
    }
    // Dim after 3 minutes without a touch (and less OLED wear).
    if (!dimmed && !screenOff && now - lastActivity > 180000) {
        dimmed = true;
        displayBrightness(BRIGHTNESS[BRIGHTNESS_LEVELS - 1]);
    }

    // Battery, every 2 s. Off below 3.0 V for 20 s, to spare the cell.
    if (now - lastBattery >= 2000 || !lastBattery) {
        // A gap (boot, or a power off on USB): let the cell settle again.
        bool gap = !lastBattery || now - lastBattery > 5000;
        lastBattery = now;
        float v = batteryVolts();
        // The charger's status pin only drives its LED, so charging is read
        // two ways: a USB host (a PC) is seen directly; a plain charger only
        // by the cell voltage, which steps up when it's plugged in (the
        // charge current through the cell's resistance) and down when it's
        // unplugged. A drop near 4.2 V is the charge ending. A charger that
        // was already in at boot can't be seen until it's unplugged.
        static float vAvg, vBefore, rise = 0.2f;
        static int settle, riseIn;
        static bool charging, full, host;
        if (gap) settle = 5;  // the first ~10 s: the cell recovers from the boot load
        bool hostNow = Serial.isPlugged();
        if (v > 4.35f) {  // no cell: the charger's output
            ui->usbPower = true;
            charging = full = false;
        } else if (settle > 0) {
            ui->usbPower = false;
            settle--;
            vAvg = v;
            charging = hostNow;
        } else {
            ui->usbPower = false;
            float step = v - vAvg;
            if (hostNow) {
                if (!charging) full = false;
                charging = true;
            } else if (host) {
                charging = false;  // unplugged from the PC
            } else if (!charging && step > 0.04f) {
                charging = true;
                full = false;
                vBefore = vAvg;  // measure the lift once it has settled
                riseIn = 5;
            } else if (charging && step < -0.04f) {
                charging = false;
                full = vAvg > 4.12f;  // still on USB, done
            }
            vAvg += 0.25f * (v - vAvg);
            if (riseIn > 0 && --riseIn == 0 && charging) {
                float r = vAvg - vBefore;
                rise = r < 0.05f ? 0.05f : r > 0.35f ? 0.35f : r;
            }
            if (full && v < 4.0f) full = false;  // unplugged and used since
        }
        host = hostNow;
        ui->charging = charging;
        ui->full = full;
        // While charging the cell reads high by the lift measured at plug-in
        // (about 0.2 V on the first unit), shrinking as the charge current
        // tapers off near 4.2 V.
        float taper = (4.2f - v) / 0.3f;
        taper = taper < 0 ? 0 : taper > 1 ? 1 : taper;
        ui->batteryPct = full ? 100 : batteryPercent(charging ? v - rise * taper : v);
        if (!charging && v > 2.5f && v < 3.0f) {
            if (!lowSince) lowSince = now;
            else if (now - lowSince > 20000) {
                powerOff("Battery empty");
                forced = R_STATUS | R_PLOT | R_ROWS;
                lowSince = 0;
            }
        } else {
            lowSince = 0;
        }
    }

    if (screenOff || now - lastDraw < 33) {
        delay(1);
        return;
    }
    lastDraw = now;

    static uint32_t usUpdate, usDraw, usFlush, usWait, usBandDraw, usSend;
    uint32_t t0 = micros();
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t lastFrame = scanner.lastFrameMs;
    ui->update(scanner, now);
    xSemaphoreGive(lock);
    uint32_t t1 = micros();
    usUpdate += t1 - t0;

    // The link: claim the scanner at boot and whenever its data stops. Until
    // the first frame it keeps trying every 3 s (the C5 may boot later, or
    // miss the first claims while it starts). Once data has come and gone,
    // five tries, then wait for a tap: the phone app may be using it over USB.
    // Signed: the link task can stamp a frame after this loop read `now`.
    bool fresh = lastFrame && (int32_t)(now - lastFrame) < 2000;
    if (fresh) {
        claims = 0;
        ui->link = LINK_OK;
    } else if (!lastFrame || claims < 5) {
        if ((int32_t)(now - claimAt) >= 0) {
            claimScanner();
            claims++;
            claimAt = now + 3000;
        }
        ui->link = claims > 5 ? LINK_LOST : LINK_WAITING;
    } else {
        ui->link = LINK_LOST;
    }

    int mask = R_PLOT | forced;
    if (now - lastStatus >= 500) mask |= R_STATUS;
    if (now - lastRows >= 250) mask |= R_ROWS;
    if (mask & R_STATUS) lastStatus = now;
    if (mask & R_ROWS) lastRows = now;
    forced = 0;
    uint32_t t2 = micros();
    // Status and rows (a few times a second): drawn in the frame buffer.
    int fbMask = mask & ~R_PLOT;
    if (fbMask) {
        ui->draw(fbMask, now);
        flushRegions(fbMask);
    }
    uint32_t t3 = micros();
    // The plot, every frame: drawn band by band straight into the panel's DMA
    // buffers in internal RAM (the PSRAM frame buffer was the bottleneck),
    // one band drawn while the previous one is sent.
    ui->preparePlot();
    RegionRows rr = regionRows(R_PLOT);
    const int bandRows = displayBandRows();
    for (int y = rr.y0; y < rr.y1; y += bandRows) {
        int rows = rr.y1 - y < bandRows ? rr.y1 - y : bandRows;
        uint32_t b0 = micros();
        uint16_t *band = displayBand();
        uint32_t b1 = micros();
        canvas->setBand(band, y, y + rows);
        ui->drawPlot(now);
        uint32_t b2 = micros();
        displaySendBand(band, y, rows);
        usWait += b1 - b0;
        usBandDraw += b2 - b1;
        usSend += micros() - b2;
    }
    canvas->setFull(fb);
    usDraw += micros() - t3;
    usFlush += t3 - t2;
    frames++;

    if (now - lastLog >= 5000) {
        // Only when a terminal has room for it: a full USB buffer must never stall the screen.
        char line[256];
        unsigned f = frames ? frames : 1;
        int n = snprintf(line, sizeof(line),
                         "fps %.1f @%u MHz (update %u, plot %u = wait %u + draw %u + send %u, status+rows %u us), frames %u, crc errors %u, skipped %u, "
                         "rotations %.1f/s, link drops %u\n",
                         frames * 1000.0f / (now - lastLog), (unsigned)getCpuFrequencyMhz(), (unsigned)(usUpdate / f), (unsigned)(usDraw / f),
                         (unsigned)(usWait / f), (unsigned)(usBandDraw / f), (unsigned)(usSend / f),
                         (unsigned)(usFlush / f), (unsigned)parser.frames, (unsigned)parser.crcErrors,
                         (unsigned)parser.skipped, scanner.rotationHz, (unsigned)scanner.drops);
        if (Serial && Serial.availableForWrite() >= n) Serial.write((const uint8_t *)line, n);
        lastLog = now;
        frames = 0;
        usUpdate = usDraw = usFlush = usWait = usBandDraw = usSend = 0;
    }
}
