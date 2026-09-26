// Background (non-blocking) OpenWeatherMap fetch. The fetch runs on a
// FreeRTOS task pinned to core 0 so a DNS/connect hang (seen on some
// restrictive/guest WiFi networks, not bounded by HTTPClient's own timeouts)
// can never block setup()/loop(); callers wait up to a caller-supplied
// timeout and otherwise just pick up the result whenever it lands.
#pragma once

#include <Arduino.h>
#include <time.h>

struct WeatherSnapshot {
    bool valid = false;
    String city;
    String desc;
    int conditionId = 0;   // OpenWeatherMap condition code, e.g. 800 = clear
    bool isDay = true;     // from the icon code's 'd'/'n' suffix
    float temp = 0.0f;
    int humidity = 0;
    float wind = 0.0f;
    time_t fetchEpoch = 0;
};

// Must be called once before fetchWeatherAsync()/getWeatherSnapshot() (creates
// the mutex guarding the shared snapshot).
void weatherInit();

// Kicks off a weather fetch on a background task and waits up to timeoutMs
// for it to finish, but never longer than that. If still running afterwards,
// it's simply left running in the background and will update the snapshot
// whenever (if ever) it completes, picked up on a later call to
// getWeatherSnapshot().
void fetchWeatherAsync(uint32_t timeoutMs);

// Returns a thread-safe copy of the latest weather data.
WeatherSnapshot getWeatherSnapshot();
