#include "boot_config.h"

#include <Preferences.h>

#include "config.h"

// Menu button on the CrowPanel's built-in button cluster (see
// Elecrow-RD/ESP32_S3-Ink-Screen crowpanel_pins.h: IO_SW_MENU = GPIO2),
// wired active-low.
static const int ROTATION_TOGGLE_PIN = 2;

uint16_t getBootDisplayRotation() {
    pinMode(ROTATION_TOGGLE_PIN, INPUT_PULLUP);
    delay(50); // let the pull-up settle before sampling
    bool menuHeld = (digitalRead(ROTATION_TOGGLE_PIN) == LOW);

    Preferences prefs;
    prefs.begin("epaper", false);
    uint16_t rotation = prefs.getUShort("rotation", DISPLAY_ROTATION);

    if (menuHeld) {
        rotation = (rotation == 180) ? 0 : 180;
        prefs.putUShort("rotation", rotation);
        Serial.printf("Menu button held at boot: display rotation toggled to %u (saved)\n", rotation);
    } else {
        Serial.printf("Display rotation: %u\n", rotation);
    }

    prefs.end();
    return rotation;
}
