#pragma once

// SDL3 audio output for the SPU's 44.1 kHz stereo int16 stream. The emulation thread drains the SPU ring once per
// video frame and queues the frames on an SDL audio stream (thread-safe, SDL pulls from it on the audio thread).
// Underrun: SDL plays silence. Overrun: when too much is queued the oldest queued audio is discarded.

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "dw3/spu.h"

namespace dw3app {

class AudioOutput {
public:
    static constexpr int kRate = 44100;
    static constexpr int kPrefillFrames = 1470;       // ~33 ms of silence queued at start to absorb frame jitter
    static constexpr int kMaxQueuedFrames = kRate / 4;  // ~250 ms: beyond this the oldest audio is dropped

    // Opens the default playback device. Returns false (and stays silent) when audio is unavailable or `mute` is set.
    bool open(bool mute) {
        if (mute) return false;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_Log("audio disabled: %s", SDL_GetError());
            return false;
        }
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kRate};
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream_) {
            SDL_Log("audio disabled: %s", SDL_GetError());
            return false;
        }
        std::vector<int16_t> silence(static_cast<std::size_t>(kPrefillFrames) * 2, 0);
        SDL_PutAudioStreamData(stream_, silence.data(), static_cast<int>(silence.size() * 2));
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }

    // Moves everything the SPU produced since the last call to the device (or discards it when closed).
    void pump(dw3::Spu& spu) {
        for (;;) {
            std::size_t n = spu.drain_samples(buf_);
            if (n == 0) break;
            if (stream_) {
                if (SDL_GetAudioStreamQueued(stream_) > kMaxQueuedFrames * 4) {
                    SDL_ClearAudioStream(stream_);
                    ++drops_;
                }
                SDL_PutAudioStreamData(stream_, buf_.data(), static_cast<int>(n * 4));
            }
            if (n < buf_.size() / 2) break;
        }
    }

    void close() {
        if (stream_) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    uint64_t drops() const { return drops_; }

private:
    SDL_AudioStream* stream_ = nullptr;
    std::vector<int16_t> buf_ = std::vector<int16_t>(4096);
    uint64_t drops_ = 0;
};

}  // namespace dw3app
