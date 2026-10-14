// Display and pacing rules that do not need a window: which window sizes and screen modes to offer, how many monitor
// refreshes one emulated frame should last, and the brightness and gamma curve. The SDL queries that feed these
// live in the presentation code.
#pragma once

#include <cstddef>
#include <cstdint>
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

// Decides how many times each emulated frame is presented, from the measured time between back-to-back presents
// instead of the refresh rate the system reports (a compositor such as gamescope can report 120 Hz and still deliver
// 60). The reported rate only says whether a probe is worth trying. A probe presents one frame several times; the
// gaps between those presents are the real vblank interval. Repeats are used only while the median gap really is
// about 1/120, 1/180 or 1/240 s, and are dropped at once when it is not or when emulation falls behind real time.
class PresentCadence {
public:
    static constexpr int kWindow = 5;                // gaps in the rolling median
    static constexpr int kMinSamples = 3;            // gaps needed before an active verdict can change
    static constexpr int kFirstWaitFrames = 120;     // frames to wait before probing again after a failure
    static constexpr int kMaxWaitFrames = 3600;
    static constexpr int64_t kMaxLagNs = 25000000;   // emulation this far behind real time ends the repeats

    // False whenever frames are not paced to the display (vsync off, pacing off, fast-forward, uncapped).
    void set_enabled(bool enabled);
    void set_reported_hz(double hz);

    // Presents for the next frame, at least 1.
    int plan_frame();
    // Time between two back-to-back presents of one frame completing.
    void on_present_gap(int64_t gap_ns);
    // After the frame: how far emulated time is behind real time (negative or zero when ahead). Returns true when
    // repeats were just dropped, so the caller restarts its real-time pacing instead of chasing the lag.
    bool end_frame(int64_t lag_ns);

    int repeats() const { return state_ == State::Active ? repeats_ : 1; }
    // The repeat count a median gap stands for: 2..4 when it is about 1/120, 1/180 or 1/240 s, otherwise 1.
    static int repeats_for_gap(int64_t gap_ns);

private:
    enum class State { Single, Probe, Active };
    int64_t median_gap() const;
    void fall_back();

    State state_ = State::Single;
    bool enabled_ = false;
    int candidate_ = 1;  // repeats the reported refresh suggests
    int repeats_ = 1;
    int wait_frames_ = kFirstWaitFrames, frames_waited_ = 0;
    int64_t gaps_[kWindow] = {};
    int gap_count_ = 0, gap_next_ = 0;
};

// The picture's gamma and brightness from the settings (percent values), and the curve the grading pass applies to
// one colour channel in 0..1. Gamma above 1 brightens the midtones; brightness is added afterwards.
float gamma_from_percent(int percent);
float brightness_from_percent(int percent);
float gamma_brightness_curve(float channel, float gamma, float brightness);

}  // namespace dw3app
