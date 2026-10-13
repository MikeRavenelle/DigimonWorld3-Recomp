#include "display.h"

#include <algorithm>
#include <cmath>

namespace dw3app {
namespace {

constexpr ScreenSize kCommonSizes[] = {
    {640, 480},   {800, 600},   {960, 720},   {1024, 768},  {1280, 960},  {1440, 1080}, {1600, 1200}, {1920, 1440},
    {2560, 1920}, {3200, 2400}, {1280, 720},  {1600, 900},  {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160},
    {1280, 800},  {1440, 900},  {1680, 1050}, {1920, 1200}, {2560, 1600},
};

constexpr int kMinModeWidth = 640, kMinModeHeight = 480;
constexpr int kMaxRefreshesPerFrame = 8;
constexpr double kRefreshTolerance = 0.5;  // Hz away from the console's 60 that still counts as even (59.94 NTSC, 60)

long long pixels(ScreenSize s) { return static_cast<long long>(s.w) * s.h; }

bool smaller(ScreenSize a, ScreenSize b) { return pixels(a) != pixels(b) ? pixels(a) < pixels(b) : a.w < b.w; }

int whole_hertz(int refresh_chz) { return (refresh_chz + 50) / 100; }

}  // namespace

const MonitorInfo* MonitorList::effective(int setting) const {
    if (monitors.empty()) return nullptr;
    if (setting >= 1 && static_cast<std::size_t>(setting) <= monitors.size()) return &monitors[static_cast<std::size_t>(setting) - 1];
    return &monitors[std::min(window_monitor, monitors.size() - 1)];
}

std::vector<ScreenSize> windowed_sizes(ScreenSize usable, ScreenSize current) {
    std::vector<ScreenSize> out;
    for (const ScreenSize s : kCommonSizes)
        if (s.w <= usable.w && s.h <= usable.h) out.push_back(s);
    if (current.w > 0 && current.h > 0 && std::find(out.begin(), out.end(), current) == out.end()) out.push_back(current);
    std::sort(out.begin(), out.end(), smaller);
    return out;
}

std::vector<ScreenMode> exclusive_modes(std::span<const ScreenMode> reported) {
    std::vector<ScreenMode> out;
    for (const ScreenMode& m : reported) {
        if (m.w < kMinModeWidth || m.h < kMinModeHeight) continue;
        const bool duplicate = std::any_of(out.begin(), out.end(), [&](const ScreenMode& o) {
            return o.w == m.w && o.h == m.h && whole_hertz(o.refresh_chz) == whole_hertz(m.refresh_chz);
        });
        if (!duplicate) out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const ScreenMode& a, const ScreenMode& b) {
        if (a.w != b.w || a.h != b.h) return smaller({a.w, a.h}, {b.w, b.h});
        return a.refresh_chz < b.refresh_chz;
    });
    return out;
}

std::size_t nearest_size(std::span<const ScreenSize> list, ScreenSize wanted) {
    std::size_t best = 0;
    long long best_distance = -1;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const long long d = std::llabs(pixels(list[i]) - pixels(wanted)) + std::llabs(list[i].w - wanted.w);
        if (best_distance < 0 || d < best_distance) {
            best = i;
            best_distance = d;
        }
    }
    return best;
}

std::size_t nearest_mode(std::span<const ScreenMode> list, ScreenMode wanted) {
    std::size_t best = 0;
    long long best_distance = -1;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const long long d = std::llabs(pixels({list[i].w, list[i].h}) - pixels({wanted.w, wanted.h})) * 100000 +
                            std::llabs(list[i].refresh_chz - wanted.refresh_chz);
        if (best_distance < 0 || d < best_distance) {
            best = i;
            best_distance = d;
        }
    }
    return best;
}

std::string size_label(ScreenSize size) { return std::to_string(size.w) + "x" + std::to_string(size.h); }

std::string mode_label(ScreenMode mode) {
    return size_label({mode.w, mode.h}) + " " + std::to_string(whole_hertz(mode.refresh_chz));
}

int refreshes_per_frame(double refresh_hz) {
    if (refresh_hz < 50.0) return 0;
    const int n = std::max(1, static_cast<int>(std::lround(refresh_hz / 60.0)));
    if (n > kMaxRefreshesPerFrame) return 0;
    return std::fabs(refresh_hz / n - 60.0) <= kRefreshTolerance ? n : 0;
}

float gamma_from_percent(int percent) { return static_cast<float>(percent) / 100.0f; }

float brightness_from_percent(int percent) { return static_cast<float>(percent) / 200.0f; }

float gamma_brightness_curve(float channel, float gamma, float brightness) {
    const float shaped = std::pow(std::clamp(channel, 0.0f, 1.0f), 1.0f / gamma);
    return std::clamp(shaped + brightness, 0.0f, 1.0f);
}

}  // namespace dw3app
