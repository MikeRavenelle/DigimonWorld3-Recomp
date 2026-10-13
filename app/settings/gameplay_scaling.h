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

}  // namespace dw3app
