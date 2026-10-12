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
        SDL_AudioSpec got{};
        int device_frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &got, &device_frames))
            SDL_Log("audio: driver %s, device %d Hz, %d channels, %d frames per buffer", SDL_GetCurrentAudioDriver(),
                    got.freq, got.channels, device_frames);
        return true;
    }

    // Moves everything the SPU produced since the last call to the device (or discards it when closed).
    void pump(dw3::Spu& spu) {
        for (;;) {
            std::size_t n = spu.drain_samples(buf_);
            if (n == 0) break;
            if (stream_) {
                const int queued = SDL_GetAudioStreamQueued(stream_);
                if (queued == 0) {  // the device drained everything we gave it before this refill
                    if (++underruns_ <= 5) SDL_Log("audio: buffer underrun after %llu refills", static_cast<unsigned long long>(pumps_));
                }
                ++pumps_;
                if (queued > max_queued_) max_queued_ = queued;
                if (queued > kMaxQueuedFrames * 4) {
                    SDL_ClearAudioStream(stream_);
                    ++drops_;
                }
                SDL_PutAudioStreamData(stream_, buf_.data(), static_cast<int>(n * 4));
            }
            if (n < buf_.size() / 2) break;
        }
    }

    void close() {
        if (stream_) {
            SDL_Log("audio: device buffer underruns=%llu overrun drops=%llu max queued=%d ms",
                    static_cast<unsigned long long>(underruns_), static_cast<unsigned long long>(drops_),
                    max_queued_ / 4 * 1000 / kRate);
            SDL_DestroyAudioStream(stream_);
        }
        stream_ = nullptr;
    }
    uint64_t drops() const { return drops_; }
    uint64_t underruns() const { return underruns_; }

private:
    SDL_AudioStream* stream_ = nullptr;
    std::vector<int16_t> buf_ = std::vector<int16_t>(4096);
    uint64_t drops_ = 0;
    uint64_t underruns_ = 0;
    uint64_t pumps_ = 0;
    int max_queued_ = 0;  // bytes
};

}  // namespace dw3app
