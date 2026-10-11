#pragma once

// In-game scenario tests: scripted input + timed checkpoints, loaded from TOML files in tests/scenarios/.
//
//   name = "newgame"
//   frames = 3000
//   skip_intro = true          # default; presses START at frame 300 to skip the intro FMV
//   memcard = "none"           # "none" | "blank" (fresh formatted in-memory card)
//
//   [[press]]                  # hold `button` from `at` for `hold` frames (default 4)
//   button = "START"
//   at = 900
//
//   [[repeat]]                 # press `button` every `every` frames in [from, to)
//   button = "CROSS"
//   from = 1100
//   to = 3000
//   every = 45
//
//   [[check]]                  # all fields optional except `at`
//   at = 2000
//   label = "field scene"
//   display_enabled = true
//   display_width = 320
//   display_height = 240
//   min_primitives = 1000      # cumulative GPU primitives since boot
//   min_commands = 1000        # cumulative GPU commands since boot
//   tty_contains = "CD_init"   # game debug output seen so far
//   screen_hash = "0123..."    # 256-bit perceptual hash of the displayed image (64 hex chars)
//   max_hash_distance = 24     # allowed Hamming distance (default 24)
//   ram = [{ addr = 0x800A0000, size = 4, value = 1 }]
//   screenshot = true          # save BMP of the display (for review; never committed)

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dw3::scenario {

struct Press {
    uint16_t button = 0;
    int64_t from = 0, to = 0;  // frames, [from, to)
};

struct RamCheck {
    uint32_t addr = 0;
    int size = 4;
    uint32_t value = 0;
};

struct Check {
    int64_t at = 0;
    std::string label;
    std::optional<bool> display_enabled;
    std::optional<int> display_width, display_height;
    std::optional<uint64_t> min_primitives, min_commands;
    std::optional<std::string> tty_contains;
    std::optional<std::array<uint64_t, 4>> screen_hash;
    int max_hash_distance = 24;
    std::vector<RamCheck> ram;
    bool screenshot = false;
};

struct Scenario {
    std::string name;
    std::string description;
    int64_t frames = 0;
    bool skip_intro = true;
    std::string memcard = "none";
    std::vector<Press> presses;
    std::vector<Check> checks;  // sorted by `at`
};

constexpr int64_t kIntroSkipFrame = 300;

// Parses a scenario file. Throws std::runtime_error with a readable message on errors.
Scenario load(const std::string& path);
Scenario parse(const std::string& toml_text, const std::string& source_name = "<string>");

uint16_t button_from_name(const std::string& name);  // 0 if unknown

// Buttons held at `frame` (including the automatic intro skip).
uint16_t buttons_at(const Scenario& s, int64_t frame);

// 256-bit perceptual hash: image box-downscaled to 16x16 luma, each bit = cell brighter than mean.
std::array<uint64_t, 4> perceptual_hash(const uint32_t* rgba, int width, int height);
int hash_distance(const std::array<uint64_t, 4>& a, const std::array<uint64_t, 4>& b);
std::string hash_to_hex(const std::array<uint64_t, 4>& h);
std::optional<std::array<uint64_t, 4>> hash_from_hex(const std::string& hex);

}  // namespace dw3::scenario
