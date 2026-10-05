# CrowPanel ESP32-S3 5.79" E-Paper Clock & Weather Panel

Firmware for the CrowPanel ESP32-S3-WROOM-1-N8R8 5.79" e-paper display that shows:

- Local time plus 4 configurable timezones, all in 24-hour format
- Current weather (via OpenWeatherMap)
- A freshness icon indicating whether the clock has been recently verified against NTP

## Behavior

- On boot: connects to WiFi, syncs time via NTP, fetches weather, and does a full-screen draw.
- Every 1 minute: redraws the display from the internally kept clock (partial refresh).
- Every 1 hour: re-verifies time against the configured local NTP source and refreshes weather.
- Every 30 minute-updates: does a full refresh cycle to clear any e-paper ghosting.
- The header icon shows a checkmark ("SYNCED") if the last successful NTP sync was within
  the staleness threshold (default 2 hours), or a cross ("STALE") otherwise.

## Setup

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI). If installed via
   `pip3 install --user platformio`, its `pio` binary is not on PATH by default — add it, e.g.:
   ```
   export PATH="$PATH:/Users/gareth/Library/Python/3.8/bin"
   ```
2. Edit [include/config.h](include/config.h):
   - `WIFI_SSID` / `WIFI_PASSWORD`
   - `NTP_SERVER_LOCAL` — your local network's NTP source (router, Pi-hole, etc.)
   - `TIMEZONES[4]` — the four configurable timezones, as POSIX TZ strings
   - `LOCAL_TZ_POSIX` — the timezone used for "local" time
   - `OWM_API_KEY` / `OWM_CITY_QUERY` — your [OpenWeatherMap](https://openweathermap.org/api) key and location
   - `DISPLAY_ROTATION` — default boot rotation (0 or 180); can also be toggled at runtime, see below
3. Build:
   ```
   pio run
   ```
4. Before uploading, put the board into bootloader mode: hold BOOT, tap RESET, then release BOOT.
   The serial port name is not stable across sessions — check it with `ls /dev/cu.*` (macOS) first.
   Then upload:
   ```
   pio run -t upload --upload-port /dev/cu.usbserial-XX
   ```
5. Monitor serial output:
   ```
   pio device monitor
   ```

## Controls

- **Menu button** (front-panel dial cluster), held during boot/reset: toggles display rotation
  (0°/180°) and persists the new value to NVS, so it survives future reboots without reflashing.
- **Dial down** (relative to the panel orientation; swapped automatically at 180°): opens the
  timezone info panel for the first configured zone - zone name, current UTC offset, next DST
  change (`yyyy-mm-dd hh:mm`, wall-clock time in that zone) and the offset after the change,
  relative to the local zone.
- **Dial in** (on the info panel): next configured timezone.
- **Dial up** (on the info panel): back to the main display. A reboot always starts on the main display.

POSIX TZ strings (with DST rules) can be looked up at:
https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv

## Project layout

- `src/main.cpp` — `setup()`/`loop()` orchestration only: init order, hourly NTP re-verify with
  fast post-boot retries, 15-min weather refresh, and wall-clock-aligned minute redraw.
- `src/net_time.h/.cpp` — WiFi connection, NTP sync (with periodic `configTime()` resend and
  RSSI logging for resilience), staleness tracking, timezone config validation.
- `src/weather.h/.cpp` — background (FreeRTOS task) weather fetch over HTTP, mutex-guarded
  `WeatherSnapshot` shared with the render loop.
- `src/display.h/.cpp` — rendering: icons, `renderBuffer()`, `pushFull()`/`pushPartial()`,
  runtime-configurable rotation via `displaySetRotation()`.
- `src/buttons.h/.cpp` - debounced dial up/down/press polling.
- `src/boot_config.h/.cpp` — reads/persists display rotation in NVS (ESP32 `Preferences`),
  detects the Menu-button-held-at-boot toggle gesture.
- `include/config.h` — user-editable settings.
- `lib/EPD/src` — vendored low-level e-paper driver (do not modify).

## Hardware notes

- Board: ESP32-S3-WROOM-1-N8R8 (8MB flash / 8MB PSRAM), driving a 792x272 e-paper panel
  (two cascaded SSD1683 controllers).
- GPIO7 must be held HIGH to power the panel.
- The e-paper driver in `lib/EPD` is vendored verbatim from Elecrow's official example code
  and communicates over bit-banged (software) SPI — not hardware SPI, not GxEPD2.
- Front-panel buttons (active-low): Menu=GPIO2, Back/Exit=GPIO1, Dial Up=GPIO6, Dial Down=GPIO4,
  Dial In/select=GPIO5. Menu is used for the boot-time rotation toggle; the dial drives the timezone info panel.

## Toolchain notes

- Build system: PlatformIO (`platform = espressif32`, `board = esp32-s3-devkitc-1`, Arduino framework),
  environment `crowpanel-esp32s3`. Only library dependency: ArduinoJson ^7.2.0; the e-paper driver is
  vendored in `lib/EPD`.
- On the dev Mac, PlatformIO is a user pip install under Python 3.8 with no `pio` on PATH. Run it as
  `/usr/bin/python3 -m platformio run` (build) and
  `/usr/bin/python3 -m platformio run -t upload --upload-port /dev/cu.usbserial-10` (flash).
  Toolchains/frameworks live in `~/.platformio/{packages,platforms}`.
- Anaconda's `curl`/`python3` on PATH are broken (wrong CPU arch); use `/usr/bin/curl` and `/usr/bin/python3`.
- The panel enumerates as `/dev/cu.usbserial-10`. Upload may need bootloader mode (hold BOOT, tap RESET).
- A clean build takes ~1 minute; firmware uses ~28% flash / ~22% RAM.
