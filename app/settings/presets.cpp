#include "presets.h"

#include <algorithm>

namespace dw3app {

namespace {

// Original: exactly the PlayStation picture, nothing added.
// Enhanced: sharper without changing how the game looks. 4x internal resolution only matters for the 3D parts (the
//   fields are pre-rendered 2D), true colour drops the 15-bit dithering, precise geometry removes the vertex wobble,
//   bilinear smooths the textures of 3D models only (flat 1:1 sprites and backgrounds stay as drawn, so the art is
//   not softened), and a linear scale to the window averages the extra samples.
// Remaster: the strongest the renderer offers today: 8x internal resolution and xBR smoothing of sprites as well.
constexpr Preset kPresets[kNamedPresets] = {
    {"original", "Original", {1, false, 0, false, 0}},
    {"enhanced", "Enhanced", {4, true, 1, true, 1}},
    {"remaster", "Remaster", {8, true, 2, true, 1}},
};

constexpr std::string_view kMembers[] = {"graphics.internal_res", "graphics.true_color", "graphics.texture_filter",
                                         "graphics.precise_geometry", "graphics.output_filter"};

}  // namespace

const Preset& preset(int id) { return kPresets[std::clamp(id, 0, kNamedPresets - 1)]; }

std::string_view preset_label(int id) { return id >= 0 && id < kNamedPresets ? kPresets[id].label : "Custom"; }

GraphicsBundle bundle_of(const Settings& s) {
    const auto& g = s.graphics;
    return {g.internal_res, g.true_color, g.texture_filter, g.precise_geometry, g.output_filter};
}

void apply_bundle(Settings& s, const GraphicsBundle& b) {
    auto& g = s.graphics;
    g.internal_res = b.internal_res;
    g.true_color = b.true_color;
    g.texture_filter = b.texture_filter;
    g.precise_geometry = b.precise_geometry;
    g.output_filter = b.output_filter;
}

int match_preset(const Settings& s) {
    const GraphicsBundle now = bundle_of(s);
    for (int i = 0; i < kNamedPresets; ++i)
        if (kPresets[i].graphics == now) return i;
    return kPresetCustom;
}

void normalize_preset(Settings& s) {
    const int m = match_preset(s);
    s.graphics.preset = m;
    if (m != kPresetCustom) s.graphics.preset_base = m;
}

std::span<const std::string_view> preset_member_ids() { return kMembers; }

}  // namespace dw3app
