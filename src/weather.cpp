#include "weather.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "config.h"

// Guards weatherSnapshot below, since it's written from the background fetch
// task (see weatherTaskFn) while getWeatherSnapshot() may be called
// concurrently from the main loop.
static SemaphoreHandle_t weatherMutex = nullptr;
static WeatherSnapshot weatherSnapshot;
static volatile bool weatherTaskRunning = false;
static volatile bool weatherTaskDone = false;

void weatherInit() {
    weatherMutex = xSemaphoreCreateMutex();
}

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
            weatherSnapshot.city = city;
            weatherSnapshot.desc = desc;
            weatherSnapshot.conditionId = conditionId;
            weatherSnapshot.isDay = !iconCode.endsWith("n");
            weatherSnapshot.temp = temp;
            weatherSnapshot.humidity = humidity;
            weatherSnapshot.wind = wind;
            weatherSnapshot.fetchEpoch = time(nullptr);
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
    weatherSnapshot.valid = ok;
    xSemaphoreGive(weatherMutex);

    weatherTaskDone = true;
    weatherTaskRunning = false;
    vTaskDelete(nullptr);
}

void fetchWeatherAsync(uint32_t timeoutMs) {
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

WeatherSnapshot getWeatherSnapshot() {
    xSemaphoreTake(weatherMutex, portMAX_DELAY);
    WeatherSnapshot copy = weatherSnapshot;
    xSemaphoreGive(weatherMutex);
    return copy;
}
