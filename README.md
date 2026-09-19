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

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Edit [include/config.h](include/config.h):
   - `WIFI_SSID` / `WIFI_PASSWORD`
   - `NTP_SERVER_LOCAL` — your local network's NTP source (router, Pi-hole, etc.)
   - `TIMEZONES[4]` — the four configurable timezones, as POSIX TZ strings
   - `LOCAL_TZ_POSIX` — the timezone used for "local" time
   - `OWM_API_KEY` / `OWM_CITY_QUERY` — your [OpenWeatherMap](https://openweathermap.org/api) key and location
3. Build and upload:
   ```
   pio run -t upload
   ```
4. Monitor serial output:
   ```
   pio device monitor
   ```

POSIX TZ strings (with DST rules) can be looked up at:
https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv

## Hardware notes

- Board: ESP32-S3-WROOM-1-N8R8 (8MB flash / 8MB PSRAM), driving a 792x272 e-paper panel
  (two cascaded SSD1683 controllers).
- GPIO7 must be held HIGH to power the panel.
- The e-paper driver in `lib/EPD` is vendored verbatim from Elecrow's official example code
  and communicates over bit-banged (software) SPI — not hardware SPI, not GxEPD2.

## Project layout

- `src/main.cpp` — application logic (WiFi, NTP, weather, rendering, update scheduling)
- `include/config.h` — user-editable settings
- `lib/EPD/src` — vendored low-level e-paper driver (do not modify)
