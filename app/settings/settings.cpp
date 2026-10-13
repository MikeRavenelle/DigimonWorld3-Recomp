#include "settings.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "dw3/platform.h"
#include "display.h"
#include "presets.h"

namespace dw3app {

namespace {

constexpr std::string_view kRenderers[] = {"auto", "gpu", "vulkan", "d3d12", "metal", "software"};
constexpr std::string_view kPresetNames[] = {"original", "enhanced", "remaster", "custom"};
constexpr std::string_view kBaseNames[] = {"original", "enhanced", "remaster"};
constexpr std::string_view kComparisonNames[] = {"original", "enhanced", "remaster"};
constexpr std::string_view kTextureFilters[] = {"nearest", "bilinear", "xbr"};
constexpr std::string_view kOutputFilters[] = {"nearest", "linear", "fsr"};
constexpr std::string_view kAntialiasing[] = {"off", "smaa"};
constexpr std::string_view kSharpening[] = {"off", "low", "medium", "high"};
constexpr std::string_view kColorGrades[] = {"neutral", "vivid", "warm", "cool"};
constexpr std::string_view kCrt[] = {"off", "scanlines", "crt"};
constexpr std::string_view kDisplayModes[] = {"windowed", "borderless", "exclusive"};
constexpr std::string_view kVsyncModes[] = {"off", "on", "adaptive"};
constexpr std::string_view kFramePacing[] = {"off", "even"};
constexpr std::string_view kScaleModes[] = {"fit", "integer", "stretch"};
constexpr std::string_view kSpeeds[] = {"2x", "4x", "unlimited"};
constexpr std::string_view kEncounters[] = {"normal", "half", "off", "double"};
constexpr std::string_view kGameSpeeds[] = {"1x", "2x", "3x", "4x"};
constexpr std::string_view kXpMultipliers[] = {"1x", "2x", "3x", "5x", "10x"};
constexpr std::string_view kTextSpeeds[] = {"normal", "fast", "instant"};
constexpr std::string_view kPads[] = {"auto", "1", "2", "3", "4"};
constexpr std::string_view kPrompts[] = {"auto", "xbox", "playstation", "nintendo"};

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
    {"graphics.output_filter", "Output filter", "How the picture is scaled to the window: nearest, linear, or fsr (AMD FidelityFX Super Resolution 1, sharper when the window is larger than the picture; GPU renderer only).",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kOutputFilters, false, DW3_FIELD(graphics.output_filter)},
    {"graphics.scale_mode", "Scaling", "fit keeps 4:3 with borders, integer uses whole multiples only, stretch fills the window.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kScaleModes, false, DW3_FIELD(graphics.scale_mode)},
    {"graphics.display_mode", "Display mode", "windowed, borderless (a desktop-sized window without borders) or exclusive (the monitor switches to the chosen mode). F11 or Alt+Enter toggles between windowed and the last fullscreen mode.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kDisplayModes, false, DW3_FIELD(graphics.display_mode)},
    {"graphics.monitor", "Monitor", "Which monitor fullscreen uses and a window opens on: 0 is auto (where the window is), 1 and up follow the system's order.",
     Group::Graphics, Type::Int, Apply::Live, 0, 8, {}, false, DW3_FIELD(graphics.monitor)},
    {"graphics.window_width", "Window width", "Window width in pixels (the in-game screen picks from a list of common sizes).",
     Group::Graphics, Type::Int, Apply::Live, 160, 16384, {}, true, DW3_FIELD(graphics.window_width)},
    {"graphics.window_height", "Window height", "Window height in pixels.",
     Group::Graphics, Type::Int, Apply::Live, 120, 16384, {}, true, DW3_FIELD(graphics.window_height)},
    {"graphics.fullscreen_width", "Exclusive width", "Width of the exclusive fullscreen mode; 0 uses the monitor's desktop mode.",
     Group::Graphics, Type::Int, Apply::Live, 0, 16384, {}, true, DW3_FIELD(graphics.fullscreen_width)},
    {"graphics.fullscreen_height", "Exclusive height", "Height of the exclusive fullscreen mode; 0 uses the monitor's desktop mode.",
     Group::Graphics, Type::Int, Apply::Live, 0, 16384, {}, true, DW3_FIELD(graphics.fullscreen_height)},
    {"graphics.fullscreen_refresh", "Exclusive refresh rate", "Refresh rate of the exclusive fullscreen mode in hundredths of a hertz (14400 is 144 Hz); 0 picks the highest the size offers.",
     Group::Graphics, Type::Int, Apply::Live, 0, 100000, {}, true, DW3_FIELD(graphics.fullscreen_refresh)},
    {"graphics.vsync", "Vertical sync", "Wait for the display refresh when presenting. adaptive tears instead of stalling when a frame is late, where the graphics path supports it (otherwise it acts as on).",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kVsyncModes, false, DW3_FIELD(graphics.vsync)},
    {"graphics.frame_pacing", "Frame pacing", "even shows every emulated frame for the same whole number of refreshes on displays that run at a multiple of 60 Hz (120, 180, 240), which avoids judder; it needs vertical sync and does nothing on 60 Hz or 144 Hz displays.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kFramePacing, false, DW3_FIELD(graphics.frame_pacing)},
    {"graphics.show_fps", "Show FPS", "A small frame rate counter in the corner of the window.",
     Group::Graphics, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(graphics.show_fps)},
    {"graphics.gamma", "Gamma", "Gamma of the finished picture in percent: 100 changes nothing, above brightens the midtones (GPU renderer only). Not part of any preset.",
     Group::Graphics, Type::Int, Apply::Live, 80, 140, {}, false, DW3_FIELD(graphics.gamma)},
    {"graphics.brightness", "Brightness", "Brightness of the finished picture in percent: 0 changes nothing (GPU renderer only). Not part of any preset.",
     Group::Graphics, Type::Int, Apply::Live, -50, 50, {}, false, DW3_FIELD(graphics.brightness)},

    {"graphics.antialiasing", "Anti-aliasing", "SMAA edge smoothing of the finished picture (GPU renderer only). Mostly useful at internal resolution 1x to 2x.",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kAntialiasing, false, DW3_FIELD(graphics.antialiasing)},
    {"graphics.sharpening", "Sharpening", "Contrast adaptive sharpening (AMD FidelityFX CAS) of the finished picture; with the fsr output filter it sets the strength of FSR's own sharpening (GPU renderer only).",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kSharpening, false, DW3_FIELD(graphics.sharpening)},
    {"graphics.color_grade", "Colour grading", "A colour look applied to the finished picture: neutral changes nothing (GPU renderer only).",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kColorGrades, false, DW3_FIELD(graphics.color_grade)},
    {"graphics.crt", "CRT look", "Scanlines, or scanlines with a phosphor mask, slight curvature and vignette. Not part of any preset (GPU renderer only).",
     Group::Graphics, Type::Choice, Apply::Live, 0, 0, kCrt, false, DW3_FIELD(graphics.crt)},

    {"graphics.hd_backgrounds", "HD BG", "Replace the pre-rendered field backgrounds with the images of the selected texture pack (GPU renderer only; a pack is made from your own disc, see docs/SETTINGS.md).",
     Group::Graphics, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(graphics.hd_backgrounds)},
    {"graphics.texture_pack", "Texture pack", "Name of the folder under <user data>/texture-packs that holds the replacement images; empty for none.",
     Group::Graphics, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(graphics.texture_pack)},

    {"audio.mute", "Mute", "No sound output.",
     Group::Audio, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(audio.mute)},
    {"audio.volume", "Volume", "Output volume in percent.",
     Group::Audio, Type::Int, Apply::Live, 0, 100, {}, false, DW3_FIELD(audio.volume)},
    {"audio.music_volume", "Music volume", "Level of the music in percent, below the master volume. Movie sound counts as music.",
     Group::Audio, Type::Int, Apply::Live, 0, 100, {}, false, DW3_FIELD(audio.music_volume)},
    {"audio.sfx_volume", "Effects volume", "Level of the sound effects in percent, below the master volume.",
     Group::Audio, Type::Int, Apply::Live, 0, 100, {}, false, DW3_FIELD(audio.sfx_volume)},
    {"audio.device", "Output device", "Name of the playback device; empty uses the system default.",
     Group::Audio, Type::String, Apply::Live, 0, 0, {}, false, DW3_FIELD(audio.device)},

    {"controls.pad", "Gamepad", "Which connected gamepad drives port 1: auto uses the first one and keeps it until it is unplugged; 1 to 4 pick by position and fall back to auto when that pad is not connected.",
     Group::Controls, Type::Choice, Apply::Live, 0, 0, kPads, false, DW3_FIELD(controls.pad)},
    {"controls.left_stick", "Left stick", "The left stick moves like the D-pad.",
     Group::Controls, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(controls.left_stick)},
    {"controls.deadzone", "Stick deadzone", "How far the left stick must lean, in percent of its travel, before it moves.",
     Group::Controls, Type::Int, Apply::Live, 10, 50, {}, false, DW3_FIELD(controls.deadzone)},
    {"controls.prompts", "Button prompts", "Names of gamepad buttons in on-screen hints: auto follows the connected pad.",
     Group::Controls, Type::Choice, Apply::Live, 0, 0, kPrompts, false, DW3_FIELD(controls.prompts)},
    {"controls.up", "Up binding", "Keyboard key and gamepad button for Up, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[0])},
    {"controls.down", "Down binding", "Keyboard key and gamepad button for Down, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[1])},
    {"controls.left", "Left binding", "Keyboard key and gamepad button for Left, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[2])},
    {"controls.right", "Right binding", "Keyboard key and gamepad button for Right, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[3])},
    {"controls.cross", "Cross binding", "Keyboard key and gamepad button for Cross, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[4])},
    {"controls.circle", "Circle binding", "Keyboard key and gamepad button for Circle, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[5])},
    {"controls.square", "Square binding", "Keyboard key and gamepad button for Square, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[6])},
    {"controls.triangle", "Triangle binding", "Keyboard key and gamepad button for Triangle, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[7])},
    {"controls.l1", "L1 binding", "Keyboard key and gamepad button for L1, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[8])},
    {"controls.r1", "R1 binding", "Keyboard key and gamepad button for R1, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[9])},
    {"controls.l2", "L2 binding", "Keyboard key and gamepad button for L2, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[10])},
    {"controls.r2", "R2 binding", "Keyboard key and gamepad button for R2, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[11])},
    {"controls.l3", "L3 binding", "Keyboard key and gamepad button for L3, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[12])},
    {"controls.r3", "R3 binding", "Keyboard key and gamepad button for R3, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[13])},
    {"controls.start", "Start binding", "Keyboard key and gamepad button for Start, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[14])},
    {"controls.select", "Select binding", "Keyboard key and gamepad button for Select, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[15])},
    {"controls.fast_forward", "Fast-forward (hold) binding", "Keyboard key and gamepad button for Fast-forward (hold), written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[16])},
    {"controls.fast_forward_latch", "Fast-forward (latch) binding", "Keyboard key and gamepad button for Fast-forward (latch), written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[17])},
    {"controls.game_speed", "Game speed cycle binding", "Keyboard key and gamepad button for Game speed cycle, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[18])},
    {"controls.compare", "Original/preset comparison binding", "Keyboard key and gamepad button for Original/preset comparison, written as key|button (for example Z|South). Reserved keys cannot be moved.",
     Group::Controls, Type::String, Apply::Live, 0, 0, {}, true, DW3_FIELD(controls.bindings[19])},

    {"system.memcards", "Memory cards", "Insert memory cards; off means games see no card.",
     Group::System, Type::Bool, Apply::Restart, 0, 0, {}, false, DW3_FIELD(system.memcards)},
    {"system.memcard1", "Memory card 1", "Path of the slot 1 card image; empty uses <user data>/memcards/slot1.mcd.",
     Group::System, Type::String, Apply::Restart, 0, 0, {}, false, DW3_FIELD(system.memcard1)},
    {"system.memcard2", "Memory card 2", "Path of the slot 2 card image; empty uses <user data>/memcards/slot2.mcd (inserted only if it exists).",
     Group::System, Type::String, Apply::Restart, 0, 0, {}, false, DW3_FIELD(system.memcard2)},
    {"system.disc_path", "Disc image", "Path of the Digimon World 3 (USA) disc image (.cue); set by the first-run setup.",
     Group::System, Type::String, Apply::Restart, 0, 0, {}, true, DW3_FIELD(system.disc_path)},
    {"system.fast_forward_speed", "Fast-forward speed", "Speed while fast-forwarding (hold Tab, or press F10 to latch it).",
     Group::System, Type::Choice, Apply::Live, 0, 0, kSpeeds, false, DW3_FIELD(system.fast_forward_speed)},

    {"gameplay.encounters", "Random encounters", "How often random battles start while walking: normal, half as often, never, or twice as often.",
     Group::Gameplay, Type::Choice, Apply::Live, 0, 0, kEncounters, false, DW3_FIELD(gameplay.encounters)},
    {"gameplay.game_speed", "Game speed", "Runs the whole game faster, with sound kept at its normal pitch (a hotkey toggles it).",
     Group::Gameplay, Type::Choice, Apply::Live, 0, 0, kGameSpeeds, false, DW3_FIELD(gameplay.game_speed)},
    {"gameplay.god_mode", "God mode", "The party takes no damage and its attacks defeat enemies in one hit.",
     Group::Gameplay, Type::Bool, Apply::Live, 0, 0, {}, false, DW3_FIELD(gameplay.god_mode)},
    {"gameplay.xp_multiplier", "XP multiplier", "Multiplies the experience each Digimon gets from a won battle.",
     Group::Gameplay, Type::Choice, Apply::Live, 0, 0, kXpMultipliers, false, DW3_FIELD(gameplay.xp_multiplier)},
    {"gameplay.text_speed", "Text speed", "How fast message boxes print their text: normal, fast, or the whole box at once (a button still closes it).",
     Group::Gameplay, Type::Choice, Apply::Live, 0, 0, kTextSpeeds, false, DW3_FIELD(gameplay.text_speed)},
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
        case Group::Controls: return "controls";
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
        case Type::String:
            if (info.group == Group::Controls && !input::parse_binding(text)) {
                error = std::string(info.id) + ": '" + std::string(text) + "' is not a binding (key|button, for example Z|South)";
                return false;
            }
            break;
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
                else if (auto legacy = n.value<bool>(); legacy && info.id == "graphics.vsync") value = *legacy ? "on" : "off";  // was a switch
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
    // Files from before the display mode existed switch fullscreen on and off.
    const SettingInfo* mode_info = find_setting("graphics.display_mode");
    if (!lookup(*mode_info)) {
        const toml::table* graphics = root["graphics"].as_table();
        if (graphics && graphics->get_as<bool>("fullscreen") && graphics->get_as<bool>("fullscreen")->get())
            s.graphics.display_mode = static_cast<int>(DisplayMode::Borderless);
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
