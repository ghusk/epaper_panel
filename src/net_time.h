// WiFi connection, NTP sync, clock-freshness tracking, and timezone config
// validation.
#pragma once

#include <Arduino.h>

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
