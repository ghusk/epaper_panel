// CrowPanel ESP32-S3 5.79" e-paper clock / weather panel
//
// - Shows local time plus 4 configurable timezones (24h clock) and current weather.
// - Gathers NTP time + weather on boot.
// - Redraws the display once a minute from the internally kept clock.
// - Re-verifies the clock against a local-network NTP source once an hour.
// - Refreshes weather every 15 minutes.
// - Shows an icon indicating whether the time signal is still "fresh" (recently
//   NTP-verified) or "stale".

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
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
static unsigned long bootMillis = 0;
static unsigned long lastSuccessfulSyncMillis = 0;
static unsigned long lastNtpAttemptMillis = 0;
static unsigned long lastWeatherFetchMillis = 0;
static unsigned long lastDisplayUpdateMillis = 0;
static unsigned long updateCounter = 0;

static bool weatherValid = false;
static String weatherCity;
static String weatherDesc;
static float weatherTemp = 0.0f;
static int weatherHumidity = 0;
static float weatherWind = 0.0f;
static time_t weatherLastFetchEpoch = 0;
static int weatherConditionId = 0;   // OpenWeatherMap condition code, e.g. 800 = clear
static bool weatherIsDay = true;     // from the icon code's 'd'/'n' suffix

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
            int conditionId = doc["weather"][0]["id"].as<int>();
            String iconCode = doc["weather"][0]["icon"].as<String>();
            float temp = doc["main"]["temp"].as<float>();
            int humidity = doc["main"]["humidity"].as<int>();
            float wind = doc["wind"]["speed"].as<float>();

            xSemaphoreTake(weatherMutex, portMAX_DELAY);
            weatherCity = city;
            weatherDesc = desc;
            weatherConditionId = conditionId;
            weatherIsDay = !iconCode.endsWith("n");
            weatherTemp = temp;
            weatherHumidity = humidity;
            weatherWind = wind;
            weatherLastFetchEpoch = time(nullptr);
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

// ---------------------------------------------------------------------------
// Weather condition icons (simple vector glyphs - no bitmap assets needed).
// ---------------------------------------------------------------------------
static void drawSunIcon(int cx, int cy, int r) {
    int coreR = r * 5 / 10;
    EPD_DrawCircle(cx, cy, coreR, BLACK, 1);
    for (int i = 0; i < 8; i++) {
        float ang = i * (float)M_PI / 4.0f;
        int x1 = cx + (int)(cosf(ang) * (coreR + 3));
        int y1 = cy + (int)(sinf(ang) * (coreR + 3));
        int x2 = cx + (int)(cosf(ang) * r);
        int y2 = cy + (int)(sinf(ang) * r);
        EPD_DrawLine(x1, y1, x2, y2, BLACK);
    }
}

static void drawMoonIcon(int cx, int cy, int r) {
    EPD_DrawCircle(cx, cy, r, BLACK, 1);
    EPD_DrawCircle(cx + r * 4 / 10, cy - r * 3 / 10, (int)(r * 0.85f), WHITE, 1);
}

// Cloud silhouette, drawn as three overlapping hollow lobes sitting on a flat
// base line at y = baseY.
static void drawCloudAt(int cx, int baseY, int r) {
    int rTop = r * 6 / 10;
    int rSide = r * 4 / 10;
    EPD_DrawCircle(cx - r * 6 / 10, baseY - rSide, rSide, BLACK, 0);
    EPD_DrawCircle(cx, baseY - rTop, rTop, BLACK, 0);
    EPD_DrawCircle(cx + r * 6 / 10, baseY - rSide, rSide, BLACK, 0);
    EPD_DrawLine(cx - r * 6 / 10 - rSide, baseY, cx + r * 6 / 10 + rSide, baseY, BLACK);
}

static void drawRainIcon(int cx, int cy, int r) {
    int baseY = cy - r / 6;
    drawCloudAt(cx, baseY, r * 9 / 10);
    for (int i = -1; i <= 1; i++) {
        int x = cx + i * (r * 4 / 10);
        EPD_DrawLine(x, baseY + r / 6, x - 2, baseY + r / 6 + r / 3, BLACK);
    }
}

static void drawSnowIcon(int cx, int cy, int r) {
    int baseY = cy - r / 6;
    drawCloudAt(cx, baseY, r * 9 / 10);
    int ys = baseY + r / 4;
    int xs[3] = { cx - r * 4 / 10, cx, cx + r * 4 / 10 };
    for (int i = 0; i < 3; i++) {
        int x = xs[i];
        int y = ys + (i % 2) * 4;
        int s = r / 6;
        if (s < 2) s = 2;
        EPD_DrawLine(x - s, y, x + s, y, BLACK);
        EPD_DrawLine(x, y - s, x, y + s, BLACK);
        EPD_DrawLine(x - s * 7 / 10, y - s * 7 / 10, x + s * 7 / 10, y + s * 7 / 10, BLACK);
        EPD_DrawLine(x - s * 7 / 10, y + s * 7 / 10, x + s * 7 / 10, y - s * 7 / 10, BLACK);
    }
}

static void drawThunderIcon(int cx, int cy, int r) {
    int baseY = cy - r / 6;
    drawCloudAt(cx, baseY, r * 9 / 10);
    int x0 = cx + r / 8, y0 = baseY + r / 8;
    EPD_DrawLine(x0, y0, x0 - r / 4, y0 + r / 3, BLACK);
    EPD_DrawLine(x0 - r / 4, y0 + r / 3, x0 + r / 8, y0 + r / 3, BLACK);
    EPD_DrawLine(x0 + r / 8, y0 + r / 3, x0 - r / 6, y0 + r * 2 / 3, BLACK);
}

