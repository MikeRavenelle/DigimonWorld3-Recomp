// What the in-game settings screen lists and how a row reads: the settings that apply while the game runs, with short
// labels that fit the game's menu window, and the rules for stepping a value left or right.
#pragma once

#include <string>
#include <vector>

#include "settings.h"

namespace dw3app::ingame {

struct Entry {
    const SettingInfo* info;
    std::string label;
    int step;  // Int settings: size of one left/right step
};

// Live, visible, non-text settings in menu order (visual preset first, then graphics, audio, system).
const std::vector<Entry>& entries();

// "Enhanced", "On", "4X", "100": the value as a few characters.
std::string value_label(const Settings& settings, const Entry& entry);
// "Preset: Enhanced"
std::string row_text(const Settings& settings, const Entry& entry);
// The text to give SettingsController::set for one step (direction -1 or +1), or empty when the value cannot move.
// Choices and the preset wrap around, switches flip, numbers stop at their limits.
std::string stepped_value(const Settings& settings, const Entry& entry, int direction);

}  // namespace dw3app::ingame
