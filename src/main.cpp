// CrowPanel ESP32-S3 5.79" e-paper clock / weather panel
//
// - Shows local time plus 4 configurable timezones (24h clock) and current weather.
// - Gathers NTP time + weather on boot.
// - Redraws the display once a minute from the internally kept clock.
// - Re-verifies the clock against a local-network NTP source once an hour.
// - Shows an icon indicating whether the time signal is still "fresh" (recently
//   NTP-verified) or "stale".

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "EPD.h"
#include "config.h"

// Visible drawable area of the panel (the driver's internal buffer is 800 wide
// to account for the 8-pixel seam between the two cascaded controller ICs).
static const uint16_t EPD_VISIBLE_W = 792;
static const uint16_t EPD_VISIBLE_H = 272;

// Screen buffer, sized to match the vendor driver's expected layout (two
// 400x272 controller RAMs back to back).
static uint8_t ImageBW[27200];

// A time_t before this is considered "not yet synced" (roughly Nov 2023).
static const time_t MIN_VALID_EPOCH = 1700000000;

static bool ntpEverSynced = false;
static unsigned long lastSuccessfulSyncMillis = 0;
static unsigned long lastNtpAttemptMillis = 0;
static unsigned long lastDisplayUpdateMillis = 0;
static unsigned long updateCounter = 0;

static bool weatherValid = false;
static String weatherCity;
static String weatherDesc;
static float weatherTemp = 0.0f;
static int weatherHumidity = 0;
static float weatherWind = 0.0f;

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(400);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected, IP: ");
        Serial.println(WiFi.localIP());
        return true;
    }
    Serial.println("WiFi connection failed");
    return false;
}

// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------
static bool syncNtp() {
    if (WiFi.status() != WL_CONNECTED) return false;

    Serial.printf("Syncing time via NTP (gateway=%s, dns=%s)...\n",
                  WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
    configTime(0, 0, NTP_SERVER_LOCAL, NTP_SERVER_FALLBACK_1, NTP_SERVER_FALLBACK_2);

    time_t now = time(nullptr);
    int attempts = 0;
    while (now < MIN_VALID_EPOCH && attempts < 60) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        attempts++;
    }
    Serial.println();

    bool ok = now >= MIN_VALID_EPOCH;
    Serial.println(ok ? "NTP sync OK" : "NTP sync FAILED");
    return ok;
}

static bool isTimeFresh() {
    if (!ntpEverSynced) return false;
    return (millis() - lastSuccessfulSyncMillis) < NTP_STALE_THRESHOLD_MS;
}

// ---------------------------------------------------------------------------
// Timezone config validation
// ---------------------------------------------------------------------------
// Catches malformed/mislabeled POSIX TZ strings (e.g. an IANA name where a
// POSIX string is required, or a plausible-looking string that resolves to an
// unexpected UTC offset) before they silently show the wrong time.
static bool tzConfigWarning = false;