static void drawFogIcon(int cx, int cy, int r) {
    for (int i = -1; i <= 2; i++) {
        int y = cy + i * (r / 3);
        int halfw = (i % 2 == 0) ? r * 7 / 10 : r * 5 / 10;
        EPD_DrawLine(cx - halfw, y, cx + halfw, y, BLACK);
    }
}

static void drawPartlyCloudyIcon(int cx, int cy, int r, bool isDay) {
    if (isDay) {
        drawSunIcon(cx - r / 3, cy - r / 3, r * 6 / 10);
    } else {
        drawMoonIcon(cx - r / 3, cy - r / 3, r * 4 / 10);
    }
    drawCloudAt(cx + r / 6, cy + r / 2, r * 8 / 10);
}

// Picks a glyph from an OpenWeatherMap condition id (+ day/night) - see
// https://openweathermap.org/weather-conditions for the id group meanings.
static void drawWeatherIcon(int cx, int cy, int r, int conditionId, bool isDay) {
    if (conditionId >= 200 && conditionId < 300) {
        drawThunderIcon(cx, cy, r);
    } else if (conditionId >= 300 && conditionId < 600) {
        drawRainIcon(cx, cy, r);
    } else if (conditionId >= 600 && conditionId < 700) {
        drawSnowIcon(cx, cy, r);
    } else if (conditionId >= 700 && conditionId < 800) {
        drawFogIcon(cx, cy, r);
    } else if (conditionId == 800) {
        if (isDay) drawSunIcon(cx, cy, r);
        else drawMoonIcon(cx, cy, r);
    } else if (conditionId == 801) {
        drawPartlyCloudyIcon(cx, cy, r, isDay);
    } else {
        drawCloudAt(cx, cy + r / 2, r);
    }
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

    // --- Big local clock (left) + weather (right), side by side ---
    const int clockColWidth = 300;
    EPD_ShowString(8, 34, LOCAL_TZ_LABEL, 24, BLACK);
    if (ntpEverSynced) {
        strftime(buf, sizeof(buf), "%H:%M", &localTm);
    } else {
        strncpy(buf, "--:--", sizeof(buf));
    }
    EPD_ShowString(8, 62, buf, 48, BLACK);

    EPD_DrawLine(clockColWidth, 32, clockColWidth, 114, BLACK);

    xSemaphoreTake(weatherMutex, portMAX_DELAY);
    bool wValid = weatherValid;
    String wCity = weatherCity;
    String wDesc = weatherDesc;
    int wConditionId = weatherConditionId;
    bool wIsDay = weatherIsDay;
    float wTemp = weatherTemp;
    int wHumidity = weatherHumidity;
    float wWind = weatherWind;
    time_t wFetchEpoch = weatherLastFetchEpoch;
    xSemaphoreGive(weatherMutex);

    int wx = clockColWidth + 16;
    if (wValid) {
        int iconCx = wx + 20;
        int iconCy = 58;
        drawWeatherIcon(iconCx, iconCy, 18, wConditionId, wIsDay);
        wx += 48;

        snprintf(buf, sizeof(buf), "%s: %s", wCity.c_str(), wDesc.c_str());
        EPD_ShowString(wx, 36, buf, 16, BLACK);

        const char *unit = (strcmp(OWM_UNITS, "metric") == 0) ? "C" : "F";
        snprintf(buf, sizeof(buf), "Temp: %.1f%s   Humidity: %d%%", wTemp, unit, wHumidity);
        EPD_ShowString(wx, 56, buf, 16, BLACK);

        if (wFetchEpoch > 0) {
            struct tm fetchTm;
            localtime_r(&wFetchEpoch, &fetchTm);
            char stamp[8];
            strftime(stamp, sizeof(stamp), "%H:%M", &fetchTm);
            snprintf(buf, sizeof(buf), "Wind: %.1f m/s   as of %s", wWind, stamp);
        } else {
            snprintf(buf, sizeof(buf), "Wind: %.1f m/s", wWind);
        }
        EPD_ShowString(wx, 76, buf, 16, BLACK);
    } else {
        EPD_ShowString(wx, 36, "Weather unavailable", 16, BLACK);
    }

    EPD_DrawLine(0, 114, EPD_VISIBLE_W, 114, BLACK);

    // --- 4 configurable timezones, given the full remaining height so the
    // label and clock fonts have more room to breathe. ---
    const int colWidth = EPD_VISIBLE_W / 4;
    const int tzTop = 114;
    const int tzBottom = 246;
    for (int i = 0; i < 4; i++) {
        setenv("TZ", TIMEZONES[i].posixTz, 1);
        tzset();
        struct tm tzTm;
        localtime_r(&now, &tzTm);

        int x0 = i * colWidth + 6;
        EPD_ShowString(x0, tzTop + 16, TIMEZONES[i].label, 24, BLACK);
        if (ntpEverSynced) {
            strftime(buf, sizeof(buf), "%H:%M", &tzTm);
        } else {
            strncpy(buf, "--:--", sizeof(buf));
        }
        EPD_ShowString(x0, tzTop + 56, buf, 48, BLACK);

        if (i > 0) {
            EPD_DrawLine(i * colWidth, tzTop, i * colWidth, tzBottom, BLACK);
        }
    }
    // Restore local TZ for anything drawn afterwards.
    setenv("TZ", LOCAL_TZ_POSIX, 1);
    tzset();

    EPD_DrawLine(0, tzBottom, EPD_VISIBLE_W, tzBottom, BLACK);

    EPD_ShowString(8, 252, "Updates every 1 min - weather every 15 min - NTP hourly", 12, BLACK);
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

    bootMillis = millis();

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
