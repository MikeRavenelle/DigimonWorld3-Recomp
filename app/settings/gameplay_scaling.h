// Arithmetic behind the gameplay options that scale numbers the game computes. Pure functions, tested without the game.
#pragma once

#include <algorithm>
#include <cstdint>

namespace dw3app {

// Experience multiplier for a gameplay.xp_multiplier choice (1x, 2x, 3x, 5x, 10x).
inline int xp_multiplier(int choice) {
    constexpr int kMultipliers[] = {1, 2, 3, 5, 10};
    return kMultipliers[std::clamp(choice, 0, 4)];
}

// The game keeps a Digimon's experience in a field that tops out at this value.
constexpr uint32_t kMaxExperience = 999999;

// An experience award multiplied by `multiplier`, never more than the field can hold.
inline uint32_t scaled_experience(uint32_t amount, int multiplier) {
    const uint64_t scaled = static_cast<uint64_t>(amount) * static_cast<uint64_t>(std::max(multiplier, 1));
    return static_cast<uint32_t>(std::min<uint64_t>(scaled, kMaxExperience));
}

// Text speed (gameplay.text_speed): normal, fast, instant.
enum class TextSpeed { Normal, Fast, Instant };
constexpr int kFastTextChars = 3;  // characters a window prints per frame at "fast"

inline TextSpeed text_speed(int choice) { return choice == 1 ? TextSpeed::Fast : choice == 2 ? TextSpeed::Instant : TextSpeed::Normal; }

// How far a message window has printed before its own tick of this frame. `shown` is the position printed so far and
// `page_end` where the current box ends. Fast moves ahead by kFastTextChars - 1 (the window's tick adds the last one)
// without passing the end; instant prints the whole box.
inline uint32_t text_position_before_tick(TextSpeed speed, uint32_t shown, uint32_t page_end) {
    switch (speed) {
        case TextSpeed::Instant: return std::max(shown, page_end);
        case TextSpeed::Fast: {
            const uint32_t last_before_end = page_end > 0 ? page_end - 1 : 0;
            return std::max(shown, std::min(shown + (kFastTextChars - 1), last_before_end));
        }
        default: return shown;
    }
}

}  // namespace dw3app
