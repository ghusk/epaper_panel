// WiFi connection, NTP sync, clock-freshness tracking, and timezone config
// validation.
#pragma once

#include <Arduino.h>
#include <time.h>

// Connects to WiFi if not already connected. Returns true if connected
// (either already, or after a successful connection attempt).
bool connectWiFi();

// Blocks (briefly) syncing the system clock via NTP. Returns true on success.
bool syncNtp();

// True once at least one NTP sync has ever succeeded.
extern bool ntpEverSynced;

// millis() timestamp of the last successful NTP sync.
extern unsigned long lastSuccessfulSyncMillis;

// True if the clock has been NTP-verified recently enough to be trusted.
bool isTimeFresh();

// Sanity-checks all configured POSIX TZ strings (LOCAL_TZ_POSIX + TIMEZONES)
// and sets tzConfigWarning if any looks suspicious. Leaves TZ set to
// LOCAL_TZ_POSIX on return.
void validateTimezoneConfig();

// True if validateTimezoneConfig() found a suspicious timezone config.
extern bool tzConfigWarning;

// UTC offset in seconds (positive east of UTC) that the given POSIX TZ string
// yields at time t. Leaves the process TZ set to `tz`; callers restore it.
long tzOffsetAt(const char *tz, time_t t);

// Finds the first moment after `from` (within ~400 days) at which the UTC offset
// of `tz` changes (DST start/end), to 1-second precision. Returns false if the
// zone has no change in that window. Leaves the process TZ set to `tz`.
bool findNextOffsetChange(const char *tz, time_t from, time_t *outChange);
