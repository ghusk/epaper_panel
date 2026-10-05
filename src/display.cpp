#include "display.h"

#include <math.h>
#include <time.h>

#include "EPD.h"
#include "config.h"
#include "net_time.h"
#include "weather.h"

// Visible drawable area of the panel (the driver's internal buffer is 800 wide
// to account for the 8-pixel seam between the two cascaded controller ICs).
static const uint16_t EPD_VISIBLE_W = 792;
static const uint16_t EPD_VISIBLE_H = 272;

// Screen buffer, sized to match the vendor driver's expected layout (two
// 400x272 controller RAMs back to back).
static uint8_t ImageBW[27200];

static uint16_t currentRotation = DISPLAY_ROTATION;

void displaySetRotation(uint16_t rotation) {
    currentRotation = rotation;
}

// ---------------------------------------------------------------------------
// Status icons
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

void renderBuffer(bool timeFresh) {
    Paint_NewImage(ImageBW, EPD_W, EPD_H, currentRotation, WHITE);
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

    WeatherSnapshot w = getWeatherSnapshot();

    int wx = clockColWidth + 16;
    if (w.valid) {
        int iconCx = wx + 20;
        int iconCy = 58;
        drawWeatherIcon(iconCx, iconCy, 18, w.conditionId, w.isDay);
        wx += 48;

        snprintf(buf, sizeof(buf), "%s: %s", w.city.c_str(), w.desc.c_str());
        EPD_ShowString(wx, 36, buf, 16, BLACK);

        const char *unit = (strcmp(OWM_UNITS, "metric") == 0) ? "C" : "F";
        snprintf(buf, sizeof(buf), "Temp: %.1f%s   Humidity: %d%%", w.temp, unit, w.humidity);
        EPD_ShowString(wx, 56, buf, 16, BLACK);

        if (w.fetchEpoch > 0) {
            struct tm fetchTm;
            localtime_r(&w.fetchEpoch, &fetchTm);
            char stamp[8];
            strftime(stamp, sizeof(stamp), "%H:%M", &fetchTm);
            snprintf(buf, sizeof(buf), "Wind: %.1f m/s   as of %s", w.wind, stamp);
        } else {
            snprintf(buf, sizeof(buf), "Wind: %.1f m/s", w.wind);
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

// ---------------------------------------------------------------------------
// Timezone info panel
// ---------------------------------------------------------------------------
int tzInfoCount() {
    return 4;
}

// Formats a seconds offset as e.g. "+5:30" / "-7:00".
static void formatOffset(char *out, size_t n, long sec) {
    char sign = sec < 0 ? '-' : '+';
    long a = sec < 0 ? -sec : sec;
    snprintf(out, n, "%c%ld:%02ld", sign, a / 3600, (a % 3600) / 60);
}

void renderTzInfoBuffer(int index) {
    Paint_NewImage(ImageBW, EPD_W, EPD_H, currentRotation, WHITE);
    Paint_Clear(WHITE);

    const TimezoneConfig &zone = TIMEZONES[index];
    char buf[96], off[16], off2[16];
    time_t now = time(nullptr);

    snprintf(buf, sizeof(buf), "Timezone info %d/%d", index + 1, tzInfoCount());
    EPD_ShowString(8, 6, buf, 16, BLACK);
    EPD_DrawLine(0, 32, EPD_VISIBLE_W, 32, BLACK);
    EPD_DrawLine(0, 246, EPD_VISIBLE_W, 246, BLACK);
    EPD_ShowString(8, 252, "Up: main display   Press: next timezone", 12, BLACK);

    if (!ntpEverSynced) {
        EPD_ShowString(8, 50, "Waiting for NTP sync...", 24, BLACK);
        return;
    }

    long curOffset = tzOffsetAt(zone.posixTz, now);
    char abbr[12];
    struct tm curTm;
    localtime_r(&now, &curTm);
    strftime(abbr, sizeof(abbr), "%Z", &curTm);

    time_t change = 0;
    bool hasChange = findNextOffsetChange(zone.posixTz, now, &change);
    long newOffset = curOffset;
    long newLocalOffset = 0;
    char changeStr[24] = "";
    if (hasChange) {
        newOffset = tzOffsetAt(zone.posixTz, change);
        newLocalOffset = tzOffsetAt(LOCAL_TZ_POSIX, change);
        // Wall-clock reading in the zone itself just as the change happens.
        time_t wall = change + curOffset;
        struct tm wallTm;
        gmtime_r(&wall, &wallTm);
        strftime(changeStr, sizeof(changeStr), "%Y-%m-%d %H:%M", &wallTm);
    }
    long localNow = tzOffsetAt(LOCAL_TZ_POSIX, now);

    // Leave TZ on the local zone for anything drawn afterwards.
    setenv("TZ", LOCAL_TZ_POSIX, 1);
    tzset();

    EPD_ShowString(8, 40, zone.label, 48, BLACK);

    formatOffset(off, sizeof(off), curOffset);
    snprintf(buf, sizeof(buf), "Now: %s  UTC%s", abbr, off);
    EPD_ShowString(8, 100, buf, 24, BLACK);

    if (hasChange) {
        snprintf(buf, sizeof(buf), "Next change: %s", changeStr);
        EPD_ShowString(8, 134, buf, 24, BLACK);

        formatOffset(off, sizeof(off), newOffset);
        formatOffset(off2, sizeof(off2), newOffset - newLocalOffset);
        snprintf(buf, sizeof(buf), "Then: UTC%s  (%s vs %s)", off, off2, LOCAL_TZ_LABEL);
        EPD_ShowString(8, 168, buf, 24, BLACK);
    } else {
        EPD_ShowString(8, 134, "Next change: none (no DST)", 24, BLACK);
        formatOffset(off2, sizeof(off2), curOffset - localNow);
        snprintf(buf, sizeof(buf), "Now: %s vs %s", off2, LOCAL_TZ_LABEL);
        EPD_ShowString(8, 168, buf, 24, BLACK);
    }
}

// True once the panel has been primed (fast-mode init + baseline clear) so
// EPD_PartUpdate() has a valid "old" frame to diff against.
static bool epdPrimed = false;

void pushFull() {
    EPD_FastMode1Init();
    EPD_Display_Clear();
    EPD_Update();
    EPD_Clear_R26A6H();
    EPD_Display(ImageBW);
    EPD_PartUpdate();
    epdPrimed = true;
}

void pushPartial() {
    if (!epdPrimed) {
        pushFull();
        return;
    }
    EPD_Display(ImageBW);
    EPD_PartUpdate();
}
