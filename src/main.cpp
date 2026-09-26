// CrowPanel ESP32-S3 5.79" e-paper clock / weather panel
//
// - Shows local time plus 4 configurable timezones (24h clock) and current weather.
// - Gathers NTP time + weather on boot.
// - Redraws the display once a minute, aligned to the wall-clock minute
//   boundary (not a fixed offset from boot) so drift never accumulates.
// - Re-verifies the clock against a local-network NTP source once an hour.
// - Refreshes weather every 15 minutes.
// - Shows an icon indicating whether the time signal is still "fresh" (recently
//   NTP-verified) or "stale".

#include <Arduino.h>
#include <time.h>

#include "config.h"
#include "EPD.h"
#include "net_time.h"
#include "weather.h"
#include "display.h"

static unsigned long bootMillis = 0;
static unsigned long lastNtpAttemptMillis = 0;
static unsigned long lastWeatherFetchMillis = 0;
static unsigned long lastDisplayUpdateMillis = 0;
static unsigned long updateCounter = 0;

// Epoch-minute (time_t / 60) of the last render, used to detect a wall-clock
// minute rollover. -1 means "none yet". Only meaningful once ntpEverSynced,
// since it relies on the system clock being trustworthy.
static long lastRenderedMinute = -1;

void setup() {
    Serial.begin(115200);
    delay(200);

    bootMillis = millis();

    // Panel power switch.
    pinMode(7, OUTPUT);
    digitalWrite(7, HIGH);

    EPD_GPIOInit();

    weatherInit();

    connectWiFi();

    if (syncNtp()) {
        ntpEverSynced = true;
        lastSuccessfulSyncMillis = millis();
    }

    validateTimezoneConfig();

    fetchWeatherAsync(12000);

    renderBuffer(isTimeFresh());
    pushFull();

    // Anchor the minute-tick to the wall clock: the next redraw only happens
    // once time(nullptr) actually rolls over to a new minute, e.g. an NTP
    // time of 10:35:35 means the next redraw is ~25s away, not a full
    // minute - and every following redraw lands right on the minute boundary
    // since it's driven by absolute time rather than an accumulating offset.
    lastRenderedMinute = ntpEverSynced ? (long)(time(nullptr) / 60) : -1;

    lastDisplayUpdateMillis = millis();
    lastNtpAttemptMillis = millis();
    lastWeatherFetchMillis = millis();
    updateCounter = 0;
}

void loop() {
    unsigned long nowMs = millis();

    // Hourly: re-verify against NTP. Until the first successful sync,
    // retry sooner (see NTP_RETRY_INTERVAL_MS) but give up after
    // NTP_RETRY_WINDOW_MS and fall back to the normal cadence, so a
    // persistent outage doesn't keep retrying WiFi/NTP indefinitely.
    unsigned long ntpInterval = NTP_RESYNC_INTERVAL_MS;
    if (!ntpEverSynced && (nowMs - bootMillis < NTP_RETRY_WINDOW_MS)) {
        ntpInterval = NTP_RETRY_INTERVAL_MS;
    }
    if (nowMs - lastNtpAttemptMillis >= ntpInterval) {
        lastNtpAttemptMillis = nowMs;
        connectWiFi();
        if (syncNtp()) {
            ntpEverSynced = true;
            lastSuccessfulSyncMillis = millis();
        }
    }

    // Every 15 minutes: refresh weather.
    if (nowMs - lastWeatherFetchMillis >= WEATHER_REFRESH_INTERVAL_MS) {
        lastWeatherFetchMillis = nowMs;
        connectWiFi();
        fetchWeatherAsync(12000);
    }

    // Redraw once the wall-clock minute actually changes (bounds any local
    // millis()/RTC drift to well under 1s, since this is checked every loop
    // iteration against absolute time rather than counted up from the last
    // redraw). Before the first NTP sync there's no trustworthy clock yet,
    // so fall back to a plain millis() cadence.
    bool shouldUpdateDisplay = false;
    if (ntpEverSynced) {
        long nowMinute = (long)(time(nullptr) / 60);
        if (nowMinute != lastRenderedMinute) {
            lastRenderedMinute = nowMinute;
            shouldUpdateDisplay = true;
        }
    } else if (nowMs - lastDisplayUpdateMillis >= DISPLAY_UPDATE_INTERVAL_MS) {
        shouldUpdateDisplay = true;
    }

    if (shouldUpdateDisplay) {
        lastDisplayUpdateMillis = nowMs;
        renderBuffer(isTimeFresh());
        updateCounter++;

        if (updateCounter % FULL_REFRESH_EVERY_N_UPDATES == 0) {
            pushFull();
        } else {
            pushPartial();
        }
    }

    delay(200);
}
