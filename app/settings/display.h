// Display and pacing rules that do not need a window: which window sizes and screen modes to offer, how many monitor
// refreshes one emulated frame should last, and the brightness and gamma curve. The SDL queries that feed these
// live in the presentation code.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace dw3app {

struct ScreenSize {
    int w = 0, h = 0;
    bool operator==(const ScreenSize&) const = default;
};

// A video mode of a monitor; the refresh rate is in hundredths of a hertz (5994 for 59.94 Hz).
struct ScreenMode {
    int w = 0, h = 0;
    int refresh_chz = 0;
    bool operator==(const ScreenMode&) const = default;
};

struct MonitorInfo {
    std::string name;
    ScreenSize usable;     // the desktop area a window can use
    ScreenMode desktop;    // the mode the desktop runs in
    std::vector<ScreenMode> modes;
};

struct MonitorList {
    std::vector<MonitorInfo> monitors;
    std::size_t window_monitor = 0;  // index of the monitor the window is on, which auto stands for
    // The monitor the setting selects (0 is auto, 1 the first monitor). A number past the end means auto. Null only
    // when there are no monitors.
    const MonitorInfo* effective(int setting) const;
};

enum class DisplayMode { Windowed, Borderless, Exclusive };

// Common window sizes (4:3 first-class, plus 16:9 and 16:10) that fit in `usable`, smallest first. `current` is kept
// in the list even when it is not a common size, so that a size from settings.toml stays selectable.
std::vector<ScreenSize> windowed_sizes(ScreenSize usable, ScreenSize current);

// The modes a monitor reports, cleaned for a menu: tiny and duplicate modes dropped, smallest first, the highest
// refresh rate last within a size.
std::vector<ScreenMode> exclusive_modes(std::span<const ScreenMode> reported);

// Index of the entry closest to the wanted one (exact match first), or 0 for an empty list.
std::size_t nearest_size(std::span<const ScreenSize> list, ScreenSize wanted);
std::size_t nearest_mode(std::span<const ScreenMode> list, ScreenMode wanted);

std::string size_label(ScreenSize size);   // "1280x960"
std::string mode_label(ScreenMode mode);   // "1920x1080 144"

// How many monitor refreshes each emulated frame is shown for so that every frame lasts the same time: 1 at 60 Hz,
// 2 at 120 Hz, and so on. 0 when the refresh rate is no whole multiple of the console's 60 Hz (144 Hz, 75 Hz).
int refreshes_per_frame(double refresh_hz);

// The picture's gamma and brightness from the settings (percent values), and the curve the grading pass applies to
// one colour channel in 0..1. Gamma above 1 brightens the midtones; brightness is added afterwards.
float gamma_from_percent(int percent);
float brightness_from_percent(int percent);
float gamma_brightness_curve(float channel, float gamma, float brightness);

}  // namespace dw3app
