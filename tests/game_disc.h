#pragma once

// Helpers for tests that need the user's disc image. Such tests GTEST_SKIP when it is absent,
// so CI without the game still passes.

#include <filesystem>

#include <gtest/gtest.h>

namespace dw3test {

inline std::filesystem::path game_cue() {
    return std::filesystem::path(DW3_GAME_DIR) / "Digimon World 3 (USA).cue";
}

#define DW3_REQUIRE_DISC()                                                            \
    do {                                                                              \
        if (!std::filesystem::exists(::dw3test::game_cue()))                          \
            GTEST_SKIP() << "disc image not present at " << ::dw3test::game_cue();    \
    } while (0)

}  // namespace dw3test
