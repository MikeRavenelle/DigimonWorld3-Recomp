#include "settings.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "dw3/platform.h"
#include "presets.h"

namespace dw3app {

namespace {

constexpr std::string_view kRenderers[] = {"auto", "gpu", "vulkan", "d3d12", "metal", "software"};
constexpr std::string_view kPresetNames[] = {"original", "enhanced", "remaster", "custom"};
constexpr std::string_view kBaseNames[] = {"original", "enhanced", "remaster"};
constexpr std::string_view kComparisonNames[] = {"original", "enhanced", "remaster"};
constexpr std::string_view kTextureFilters[] = {"nearest", "bilinear", "xbr"};
constexpr std::string_view kOutputFilters[] = {"nearest", "linear"};
constexpr std::string_view kScaleModes[] = {"fit", "integer", "stretch"};
constexpr std::string_view kSpeeds[] = {"2x", "4x", "unlimited"};
constexpr std::string_view kEncounters[] = {"normal", "half", "off", "double"};
constexpr std::string_view kGameSpeeds[] = {"1x", "2x", "3x", "4x"};

#define DW3_FIELD(path) +[](Settings& s) -> void* { return &s.path; }

const SettingInfo kSettings[] = {
    {"graphics.renderer", "Renderer", "Graphics API: auto picks the best available and falls back to the software path.",
     Group::Graphics, Type::Choice, Apply::Restart, 0, 0, kRenderers, false, DW3_FIELD(graphics.renderer)},
    {"graphics.preset", "Visual preset", "Original is the exact PlayStation picture; Enhanced and Remaster add the options below. Custom means the options below were changed after picking one.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kPresetNames, false, DW3_FIELD(graphics.preset)},
    {"graphics.preset_base", "Preset the custom settings started from", "Kept so that Custom can show which preset it was based on.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kBaseNames, true, DW3_FIELD(graphics.preset_base)},
    {"graphics.comparison_preset", "Comparison hotkey preset", "What the comparison hotkey (F9) switches Original to.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, std::span<const std::string_view>(kComparisonNames).subspan(1), false,
     DW3_FIELD(graphics.comparison_preset)},
    {"graphics.internal_res", "Internal resolution", "Multiplier of the 3D rendering resolution (1 is native). Pre-rendered 2D fields are not affected.",
     Group::Graphics, Type::Int, Apply::Live, 1, 8, {}, false, DW3_FIELD(graphics.internal_res)},
    {"graphics.true_color", "True colour", "Full 8-bit shading and blending on screen instead of the 15-bit dithered output.",
     Group::Graphics, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(graphics.true_color)},
    {"graphics.texture_filter", "Texture filter", "nearest is the original look; bilinear smooths 3D textures; xbr also smooths 2D sprites (needs internal resolution above 1).",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kTextureFilters, false, DW3_FIELD(graphics.texture_filter)},
    {"graphics.precise_geometry", "Precise geometry", "Sub-pixel 3D vertices and perspective-correct textures (best with internal resolution above 1).",
     Group::Graphics, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(graphics.precise_geometry)},
    {"graphics.output_filter", "Output filter", "How the picture is scaled to the window.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kOutputFilters, false, DW3_FIELD(graphics.output_filter)},
    {"graphics.scale_mode", "Scaling", "fit keeps 4:3 with borders, integer uses whole multiples only, stretch fills the window.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kScaleModes, false, DW3_FIELD(graphics.scale_mode)},
    {"graphics.window_width", "Window width", "Window width in pixels.",
     Group::Graphics, Type::Int, Apply::Live, 160, 16384, {}, false, DW3_FIELD(graphics.window_width)},
    {"graphics.window_height", "Window height", "Window height in pixels.",
     Group::Graphics, Type::Int, Apply::Live, 120, 16384, {}, false, DW3_FIELD(graphics.window_height)},
    {"graphics.fullscreen", "Fullscreen", "Borderless fullscreen (F11 or Alt+Enter toggles it while playing).",
     Group::Graphics, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(graphics.fullscreen)},
    {"graphics.vsync", "Vertical sync", "Wait for the display refresh when presenting.",
     Group::Graphics, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(graphics.vsync)},

    {"audio.mute", "Mute", "No sound output.",
     Group::Audio, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(audio.mute)},
    {"audio.volume", "Volume", "Output volume in percent.",
     Group::Audio, Type::Int, Apply::Live, 0, 100, {}, false, DW3_FIELD(audio.volume)},
    {"audio.device", "Output device", "Name of the playback device; empty uses the system default.",
     Group::Audio, Type::String, Apply::Live, 0, 0, {}, false, DW3_FIELD(audio.device)},

    {"system.memcards", "Memory cards", "Insert memory cards; off means games see no card.",
     Group::System, Type::Bool, Apply::Restart, 0, 0, {}, false, DW3_FIELD(system.memcards)},
    {"system.memcard1", "Memory card 1", "Path of the slot 1 card image; empty uses <user data>/memcards/slot1.mcd.",
     Group::System, Type::String, Apply::Restart, 0, 0, {}, false, DW3_FIELD(system.memcard1)},
    {"system.memcard2", "Memory card 2", "Path of the slot 2 card image; empty uses <user data>/memcards/slot2.mcd (inserted only if it exists).",
     Group::System, Type::String, Apply::Restart, 0, 0, {}, false, DW3_FIELD(system.memcard2)},
    {"system.fast_forward_speed", "Fast-forward speed", "Speed while fast-forwarding (hold Tab, or press F10 to latch it).",
     Group::System, Type::Choice, Apply::Live, 0, 0, kSpeeds, false, DW3_FIELD(system.fast_forward_speed)},

    {"gameplay.encounters", "Random encounters", "How often random battles start while walking: normal, half as often, never, or twice as often.",
     Group::Gameplay, Type::Choice, Apply::Live, 0, 0, kEncounters, false, DW3_FIELD(gameplay.encounters)},
    {"gameplay.game_speed", "Game speed", "Runs the whole game faster, with sound kept at its normal pitch (a hotkey toggles it).",
     Group::Gameplay, Type::Choice, Apply::Live, 0, 0, kGameSpeeds, false, DW3_FIELD(gameplay.game_speed)},
    {"gameplay.god_mode", "God mode", "The party takes no damage and its attacks defeat enemies in one hit.",
     Group::Gameplay, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(gameplay.god_mode)},
};

#undef DW3_FIELD

bool is_preset_member(std::string_view id) {
    for (std::string_view m : preset_member_ids())
        if (m == id) return true;
    return false;
}

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

int find_choice(const SettingInfo& info, std::string_view text) {
    const std::string l = lower(text);
    for (std::size_t i = 0; i < info.choices.size(); ++i)
        if (info.choices[i] == l) return static_cast<int>(i);
    return -1;
}

std::string choice_list(const SettingInfo& info) {
    std::string r;
    for (std::string_view c : info.choices) r += (r.empty() ? "" : ", ") + std::string(c);
    return r;
}

bool parse_int(std::string_view text, int& out) {
    const std::string t(text);
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (t.empty() || *end != '\0' || errno != 0 || v < INT32_MIN || v > INT32_MAX) return false;
    out = static_cast<int>(v);
    return true;
}

// Writes a validated value. For Choice and Int the caller has already checked the range.
void store(Settings& s, const SettingInfo& info, int number, bool flag, const std::string& text) {
    void* f = info.field(s);
    switch (info.type) {
        case Type::Bool: *static_cast<bool*>(f) = flag; break;
        case Type::Int:
        case Type::Choice: *static_cast<int*>(f) = number; break;
        case Type::String: *static_cast<std::string*>(f) = text; break;
    }
}

std::string toml_quote(const std::string& v) {
    std::string r = "\"";
    for (unsigned char c : v) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    r += buf;
                } else {
                    r += static_cast<char>(c);
                }
        }
    }
    return r + "\"";
}

}  // namespace

std::span<const SettingInfo> all_settings() { return kSettings; }

const SettingInfo* find_setting(std::string_view id) {
    for (const SettingInfo& i : kSettings)
        if (i.id == id) return &i;
    return nullptr;
}

std::string_view group_name(Group g) {
    switch (g) {
        case Group::Graphics: return "graphics";
        case Group::Audio: return "audio";
        case Group::System: return "system";
        case Group::Gameplay: return "gameplay";
    }
    return "";
}

std::string get_text(const Settings& s, const SettingInfo& info) {
    void* f = info.field(const_cast<Settings&>(s));
    switch (info.type) {
        case Type::Bool: return *static_cast<bool*>(f) ? "true" : "false";
        case Type::Int: return std::to_string(*static_cast<int*>(f));
        case Type::Choice: {
            const int v = *static_cast<int*>(f);
            if (info.id == "graphics.comparison_preset") return std::string(kComparisonNames[std::clamp(v, 1, 2)]);
            return v >= 0 && v < static_cast<int>(info.choices.size()) ? std::string(info.choices[static_cast<std::size_t>(v)]) : "";
        }
        case Type::String: return *static_cast<std::string*>(f);
    }
    return "";
}

bool set_text(Settings& s, const SettingInfo& info, std::string_view text, std::string& error) {
    int number = 0;
    bool flag = false;
    switch (info.type) {
        case Type::Bool: {
            const std::string l = lower(text);
            if (l == "true" || l == "1" || l == "on" || l == "yes") flag = true;
            else if (l == "false" || l == "0" || l == "off" || l == "no") flag = false;
            else {
                error = std::string(info.id) + ": expected true or false, got '" + std::string(text) + "'";
                return false;
            }
            break;
        }
        case Type::Int:
            if (!parse_int(text, number) || number < info.min || number > info.max) {
                error = std::string(info.id) + ": '" + std::string(text) + "' is not a whole number from " +
                        std::to_string(info.min) + " to " + std::to_string(info.max);
                return false;
            }
            break;
        case Type::Choice:
            number = find_choice(info, text);
            if (number < 0) {
                error = std::string(info.id) + ": '" + std::string(text) + "' is not one of " + choice_list(info);
                return false;
            }
            // The comparison preset's list leaves out "original", so its indices are offset by one.
            if (info.id == "graphics.comparison_preset") number += 1;
            break;
        case Type::String: break;
    }
    if (info.id == "graphics.preset") {
        if (number < kNamedPresets) {
            apply_bundle(s, preset(number).graphics);
            s.graphics.preset_base = number;
        }
        normalize_preset(s);
        return true;
    }
    store(s, info, number, flag, std::string(text));
    if (is_preset_member(info.id)) normalize_preset(s);
    return true;
}

std::vector<const SettingInfo*> diff(const Settings& a, const Settings& b) {
    std::vector<const SettingInfo*> r;
    for (const SettingInfo& i : kSettings)
        if (get_text(a, i) != get_text(b, i)) r.push_back(&i);
    return r;
}

// ---- TOML --------------------------------------------------------------------------------------------------------

Settings settings_from_toml(std::string_view text, LoadReport& report) {
    Settings s;
    normalize_preset(s);
    toml::table root;
    try {
        root = toml::parse(std::string(text), std::string_view("settings.toml"));
    } catch (const toml::parse_error& e) {
        std::ostringstream m;
        m << "cannot parse settings: " << e.description() << " (line " << e.source().begin.line << "); using the defaults";
        report.messages.push_back(m.str());
        return s;
    }
    auto lookup = [&](const SettingInfo& info) -> const toml::node* {
        const std::string_view id = info.id;
        const auto dot = id.find('.');
        const toml::table* t = root[id.substr(0, dot)].as_table();
        return t ? t->get(id.substr(dot + 1)) : nullptr;
    };
    // Convert a TOML node to the text form that set_text validates.
    auto apply = [&](const SettingInfo& info, const toml::node& n) {
        std::string value, error;
        bool type_ok = true;
        switch (info.type) {
            case Type::Bool:
                if (auto v = n.value<bool>()) value = *v ? "true" : "false";
                else type_ok = false;
                break;
            case Type::Int:
                if (n.is_integer()) value = std::to_string(n.as_integer()->get());
                else type_ok = false;
                break;
            case Type::Choice:
            case Type::String:
                if (n.is_string()) value = n.as_string()->get();
                else type_ok = false;
                break;
        }
        const std::string_view fallback = "using the default";
        if (!type_ok) {
            report.messages.push_back(std::string(info.id) + ": wrong type of value; " + std::string(fallback));
            return;
        }
        Settings trial = s;
        if (set_text(trial, info, value, error)) s = trial;
        else report.messages.push_back(error + "; " + std::string(fallback));
    };
    // A named preset first, so that explicit keys can override its values and make the result custom.
    const SettingInfo* preset_info = find_setting("graphics.preset");
    const SettingInfo* base_info = find_setting("graphics.preset_base");
    if (const toml::node* n = lookup(*preset_info)) apply(*preset_info, *n);
    if (const toml::node* n = lookup(*base_info)) apply(*base_info, *n);
    const int base_from_file = s.graphics.preset_base;
    for (const SettingInfo& info : kSettings) {
        if (&info == preset_info || &info == base_info) continue;
        if (const toml::node* n = lookup(info)) apply(info, *n);
    }
    // Custom keeps the base the file named; a named preset that the values still match is its own base.
    if (match_preset(s) == kPresetCustom) s.graphics.preset_base = base_from_file;
    normalize_preset(s);
    return s;
}

std::string settings_to_toml(const Settings& s) {
    std::string out =
        "# DW3Recomp settings. Missing keys use their defaults and unknown keys are ignored.\n"
        "# Command-line flags override this file for one run and are never written back to it.\n";
    Group current = Group::System;
    bool first = true;
    for (const SettingInfo& info : kSettings) {
        if (first || info.group != current) {
            out += "\n[" + std::string(group_name(info.group)) + "]\n";
            current = info.group;
            first = false;
        }
        out += "# " + std::string(info.label) + ": " + std::string(info.help) + "\n";
        if (info.type == Type::Choice) out += "#   one of: " + choice_list(info) + "\n";
        if (info.type == Type::Int) out += "#   range: " + std::to_string(info.min) + " to " + std::to_string(info.max) + "\n";
        if (info.is_restart()) out += "#   takes effect after a restart\n";
        const std::string v = get_text(s, info);
        out += std::string(info.id.substr(info.id.find('.') + 1)) + " = ";
        out += (info.type == Type::Choice || info.type == Type::String) ? toml_quote(v) : v;
        out += "\n";
    }
    return out;
}

std::filesystem::path default_settings_path() { return dw3::platform::user_data_dir() / "settings.toml"; }

Settings load_settings_file(const std::filesystem::path& path, LoadReport& report) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        Settings s;
        normalize_preset(s);
        return s;
    }
    report.file_found = true;
    std::ostringstream ss;
    ss << in.rdbuf();
    return settings_from_toml(ss.str(), report);
}

bool save_settings_file(const std::filesystem::path& path, const Settings& s, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + dw3::platform::path_to_utf8(tmp);
            return false;
        }
        const std::string text = settings_to_toml(s);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            error = "cannot write " + dw3::platform::path_to_utf8(tmp);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "cannot replace " + dw3::platform::path_to_utf8(path) + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace dw3app
