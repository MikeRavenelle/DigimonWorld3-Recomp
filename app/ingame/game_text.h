// The game's own text encoding, for the strings the settings screen puts into its windows.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace dw3app::ingame {

// Encodes printable ASCII as the game's font codes (digits, letters and a few symbols; anything else becomes a
// space) and appends the terminating zero.
std::vector<uint8_t> encode_game_text(std::string_view text);

}  // namespace dw3app::ingame
