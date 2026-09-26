// Runtime display-rotation selection: normally comes from the compile-time
// DISPLAY_ROTATION default, but can be flipped (0<->180) in the field by
// holding the Menu button (GPIO2, active low) while the board powers on or
// resets. The choice is persisted in NVS so it survives future resets
// without needing to hold the button again.
#pragma once

#include <Arduino.h>

// Must be called once, early in setup(). Reads/toggles/persists the stored
// rotation and returns the value to use for this boot (0 or 180).
uint16_t getBootDisplayRotation();
