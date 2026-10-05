// Polls the CrowPanel's jog-wheel ("rocker") buttons with debouncing.
#pragma once

#include <Arduino.h>

enum ButtonEvent {
    BTN_EVENT_NONE = 0,
    BTN_EVENT_DOWN,   // rocker pushed down, relative to the panel's orientation
    BTN_EVENT_UP,     // rocker pushed up, relative to the panel's orientation
    BTN_EVENT_PRESS,  // rocker pressed in (center)
};

// Configures the button pins. `rotation` is the display rotation (0 or 180);
// at 180 the panel is mounted upside down, so physical up/down are swapped.
void buttonsInit(uint16_t rotation);

// Returns at most one new press since the last call. Call frequently.
ButtonEvent buttonsPoll();
