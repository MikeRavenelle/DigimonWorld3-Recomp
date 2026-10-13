// Visual presets: named bundles of the graphics settings that can change while the game runs.
#pragma once

#include <span>
#include <string_view>

#include "settings.h"

namespace dw3app {

enum PresetId { kPresetOriginal = 0, kPresetEnhanced = 1, kPresetRemaster = 2, kPresetCustom = 3 };
constexpr int kNamedPresets = 3;

// The settings a preset sets. Every member must be live-applicable: switching presets never needs a restart, which
// is what lets the comparison hotkey flip between them mid-game. Settings that choose the API or the window
// (renderer, window size, vsync, scale mode) are never part of a preset.
struct GraphicsBundle {
    int internal_res = 1;
    bool true_color = false;
    int texture_filter = 0;
    bool precise_geometry = false;
    int output_filter = 0;
    bool operator==(const GraphicsBundle&) const = default;
};

struct Preset {
    std::string_view name;   // the value stored in settings.toml
    std::string_view label;  // for the screen
    GraphicsBundle graphics;
    // Remaster is the home of the features that are still to come (a post-processing chain, replacement textures
    // for the pre-rendered backgrounds). They belong here as further members, set per preset next to `graphics`,
    // and are applied by the same code that applies `graphics`.
};

const Preset& preset(int id);            // id 0..2
std::string_view preset_label(int id);   // also for custom
GraphicsBundle bundle_of(const Settings& s);
void apply_bundle(Settings& s, const GraphicsBundle& b);
// The named preset whose bundle equals the current graphics settings, or kPresetCustom.
int match_preset(const Settings& s);
// Re-derives graphics.preset (and the base) from the values.
void normalize_preset(Settings& s);
// The ids of the settings a preset covers.
std::span<const std::string_view> preset_member_ids();

}  // namespace dw3app
