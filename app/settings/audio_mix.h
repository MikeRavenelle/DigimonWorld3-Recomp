// How the game's sound driver (PsyQ libsnd) names the source of a voice, and which of those sources are music.
#pragma once

#include <cstdint>

namespace dw3app {

// libsnd keys every note on with a sequence id: the low byte is the sequence access number, the high byte the separate
// score (SEP) within it. The game opens its two sound-effect banks as accesses 0 and 1 (many short scores, one per
// effect) and plays its music from accesses 2 and 3, which the stage and the battle load alternately (the first score
// of a stage's file, or another one of it, for example the card game's). Effects started directly with SsUtKeyOn use
// the fixed id 0x21. Everything else is an effect.
constexpr uint16_t kUtilityKeyOnSequence = 0x21;
constexpr uint8_t kFirstMusicAccess = 2;

inline bool is_music_sequence(uint16_t sequence_id) {
    const uint8_t access = sequence_id & 0xFF;
    return sequence_id != kUtilityKeyOnSequence && access >= kFirstMusicAccess && access != 0xFF;
}

}  // namespace dw3app
