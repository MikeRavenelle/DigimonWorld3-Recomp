// Left stick as a D-pad, and the pad selection rules for hotplug. Header-only, no SDL.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

#include "dw3/sio.h"

namespace dw3app::input {

constexpr int kDeadzoneMin = 10, kDeadzoneMax = 50, kDeadzoneDefault = 25;

// A stick axis as a fraction of full travel, -1 to 1 (SDL axes run from -32768 to 32767).
inline float axis_fraction(int16_t raw) { return std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f); }

// The D-pad directions (dw3::PadButton bits) a stick position presses. The deadzone is a circle around the centre,
// as a percentage of full travel. Outside it the stick presses the axis it leans on, and both axes on a diagonal: an
// axis counts once it is at least 38% of the stick's distance from centre (the diagonal sectors are 45 degrees wide).
// SDL's y axis grows downward, so positive y is Down.
inline uint16_t stick_dpad_bits(int16_t raw_x, int16_t raw_y, int deadzone_percent) {
    const float x = axis_fraction(raw_x), y = axis_fraction(raw_y);
    const float distance = std::hypot(x, y);
    if (distance < static_cast<float>(deadzone_percent) / 100.0f || distance == 0.0f) return 0;
    const float threshold = distance * 0.38f;
    uint16_t bits = 0;
    if (x <= -threshold) bits |= dw3::kPadLeft;
    if (x >= threshold) bits |= dw3::kPadRight;
    if (y <= -threshold) bits |= dw3::kPadUp;
    if (y >= threshold) bits |= dw3::kPadDown;
    return bits;
}

// A trigger counts as pressed past a quarter of its travel.
inline bool trigger_pressed(int16_t raw) { return raw > 8192; }

// The pad that drives port 1 among the connected ones (in the order SDL lists them). `preference` is the setting:
// 0 for Auto, otherwise the 1-based position in that list. Auto keeps the pad already in use while it stays
// connected, otherwise takes the first; a position beyond the connected pads falls back to Auto.
template <class Id>
std::optional<Id> choose_pad(std::span<const Id> connected, int preference, std::optional<Id> current) {
    if (connected.empty()) return std::nullopt;
    if (preference >= 1 && static_cast<std::size_t>(preference) <= connected.size()) return connected[static_cast<std::size_t>(preference - 1)];
    if (current && std::find(connected.begin(), connected.end(), *current) != connected.end()) return current;
    return connected.front();
}

}  // namespace dw3app::input
