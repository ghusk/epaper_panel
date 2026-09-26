#pragma once

// ----------------------------------------------------------------------------
// User configuration for the CrowPanel ESP32-S3 5.79" e-paper clock/weather panel
// ----------------------------------------------------------------------------
// Copy this file to config.h and fill in your own values. config.h is
// gitignored so real credentials/API keys never get committed.

// ---------------- WiFi ----------------
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define WIFI_CONNECT_TIMEOUT_MS   20000

// ---------------- NTP ----------------
// Primary NTP source should be a server on your local network (e.g. a router,
// Pi-hole, or dedicated NTP appliance). Public pool servers are used as a
// fallback if the local source cannot be reached.
#define NTP_SERVER_LOCAL    "192.168.1.1"
#define NTP_SERVER_FALLBACK_1 "pool.ntp.org"
#define NTP_SERVER_FALLBACK_2 "time.cloudflare.com"

// How often the clock display is redrawn from the internally kept time.
#define DISPLAY_UPDATE_INTERVAL_MS   (60UL * 1000UL)        // 1 minute

// How often the internal clock is re-verified against the NTP source.
#define NTP_RESYNC_INTERVAL_MS       (60UL * 60UL * 1000UL) // 1 hour

// Until the first successful sync, retry this often instead of waiting a
// full hour (e.g. NTP server slow/unreachable at boot on a flaky network).
#define NTP_RETRY_INTERVAL_MS        (60UL * 1000UL)        // 1 minute

// Stop the fast retries after this long since boot and fall back to the
// normal hourly cadence, so a persistent NTP outage doesn't keep retrying
// WiFi/NTP indefinitely (battery drain concern if LAN is up but NTP isn't
// responding).
#define NTP_RETRY_WINDOW_MS          (5UL * 60UL * 1000UL)  // 5 minutes

// How often the weather is re-fetched.
#define WEATHER_REFRESH_INTERVAL_MS  (15UL * 60UL * 1000UL) // 15 minutes

// If a successful NTP sync hasn't happened within this window, the on-screen
// "stale" indicator is shown instead of the "fresh" one.
#define NTP_STALE_THRESHOLD_MS       (2UL * 60UL * 60UL * 1000UL) // 2 hours

// Do a full (ghost-clearing) refresh after this many partial-update cycles.
#define FULL_REFRESH_EVERY_N_UPDATES  30

// ---------------- Display ----------------
// Panel mounting orientation. Use 180 if the panel is mounted with its cable
// exiting the opposite side from the vendor's default, 0 otherwise. The
// vendor driver only supports 0/90/180/270; this firmware only exercises
// 0 and 180 since the panel is wider than it is tall.
#define DISPLAY_ROTATION  180

// ---------------- Timezones ----------------
// Local time uses this POSIX TZ string. Find your string at:
// https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv
//
// NOTE: some regions (e.g. BC and Alberta) have moved to permanent DST (no
// seasonal fall-back to standard time). A POSIX TZ string with no dst-rule
// suffix is a fixed, non-transitioning offset, so permanent-DST zones are
// expressed as a plain "std offset" using the DST abbreviation/offset.
#define LOCAL_TZ_LABEL   "Local"
#define LOCAL_TZ_POSIX   "GMT0BST,M3.5.0/1,M10.5.0"   // Example: UK

// Four additional configurable timezones shown alongside local time.
struct TimezoneConfig {
    const char *label;
    const char *posixTz;
};

static const TimezoneConfig TIMEZONES[4] = {
    { "New York",  "EST5EDT,M3.2.0,M11.1.0" },
    { "London",    "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Tokyo",     "JST-9" },
    { "Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};

// ---------------- Weather ----------------
// Create a free API key at https://openweathermap.org/api
#define OWM_API_KEY      "YOUR_OPENWEATHERMAP_API_KEY"
#define OWM_CITY_QUERY   "London,GB"     // "City,CountryCode"
#define OWM_UNITS        "metric"        // "metric" (C) or "imperial" (F)

// Weather is refreshed on its own cadence (see WEATHER_REFRESH_INTERVAL_MS above).