// Days since the Unix epoch for a UTC-agnostic (year, month, day), used below
// to derive a TZ offset without relying on the non-portable tm_gmtoff field.
static long daysFromCivil(long y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Reinterprets a struct tm's fields as if they were UTC, ignoring tm_isdst.
static time_t tmFieldsAsEpoch(const struct tm &t) {
    long days = daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return days * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec;
}

static bool checkTzOffset(const char *tz, long *outOffsetSec) {
    setenv("TZ", tz, 1);
    tzset();
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    *outOffsetSec = (long)(tmFieldsAsEpoch(tmNow) - now);
    // Real-world UTC offsets range from -12:00 to +14:00 and fall on 15-minute
    // boundaries; anything outside that points at a config typo.
    return (*outOffsetSec >= -12 * 3600L) && (*outOffsetSec <= 14 * 3600L) &&
           (*outOffsetSec % 900 == 0);
}

static void validateTimezoneConfig() {
    tzConfigWarning = false;
    long offsetSec;

    if (!checkTzOffset(LOCAL_TZ_POSIX, &offsetSec)) {
        Serial.printf("TZ CONFIG WARNING: %s zone '%s' -> suspicious offset %lds\n",
                      LOCAL_TZ_LABEL, LOCAL_TZ_POSIX, offsetSec);
        tzConfigWarning = true;
    }

    for (int i = 0; i < 4; i++) {
        if (!checkTzOffset(TIMEZONES[i].posixTz, &offsetSec)) {
            Serial.printf("TZ CONFIG WARNING: zone '%s' (%s) -> suspicious offset %lds\n",
                          TIMEZONES[i].label, TIMEZONES[i].posixTz, offsetSec);
            tzConfigWarning = true;
        }
        // A zone explicitly labeled UTC should genuinely have a zero offset.
        if (strcmp(TIMEZONES[i].label, "UTC") == 0 && offsetSec != 0) {
            Serial.printf("TZ CONFIG WARNING: zone labeled 'UTC' has non-zero offset %lds\n", offsetSec);
            tzConfigWarning = true;
        }
    }

    setenv("TZ", LOCAL_TZ_POSIX, 1);
    tzset();
}

// ---------------------------------------------------------------------------
// Weather
// ---------------------------------------------------------------------------
// Guards the shared weather* fields below, since they are written from the
// background fetch task (see fetchWeatherAsync) while renderBuffer() may read
// them concurrently on the main loop.
static SemaphoreHandle_t weatherMutex = nullptr;
static volatile bool weatherTaskRunning = false;
static volatile bool weatherTaskDone = false;

// Does the actual (blocking, potentially very slow) HTTP fetch + parse. Only
// ever called from weatherTaskFn(), never directly from setup()/loop().
static bool fetchWeatherBlocking() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    String url = String("http://api.openweathermap.org/data/2.5/weather?q=") +
                 OWM_CITY_QUERY + "&appid=" + OWM_API_KEY + "&units=" + OWM_UNITS;

    Serial.println("Fetching weather...");
    http.begin(url);
    int code = http.GET();

    bool ok = false;
    if (code == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
            String city = doc["name"].as<String>();
            String desc = doc["weather"][0]["description"].as<String>();
            float temp = doc["main"]["temp"].as<float>();
            int humidity = doc["main"]["humidity"].as<int>();
            float wind = doc["wind"]["speed"].as<float>();

            xSemaphoreTake(weatherMutex, portMAX_DELAY);
            weatherCity = city;
            weatherDesc = desc;
            weatherTemp = temp;
            weatherHumidity = humidity;
            weatherWind = wind;
            xSemaphoreGive(weatherMutex);
            ok = true;
        } else {
            Serial.print("Weather JSON parse error: ");
            Serial.println(err.c_str());
        }
    } else {
        Serial.printf("Weather HTTP error: %d\n", code);
    }
    http.end();
    return ok;
}

static void weatherTaskFn(void *param) {
    bool ok = fetchWeatherBlocking();

    xSemaphoreTake(weatherMutex, portMAX_DELAY);
    weatherValid = ok;
    xSemaphoreGive(weatherMutex);

    weatherTaskDone = true;
    weatherTaskRunning = false;
    vTaskDelete(nullptr);
}

