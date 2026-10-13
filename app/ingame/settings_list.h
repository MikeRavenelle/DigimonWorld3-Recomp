// What the in-game settings screen lists and how a row reads: the settings that apply while the game runs, with short
// labels that fit the game's menu window, and the rules for stepping a value left or right.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "glyphs.h"
#include "settings.h"

namespace dw3app::ingame {

// The screen is split into pages; the header names the page and L1 / R1 jump between them.
enum class Page { Graphics, Audio, Controls, Gameplay };
std::string_view page_name(Page p);

enum class Kind {
    Setting,  // left and right change the value
    Binding,  // cross waits for the next key or button
    Reset,    // cross restores every control setting
};

struct Entry {
    const SettingInfo* info;  // null for Binding and Reset rows
    std::string label;
    int step;  // Int settings: size of one left/right step
    Page page = Page::Graphics;
    Kind kind = Kind::Setting;
    input::Action action = input::Action::Up;  // Binding rows
};

// What a row needs beyond the settings to read right.
struct RowContext {
    input::PromptStyle style = input::PromptStyle::Xbox;  // names of gamepad buttons (never Auto)
    std::optional<input::Action> capturing;               // the binding row that waits for a key
};

// Rows in menu order: the visual preset first, then graphics, audio, controls (the live settings, one row per
// binding, a reset row) and gameplay.
const std::vector<Entry>& entries();
// The entries of the page the entry at `index` belongs to: first index and count.
std::pair<std::size_t, std::size_t> page_range(std::size_t index);
// The first entry of the next page (direction 1) or the previous one (-1), wrapping around.
std::size_t jump_page(std::size_t index, int direction);
// Setting changes that restore every control setting and binding to its default: (id, value text).
std::vector<std::pair<std::string, std::string>> controls_reset_changes();

// "Enhanced", "On", "4X", "100": the value as a few characters.
std::string value_label(const Settings& settings, const Entry& entry, const RowContext& context = {});
// "Preset: Enhanced", "Cross: Z/A"
std::string row_text(const Settings& settings, const Entry& entry, const RowContext& context = {});
// The text to give SettingsController::set for one step (direction -1 or +1), or empty when the value cannot move
// (always for Binding and Reset rows).
// Choices and the preset wrap around, switches flip, numbers stop at their limits.
std::string stepped_value(const Settings& settings, const Entry& entry, int direction);

}  // namespace dw3app::ingame
