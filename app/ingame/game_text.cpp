#include "game_text.h"

namespace dw3app::ingame {
namespace {

// Symbols live in the two-byte range that starts with 1.
uint8_t symbol_code(char c) {
    switch (c) {
        case '.': return 0x05;
        case '/': return 0x0F;
        case ':': return 0x07;
        case '\'': return 0x13;
        case '-': return 0x1A;
        default: return 0x01;  // space
    }
}

}  // namespace

std::vector<uint8_t> encode_game_text(std::string_view text) {
    std::vector<uint8_t> out;
    out.reserve(text.size() * 2 + 1);
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            out.push_back(static_cast<uint8_t>(4 + (c - '0')));
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<uint8_t>(0x0E + (c - 'A')));
        } else if (c >= 'a' && c <= 'z') {
            out.push_back(static_cast<uint8_t>(0x28 + (c - 'a')));
        } else {
            out.push_back(1);
            out.push_back(symbol_code(c));
        }
    }
    out.push_back(0);
    return out;
}

}  // namespace dw3app::ingame
