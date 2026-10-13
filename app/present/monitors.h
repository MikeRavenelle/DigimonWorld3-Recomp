// What SDL knows about the monitors, and the window changes the display settings ask for.
#pragma once

#include "display.h"
#include "settings.h"

struct SDL_Window;

namespace dw3app {

// The monitors with their usable desktop area and modes. `window` marks the monitor auto stands for (may be null).
MonitorList query_monitors(SDL_Window* window);

// Puts the window in the display mode, on the monitor and, when windowed, at the size the settings name. Exclusive
// mode switches the monitor to the nearest mode it offers.
void apply_display_settings(SDL_Window* window, const Settings::Graphics& graphics);

// The refresh rate of the monitor the window is on, in hertz; 0 when unknown.
double window_refresh_hz(SDL_Window* window);

}  // namespace dw3app
