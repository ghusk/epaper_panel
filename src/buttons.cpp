#include "buttons.h"

// Jog-wheel pins on the CrowPanel 5.79", wired active-low.
static const int PIN_WHEEL_UP = 6;
static const int PIN_WHEEL_DOWN = 4;
static const int PIN_WHEEL_PRESS = 5;

static const unsigned long DEBOUNCE_MS = 30;

struct ButtonState {
    int pin;
    ButtonEvent event;
    bool stable;     // debounced level: true = pressed
    bool lastRaw;
    unsigned long changedAt;
};

static ButtonState buttons[3] = {
    { PIN_WHEEL_UP, BTN_EVENT_UP, false, false, 0 },
    { PIN_WHEEL_DOWN, BTN_EVENT_DOWN, false, false, 0 },
    { PIN_WHEEL_PRESS, BTN_EVENT_PRESS, false, false, 0 },
};

void buttonsInit(uint16_t rotation) {
    // The pin mapping above matches the 180 mounting; swap for 0.
    if (rotation == 0) {
        buttons[0].pin = PIN_WHEEL_DOWN;
        buttons[1].pin = PIN_WHEEL_UP;
    }
    for (auto &b : buttons) {
        pinMode(b.pin, INPUT_PULLUP);
        // Require release before the first press counts (e.g. held at boot).
        b.stable = b.lastRaw = (digitalRead(b.pin) == LOW);
    }
}

ButtonEvent buttonsPoll() {
    unsigned long now = millis();
    ButtonEvent result = BTN_EVENT_NONE;
    for (auto &b : buttons) {
        bool raw = (digitalRead(b.pin) == LOW);
        if (raw != b.lastRaw) {
            b.lastRaw = raw;
            b.changedAt = now;
        } else if (raw != b.stable && now - b.changedAt >= DEBOUNCE_MS) {
            b.stable = raw;
            if (raw && result == BTN_EVENT_NONE) result = b.event;
        }
    }
    return result;
}
