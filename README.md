# Widscan58 for Android

An Android app for [Widscan58](https://github.com/CapnBry/widscan58), the
ESP32-C5 FPV channel scanner. Plug the board into your phone and the app
opens with the Widscan58 scan screen, connected over USB.

## Install

1. Download the latest `widscan58-*.apk` from Releases and open it on the phone
   (allow installing from your browser or file manager when asked).
2. Plug the board into the phone with a USB-C OTG cable (or a C-to-C cable).
3. Android asks whether to open Widscan58 for the board. Tick
   **Always open Widscan58 when this device is connected** and tap OK.

From then on, plugging the board in opens the app and it connects by itself.
The board's firmware is flashed from the app (**Flash device**), the same as
from the web page.

## How it works

The app shows the unmodified Widscan58 web UI, built from source and bundled
in the APK, so it works without internet. The web UI already talks to the board
over USB through WebUSB in Chrome on Android; Android's in-app WebView has no
WebUSB, so `app/src/main/assets/bridge/android-bridge.js` provides the part of
WebUSB the UI uses on top of Android's USB host API (`UsbBridge.java`). It also
routes lap-timer voice callouts to Android's text-to-speech and saves session
recordings to Downloads.

The app adds a **Channels** tab next to the scanner: a chart in the style of
[Oscar Liang's 5.8 GHz channel chart](https://oscarliang.com/fpv-channels/)
with a row per platform and mode (analog bands, DJI V1/Vista 25 and 50 Mbps,
DJI O3/O4 10, 20 and 40 MHz, HDZero, Walksnail race mode and standard 25 and
50 Mbps; FCC or CE), each channel coloured by how far its mean power over the
platform's bandwidth sits above the band's noise floor, plus the clearest
channel of each platform. **Platforms** picks the rows; it reads the band
panorama of monitor mode, so it only sees the parts of the band around the
channels being scanned (`app/src/main/assets/bridge/channels-view.js`).

The board is matched by Espressif's USB vendor ID (`res/xml/device_filter.xml`).

## Build

```sh
FIRMWARE=pages scripts/fetch-webui.sh   # widscan58 web UI (+ firmware) into the assets
./gradlew assembleRelease               # app/build/outputs/apk/release/app-release.apk
```

Needs JDK 17+, Node 22 and the Android SDK. Without `FIRMWARE=pages` the APK
carries no firmware and the flasher downloads it from the published web UI.
GitHub Actions (`.github/workflows/build.yml`) builds and releases the APK on
every push to `main`, and on demand from the Actions tab to pick up a newer
widscan58.

The APK is signed with the committed `app/debug.keystore` (not a secret) so
every build installs over the previous one.

## License

GPL-3.0-or-later, like Widscan58, whose web UI the APK bundles.
