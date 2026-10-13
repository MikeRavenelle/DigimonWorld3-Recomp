#include "monitors.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <vector>

namespace dw3app {
namespace {

struct SdlFree {
    void operator()(void* p) const { SDL_free(p); }
};

std::vector<SDL_DisplayID> display_ids() {
    int count = 0;
    std::unique_ptr<SDL_DisplayID[], SdlFree> ids(SDL_GetDisplays(&count));
    return ids ? std::vector<SDL_DisplayID>(ids.get(), ids.get() + count) : std::vector<SDL_DisplayID>();
}

ScreenMode to_mode(const SDL_DisplayMode& m) {
    return {m.w, m.h, static_cast<int>(std::lround(m.refresh_rate * 100.0f))};
}

std::vector<SDL_DisplayMode> fullscreen_modes(SDL_DisplayID id) {
    int count = 0;
    std::unique_ptr<SDL_DisplayMode*[], SdlFree> modes(SDL_GetFullscreenDisplayModes(id, &count));
    std::vector<SDL_DisplayMode> out;
    for (int i = 0; modes && i < count; ++i) out.push_back(*modes[i]);
    return out;
}

SDL_DisplayID target_display(SDL_Window* window, int monitor) {
    const auto ids = display_ids();
    if (monitor >= 1 && static_cast<std::size_t>(monitor) <= ids.size()) return ids[static_cast<std::size_t>(monitor) - 1];
    return SDL_GetDisplayForWindow(window);
}

void leave_fullscreen(SDL_Window* window) {
    SDL_SetWindowFullscreen(window, false);
    SDL_SyncWindow(window);
}

// The exclusive mode for the settings: the closest the monitor reports (the highest refresh rate when none is saved),
// or its desktop mode when no size is saved.
SDL_DisplayMode exclusive_mode(SDL_DisplayID display, const Settings::Graphics& g) {
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    SDL_DisplayMode result = desktop ? *desktop : SDL_DisplayMode{};
    if (g.fullscreen_width <= 0 || g.fullscreen_height <= 0) return result;
    const std::vector<SDL_DisplayMode> modes = fullscreen_modes(display);
    std::vector<ScreenMode> reported;
    for (const SDL_DisplayMode& m : modes) reported.push_back(to_mode(m));
    if (reported.empty()) return result;
    const int refresh = g.fullscreen_refresh > 0 ? g.fullscreen_refresh : INT_MAX / 4;
    return modes[nearest_mode(reported, {g.fullscreen_width, g.fullscreen_height, refresh})];
}

}  // namespace

MonitorList query_monitors(SDL_Window* window) {
    MonitorList list;
    const auto ids = display_ids();
    const SDL_DisplayID current = window ? SDL_GetDisplayForWindow(window) : 0;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        MonitorInfo info;
        const char* name = SDL_GetDisplayName(ids[i]);
        info.name = name ? name : "";
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(ids[i], &usable)) info.usable = {usable.w, usable.h};
        if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(ids[i])) info.desktop = to_mode(*desktop);
        for (const SDL_DisplayMode& m : fullscreen_modes(ids[i])) info.modes.push_back(to_mode(m));
        if (ids[i] == current) list.window_monitor = i;
        list.monitors.push_back(std::move(info));
    }
    return list;
}

void apply_display_settings(SDL_Window* window, const Settings::Graphics& g) {
    const bool is_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    const SDL_DisplayID target = target_display(window, g.monitor);
    const bool moves = target != 0 && target != SDL_GetDisplayForWindow(window);
    if (g.display_mode == static_cast<int>(DisplayMode::Windowed)) {
        if (is_fullscreen) leave_fullscreen(window);
        SDL_SetWindowSize(window, g.window_width, g.window_height);
        if (moves) SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(target), SDL_WINDOWPOS_CENTERED_DISPLAY(target));
        return;
    }
    if (is_fullscreen && moves) leave_fullscreen(window);
    if (moves) {
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(target, &bounds)) SDL_SetWindowPosition(window, bounds.x, bounds.y);
        SDL_SyncWindow(window);
    }
    if (g.display_mode == static_cast<int>(DisplayMode::Exclusive)) {
        const SDL_DisplayMode mode = exclusive_mode(target, g);
        SDL_SetWindowFullscreenMode(window, mode.w > 0 ? &mode : nullptr);
    } else {
        SDL_SetWindowFullscreenMode(window, nullptr);
    }
    SDL_SetWindowFullscreen(window, true);
    SDL_SyncWindow(window);
}

bool window_hdr_enabled(SDL_Window* window) {
    return SDL_GetBooleanProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
}

double window_refresh_hz(SDL_Window* window) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return mode ? mode->refresh_rate : 0.0;
}

}  // namespace dw3app
