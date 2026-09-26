#include "net_time.h"

#include <WiFi.h>
#include <time.h>

#include "config.h"

// A time_t before this is considered "not yet synced" (roughly Nov 2023).
static const time_t MIN_VALID_EPOCH = 1700000000;

bool ntpEverSynced = false;
unsigned long lastSuccessfulSyncMillis = 0;
bool tzConfigWarning = false;

bool connectWiFi() {
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

bool syncNtp() {
    if (WiFi.status() != WL_CONNECTED) return false;

    Serial.printf("Syncing time via NTP (gateway=%s, dns=%s, rssi=%ddBm)...\n",
                  WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, NTP_SERVER_LOCAL, NTP_SERVER_FALLBACK_1, NTP_SERVER_FALLBACK_2);

    // A single dropped UDP request (e.g. weak signal from a marginal power
    // source) would otherwise leave us waiting on lwIP's own much longer
    // internal backoff; re-issuing configTime() periodically resends the
    // request sooner, without extending the worst-case wait by much.
    const int RESEND_EVERY_ATTEMPTS = 16; // ~8s at 500ms/attempt
    time_t now = time(nullptr);
    int attempts = 0;
    while (now < MIN_VALID_EPOCH && attempts < 90) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
        attempts++;
        if (now < MIN_VALID_EPOCH && attempts % RESEND_EVERY_ATTEMPTS == 0) {
            configTime(0, 0, NTP_SERVER_LOCAL, NTP_SERVER_FALLBACK_1, NTP_SERVER_FALLBACK_2);
        }
    }
    Serial.println();

    bool ok = now >= MIN_VALID_EPOCH;
    Serial.println(ok ? "NTP sync OK" : "NTP sync FAILED");
    return ok;
}

bool isTimeFresh() {
    if (!ntpEverSynced) return false;
    return (millis() - lastSuccessfulSyncMillis) < NTP_STALE_THRESHOLD_MS;
}

// ---------------------------------------------------------------------------
// Timezone config validation
// ---------------------------------------------------------------------------
// Catches malformed/mislabeled POSIX TZ strings (e.g. an IANA name where a
// POSIX string is required, or a plausible-looking string that resolves to an
// unexpected UTC offset) before they silently show the wrong time.

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

void validateTimezoneConfig() {
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
