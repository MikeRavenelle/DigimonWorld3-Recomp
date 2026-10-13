#include "settings_list.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "presets.h"

namespace dw3app::ingame {
namespace {

struct Labels {
    std::string_view id, label;
    int step;
};

// Short names, in the order the screen lists them.
constexpr Labels kLabels[] = {
    {"graphics.preset", "Preset", 1},
    {"graphics.comparison_preset", "F9 Preset", 1},
    {"graphics.internal_res", "3D Scale", 1},
    {"graphics.true_color", "True Color", 1},
    {"graphics.texture_filter", "Texture", 1},
    {"graphics.precise_geometry", "Precise 3D", 1},
    {"graphics.hd_backgrounds", "HD BG", 1},
    {"graphics.output_filter", "Output", 1},
    {"graphics.antialiasing", "AA", 1},
    {"graphics.sharpening", "Sharpen", 1},
    {"graphics.color_grade", "Color", 1},
    {"graphics.crt", "CRT", 1},
    {"graphics.scale_mode", "Scaling", 1},
    {"graphics.window_width", "Win Width", 160},
    {"graphics.window_height", "Win Height", 120},
    {"graphics.fullscreen", "Fullscreen", 1},
    {"graphics.vsync", "VSync", 1},
    {"audio.mute", "Mute", 1},
    {"audio.volume", "Volume", 10},
    {"system.fast_forward_speed", "Fast Forward", 1},
    {"gameplay.encounters", "Encounters", 1},
    {"gameplay.game_speed", "Game Speed", 1},
    {"gameplay.god_mode", "God Mode", 1},
};

std::string capitalised(std::string_view name) {
    std::string s(name);
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string choice_label(std::string_view name) {
    if (name == "xbr") return "xBR";
    if (name == "smaa") return "SMAA";
    if (name == "fsr") return "FSR";
    if (name == "crt") return "Full";
    if (name.size() == 2 && name[1] == 'x' && std::isdigit(static_cast<unsigned char>(name[0]))) return std::string(1, name[0]) + "X";
    if (name == "unlimited") return "Max";
    return capitalised(name);
}

int choice_index(const Settings& settings, const SettingInfo& info) {
    const std::string current = get_text(settings, info);
    for (std::size_t i = 0; i < info.choices.size(); ++i)
        if (info.choices[i] == current) return static_cast<int>(i);
    return -1;
}

// The preset cycles through the three named presets; Custom is only ever a result of changing the options.
std::string stepped_preset(const Settings& settings, int direction) {
    const int current = settings.graphics.preset;
    const int from = current == kPresetCustom ? settings.graphics.preset_base : current;
    const int next = (from + direction + kNamedPresets) % kNamedPresets;
    return std::string(preset(next).name);
}

}  // namespace

const std::vector<Entry>& entries() {
    static const std::vector<Entry> list = [] {
        const auto listed = [](const SettingInfo& info) {
            return !info.hidden && info.apply == Apply::Live && info.type != Type::String;
        };
        std::vector<Entry> out;
        for (const Labels& l : kLabels) {
            const SettingInfo* info = find_setting(l.id);
            if (info && listed(*info)) out.push_back({info, std::string(l.label), l.step});
        }
        // A live setting added later shows up with its full label until it gets a short one above.
        for (const SettingInfo& info : all_settings()) {
            const bool known = std::any_of(out.begin(), out.end(), [&](const Entry& e) { return e.info == &info; });
            if (listed(info) && !known) out.push_back({&info, std::string(info.label), 1});
        }
        return out;
    }();
    return list;
}

std::string value_label(const Settings& settings, const Entry& entry) {
    const SettingInfo& info = *entry.info;
    switch (info.type) {
        case Type::Bool: return get_text(settings, info) == "true" ? "On" : "Off";
        case Type::Int: {
            std::string v = get_text(settings, info);
            if (info.id == "graphics.internal_res") v += "X";
            return v;
        }
        case Type::Choice: return choice_label(get_text(settings, info));
        case Type::String: return get_text(settings, info);
    }
    return "";
}

std::string row_text(const Settings& settings, const Entry& entry) {
    return entry.label + ": " + value_label(settings, entry);
}

std::string stepped_value(const Settings& settings, const Entry& entry, int direction) {
    const SettingInfo& info = *entry.info;
    switch (info.type) {
        case Type::Bool: return get_text(settings, info) == "true" ? "false" : "true";
        case Type::Int: {
            const int current = std::stoi(get_text(settings, info));
            const int next = std::clamp(current + direction * entry.step, info.min, info.max);
            return next == current ? "" : std::to_string(next);
        }
        case Type::Choice: {
            if (info.id == "graphics.preset") return stepped_preset(settings, direction);
            const int n = static_cast<int>(info.choices.size());
            const int at = std::max(choice_index(settings, info), 0);
            return std::string(info.choices[static_cast<std::size_t>((at + direction + n) % n)]);
        }
        case Type::String: return "";
    }
    return "";
}

}  // namespace dw3app::ingame