// Kicks off a weather fetch on a background task and waits up to timeoutMs
// for it to finish, but never longer than that - if the underlying HTTP
// call is stuck (e.g. DNS silently dropped by a restrictive/guest network,
// which is not bounded by HTTPClient's connect/read timeouts), this returns
// anyway and the task is simply left running in the background. It will
// still update weatherValid/weatherCity/etc. under the mutex whenever (if
// ever) it completes, picked up on a later render. This guarantees setup()
// and loop() can never hang indefinitely because of the network.
static void fetchWeatherAsync(uint32_t timeoutMs) {
    if (weatherTaskRunning) {
        Serial.println("Weather fetch still in progress from a previous attempt; skipping.");
        return;
    }
    weatherTaskRunning = true;
    weatherTaskDone = false;
    xTaskCreatePinnedToCore(weatherTaskFn, "weatherFetch", 8192, nullptr, 1, nullptr, 0);

    uint32_t start = millis();
    while (!weatherTaskDone && (millis() - start) < timeoutMs) {
        delay(50);
    }
    if (!weatherTaskDone) {
        Serial.println("Weather fetch taking too long; continuing without blocking further.");
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void drawFreshnessIcon(int cx, int cy, int r, bool fresh) {
    EPD_DrawCircle(cx, cy, r, BLACK, 0);
    if (fresh) {
        // Checkmark.
        EPD_DrawLine(cx - r / 2, cy, cx - r / 6, cy + r / 2, BLACK);
        EPD_DrawLine(cx - r / 6, cy + r / 2, cx + r / 2, cy - r / 2, BLACK);
    } else {
        // Cross.
        EPD_DrawLine(cx - r / 2, cy - r / 2, cx + r / 2, cy + r / 2, BLACK);
        EPD_DrawLine(cx - r / 2, cy + r / 2, cx + r / 2, cy - r / 2, BLACK);
    }
}

// Triangle-with-exclamation mark, used to flag a suspicious TZ config separately
// from the NTP freshness icon.
static void drawWarningTriangle(int cx, int cy, int r) {
    EPD_DrawLine(cx, cy - r, cx - r, cy + r, BLACK);
    EPD_DrawLine(cx, cy - r, cx + r, cy + r, BLACK);
    EPD_DrawLine(cx - r, cy + r, cx + r, cy + r, BLACK);
    EPD_DrawLine(cx, cy - r / 3, cx, cy + r / 4, BLACK);
    EPD_DrawCircle(cx, cy + r / 2, 1, BLACK, 1);
}

static void renderBuffer(bool timeFresh) {
    Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
    Paint_Clear(WHITE);

    char buf[64];
    time_t now = time(nullptr);

    // --- Header: date + freshness icon ---
    setenv("TZ", LOCAL_TZ_POSIX, 1);
    tzset();
    struct tm localTm;
    localtime_r(&now, &localTm);
    if (ntpEverSynced) {
        strftime(buf, sizeof(buf), "%a %d %b %Y", &localTm);
    } else {
        strncpy(buf, "Waiting for NTP sync...", sizeof(buf));
    }
    EPD_ShowString(8, 6, buf, 16, BLACK);

    drawFreshnessIcon(EPD_VISIBLE_W - 70, 18, 14, timeFresh);
    EPD_ShowString(EPD_VISIBLE_W - 150, 10, timeFresh ? "SYNCED" : "STALE", 16, BLACK);

    if (tzConfigWarning) {
        EPD_ShowString(180, 10, "TZ CFG", 16, BLACK);
        drawWarningTriangle(290, 18, 12);
    }

    EPD_DrawLine(0, 32, EPD_VISIBLE_W, 32, BLACK);

    // --- Big local clock ---
    EPD_ShowString(8, 38, LOCAL_TZ_LABEL, 16, BLACK);
    if (ntpEverSynced) {
        strftime(buf, sizeof(buf), "%H:%M", &localTm);
    } else {
        strncpy(buf, "--:--", sizeof(buf));
    }
    EPD_ShowString(8, 58, buf, 48, BLACK);

    EPD_DrawLine(0, 116, EPD_VISIBLE_W, 116, BLACK);

    // --- 4 configurable timezones ---
    const int colWidth = EPD_VISIBLE_W / 4;
    for (int i = 0; i < 4; i++) {
        setenv("TZ", TIMEZONES[i].posixTz, 1);
        tzset();
        struct tm tzTm;
        localtime_r(&now, &tzTm);

        int x0 = i * colWidth + 6;
        EPD_ShowString(x0, 118, TIMEZONES[i].label, 16, BLACK);
        if (ntpEverSynced) {
            strftime(buf, sizeof(buf), "%H:%M", &tzTm);
        } else {
            strncpy(buf, "--:--", sizeof(buf));
        }
        EPD_ShowString(x0, 136, buf, 48, BLACK);

        if (i > 0) {
            EPD_DrawLine(i * colWidth, 116, i * colWidth, 192, BLACK);
        }
    }
    // Restore local TZ for anything drawn afterwards.
    setenv("TZ", LOCAL_TZ_POSIX, 1);
    tzset();

    EPD_DrawLine(0, 192, EPD_VISIBLE_W, 192, BLACK);

    // --- Weather ---
    xSemaphoreTake(weatherMutex, portMAX_DELAY);
    bool wValid = weatherValid;
    String wCity = weatherCity;
    String wDesc = weatherDesc;
    float wTemp = weatherTemp;
    int wHumidity = weatherHumidity;
    float wWind = weatherWind;
    xSemaphoreGive(weatherMutex);

    if (wValid) {
        snprintf(buf, sizeof(buf), "%s: %s", wCity.c_str(), wDesc.c_str());
        EPD_ShowString(8, 200, buf, 16, BLACK);

        const char *unit = (strcmp(OWM_UNITS, "metric") == 0) ? "C" : "F";
        snprintf(buf, sizeof(buf), "Temp: %.1f%s   Humidity: %d%%   Wind: %.1f m/s",
                  wTemp, unit, wHumidity, wWind);
        EPD_ShowString(8, 224, buf, 16, BLACK);
    } else {
        EPD_ShowString(8, 200, "Weather unavailable", 16, BLACK);
    }

    EPD_ShowString(8, 250, "Updates every 1 min - NTP re-sync hourly", 12, BLACK);
}

// True once the panel has been primed (fast-mode init + baseline clear) so
// EPD_PartUpdate() has a valid "old" frame to diff against.
static bool epdPrimed = false;

// Full re-init + a full-waveform clear/redraw. Visibly flashes the whole
// panel, so this is only used for the initial draw and the periodic
// ghost-clearing refresh - not every minute.
static void pushFull() {
    EPD_FastMode1Init();
    EPD_Display_Clear();
    EPD_Update();
    EPD_Clear_R26A6H();
    EPD_Display(ImageBW);
    EPD_PartUpdate();
    epdPrimed = true;
}

// Loads the new frame and triggers a partial refresh only - the panel is left
// initialized (no re-init, no deep sleep) so only the changed pixels redraw
// instead of flashing the whole screen every minute.
static void pushPartial() {
    if (!epdPrimed) {
        pushFull();
        return;
    }
    EPD_Display(ImageBW);
    EPD_PartUpdate();
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);

    // Panel power switch.
    pinMode(7, OUTPUT);
    digitalWrite(7, HIGH);

    EPD_GPIOInit();

    weatherMutex = xSemaphoreCreateMutex();

    connectWiFi();

    if (syncNtp()) {
        ntpEverSynced = true;
        lastSuccessfulSyncMillis = millis();
    }

    fetchWeatherAsync(12000);

    renderBuffer(isTimeFresh());
    pushFull();

    lastDisplayUpdateMillis = millis();
    lastNtpAttemptMillis = millis();
    updateCounter = 0;
}

void loop() {
    unsigned long nowMs = millis();

    // Hourly: re-verify against NTP and refresh weather.
    if (nowMs - lastNtpAttemptMillis >= NTP_RESYNC_INTERVAL_MS) {
        lastNtpAttemptMillis = nowMs;
        connectWiFi();
        if (syncNtp()) {
            ntpEverSynced = true;
            lastSuccessfulSyncMillis = millis();
        }
        fetchWeatherAsync(12000);
    }

    // Every minute: redraw from the internally kept clock.
    if (nowMs - lastDisplayUpdateMillis >= DISPLAY_UPDATE_INTERVAL_MS) {
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
