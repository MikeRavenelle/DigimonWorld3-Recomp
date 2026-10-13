// User-facing options of the application: one struct, a table that describes every option (so a settings menu can
// list them without knowing about any of them), and the settings.toml reader and writer.
//
// Precedence when the program starts: built-in defaults, then settings.toml, then command-line flags. Flags are
// applied to the running configuration only; the file is written from the stored configuration, which never sees them.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bindings.h"

namespace dw3app {

struct Settings {
    struct Graphics {
        int renderer = 0;            // choice: auto, gpu, vulkan, d3d12, metal, software
        int preset = 0;              // choice: original, enhanced, remaster, custom (derived from the values below)
        int preset_base = 0;         // original, enhanced or remaster: the preset a custom configuration started from
        int comparison_preset = 1;   // enhanced or remaster: what the comparison hotkey switches Original to
        int internal_res = 1;        // 1..8
        bool true_color = false;
        int texture_filter = 0;      // nearest, bilinear, xbr
        bool precise_geometry = false;
        int output_filter = 0;       // nearest, linear, fsr (scaling the picture to the window)
        int scale_mode = 0;          // fit, integer, stretch
        int widescreen = 0;          // battles: off, 16:9, 16:10, 21:9 (more of the arena on each side; fields stay 4:3)
        int display_mode = 0;        // choice: windowed, borderless, exclusive
        int monitor = 0;             // 0 is auto (where the window is), 1.. the monitors in the system's order
        int window_width = 1280;     // windowed size
        int window_height = 960;
        int fullscreen_width = 0;    // exclusive mode: 0 for the monitor's desktop mode
        int fullscreen_height = 0;
        int fullscreen_refresh = 0;  // in hundredths of a hertz; 0 for the highest the size offers
        int vsync = 1;               // choice: off, on, adaptive
        int frame_pacing = 1;        // choice: off, even (show each frame for a whole number of refreshes when possible)
        bool show_fps = false;
        int gamma = 100;             // percent, 80..140
        int brightness = 0;          // percent, -50..50
        bool hdr = false;            // HDR output on a display that is in HDR mode (GPU renderer)
        int hdr_peak_nits = 1000;    // 400..1500: the brightness white is lifted towards
        int hdr_paper_white = 203;   // 100..300 nits: where SDR white is placed
        // Post-processing (GPU renderer only; see docs/SETTINGS.md).
        int antialiasing = 0;        // off, smaa
        int sharpening = 0;          // off, low, medium, high
        int color_grade = 0;         // neutral, vivid, warm, cool
        int crt = 0;                 // off, scanlines, crt (not part of any preset)
        // Replacement background images (GPU renderer only; see docs/SETTINGS.md).
        bool hd_backgrounds = false;
        std::string texture_pack;    // name of a folder in <user data>/texture-packs; empty for none
        bool operator==(const Graphics&) const = default;
    } graphics;
    struct Audio {
        bool mute = false;
        int volume = 100;            // percent
        int music_volume = 100;      // percent of the music's level (the master volume applies on top)
        int sfx_volume = 100;        // percent of the sound effects' level
        std::string device;          // playback device name; empty selects the system default
        bool operator==(const Audio&) const = default;
    } audio;
    struct Controls {
        int pad = 0;                 // choice: auto, 1, 2, 3, 4 (which connected gamepad drives port 1)
        bool left_stick = true;      // the left stick moves like the D-pad
        int deadzone = 25;           // percent of stick travel that does nothing
        int prompts = 0;             // choice: auto, xbox, playstation, nintendo (names of gamepad buttons in hints)
        std::array<std::string, input::kActionCount> bindings = input::default_binding_texts();  // "Z|South" per action
        bool operator==(const Controls&) const = default;
    } controls;
    struct System {
        bool memcards = true;        // false: games see no card
        std::string memcard1;        // empty selects <user data dir>/memcards/slot1.mcd
        std::string memcard2;        // empty selects <user data dir>/memcards/slot2.mcd
        std::string disc_path;       // the disc image chosen in the first-run setup; empty if none yet
        int fast_forward_speed = 1;  // choice: 2x, 4x, unlimited
        bool operator==(const System&) const = default;
    } system;

    struct Gameplay {
        int encounters = 0;          // choice: normal, half, off, double
        int game_speed = 0;          // choice: 1x, 2x, 3x, 4x
        bool god_mode = false;       // the party takes no damage and defeats enemies in one hit
        int xp_multiplier = 0;       // choice: 1x, 2x, 3x, 5x, 10x (experience from won battles)
        int text_speed = 0;          // choice: normal, fast, instant (how quickly message windows print)
        bool operator==(const Gameplay&) const = default;
    } gameplay;

    bool operator==(const Settings&) const = default;
};

enum class Group { Graphics, Audio, Controls, System, Gameplay };
enum class Type { Bool, Int, Choice, String };
// Whether a changed value takes effect in the running program or only after a restart.
enum class Apply { Live, Restart };

struct SettingInfo {
    std::string_view id;           // "graphics.internal_res": the TOML table and key
    std::string_view label;
    std::string_view help;
    Group group;
    Type type;
    Apply apply;
    int min = 0, max = 0;          // Int range, inclusive
    std::span<const std::string_view> choices;  // Choice names; the stored value is the index
    bool hidden = false;           // saved, but not meant for a settings screen
    void* (*field)(Settings&) = nullptr;

    bool is_restart() const { return apply == Apply::Restart; }
};

std::span<const SettingInfo> all_settings();
const SettingInfo* find_setting(std::string_view id);
std::string_view group_name(Group g);

// The canonical text of a value: "true", "4", the choice name, or the string itself.
std::string get_text(const Settings& s, const SettingInfo& info);
// Sets a value from text (the form used on the command line and in menus). Returns false and describes the problem
// in `error` when the text is not valid for the setting; the value is then unchanged. Setting graphics.preset applies
// that preset's bundle; changing any setting a preset covers re-derives the preset (custom, if no preset matches).
bool set_text(Settings& s, const SettingInfo& info, std::string_view text, std::string& error);
// Ids whose values differ between the two configurations.
std::vector<const SettingInfo*> diff(const Settings& a, const Settings& b);

// ---- settings.toml ---------------------------------------------------------------------------------------------

struct LoadReport {
    bool file_found = false;
    std::vector<std::string> messages;  // one line per problem: parse errors, unknown values; for the log
};

// Settings from TOML text, starting at the defaults. Keys that are missing keep their default, unknown keys are
// ignored, and a value of the wrong type or outside its range is reported and left at its default. Text that does
// not parse at all yields the defaults and a message.
Settings settings_from_toml(std::string_view text, LoadReport& report);
// The full configuration as commented TOML text.
std::string settings_to_toml(const Settings& s);

std::filesystem::path default_settings_path();
// Loads the file if it exists; a missing file is not an error (defaults, report.file_found == false).
Settings load_settings_file(const std::filesystem::path& path, LoadReport& report);
// Writes through a temporary file and a rename, so a crash never leaves half a file. Creates the directory.
bool save_settings_file(const std::filesystem::path& path, const Settings& s, std::string& error);

}  // namespace dw3app
