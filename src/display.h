// Renders the clock/weather layout into the panel's screen buffer and pushes
// full or partial refreshes to the physical display.
#pragma once

#include <Arduino.h>

// Draws the full layout (header, local clock, weather, 4 timezones, footer)
// into the internal screen buffer for the given freshness state. Does not
// touch the physical panel; call pushFull()/pushPartial() afterwards.
void renderBuffer(bool timeFresh);

// Full re-init + a full-waveform clear/redraw. Visibly flashes the whole
// panel, so this is only used for the initial draw and the periodic
// ghost-clearing refresh - not every minute.
void pushFull();

// Loads the current buffer and triggers a partial refresh only - the panel
// is left initialized (no re-init, no deep sleep) so only the changed
// pixels redraw instead of flashing the whole screen every minute.
void pushPartial();
