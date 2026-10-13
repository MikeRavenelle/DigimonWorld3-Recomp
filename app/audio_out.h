#pragma once

// SDL3 audio output for the SPU's 44.1 kHz stereo int16 stream. The emulation thread drains the SPU ring once per
// video frame and queues the frames on an SDL audio stream (thread-safe, SDL pulls from it on the audio thread).
// Underrun: SDL plays silence. Overrun: when too much is queued the oldest queued audio is discarded.

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "dw3/spu.h"
#include "timestretch.h"

namespace dw3app {

class AudioOutput {
public:
    static constexpr int kRate = 44100;
    static constexpr int kPrefillFrames = 1470;       // ~33 ms of silence queued at start to absorb frame jitter
    static constexpr int kMaxQueuedFrames = kRate / 4;  // ~250 ms: beyond this the oldest audio is dropped

    // Opens the playback device. Returns false (and stays silent) when audio is unavailable, muted or forced silent.
    // `forced_silent` (audit runs) keeps the device closed whatever is set later. `device` is a playback device name;
    // empty or unknown selects the default.
    bool open(bool forced_silent, bool muted = false, const std::string& device = {}, int volume_percent = 100) {
        forced_silent_ = forced_silent;
        muted_ = muted;
        device_ = device;
        volume_ = volume_percent;
        return start();
    }

    // Live changes. Muting keeps draining the SPU but plays nothing; unmuting opens the device if it was never opened.
    void set_muted(bool muted) {
        muted_ = muted;
        if (muted_) {
            if (stream_) SDL_ClearAudioStream(stream_);
        } else if (!stream_) {
            start();
        } else {
            refill();
        }
    }
    void set_volume(int percent) {
        volume_ = percent;
        if (stream_) SDL_SetAudioStreamGain(stream_, static_cast<float>(volume_) / 100.0f);
    }
    void set_device(const std::string& device) {
        if (device == device_) return;
        device_ = device;
        if (stream_) {
            stop();
            if (!muted_) start();
        }
    }
    // Game speed above 1: the SPU produces `speed` times the normal samples per second; they are time-compressed by the
    // same factor so the device plays them at their normal pitch.
    void set_speed(double speed) {
        stretch_.set_speed(speed);
        if (stream_ && !discard_) {
            SDL_ClearAudioStream(stream_);
            refill();
        }
    }
    // Fast-forward: the SPU is still drained so it never backs up, but nothing is played; going back to normal speed
    // starts from an empty queue, so no stale audio and no added latency.
    void set_discard(bool discard) {
        if (discard == discard_) return;
        discard_ = discard;
        if (stream_) {
            SDL_ClearAudioStream(stream_);
            if (!discard_) refill();
        }
    }

private:
    bool start() {
        if (forced_silent_ || muted_) return false;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_Log("audio disabled: %s", SDL_GetError());
            return false;
        }
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kRate};
        stream_ = SDL_OpenAudioDeviceStream(find_device(), &spec, nullptr, nullptr);
        if (!stream_) {
            SDL_Log("audio disabled: %s", SDL_GetError());
            return false;
        }
        SDL_SetAudioStreamGain(stream_, static_cast<float>(volume_) / 100.0f);
        refill();
        SDL_ResumeAudioStreamDevice(stream_);
        SDL_AudioSpec got{};
        int device_frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &got, &device_frames))
            SDL_Log("audio: driver %s, device %d Hz, %d channels, %d frames per buffer", SDL_GetCurrentAudioDriver(),
                    got.freq, got.channels, device_frames);
        return true;
    }

    SDL_AudioDeviceID find_device() const {
        if (device_.empty()) return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        int n = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
        SDL_AudioDeviceID found = 0;
        for (int i = 0; ids && i < n && !found; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (name && device_ == name) found = ids[i];
        }
        SDL_free(ids);
        if (!found) SDL_Log("audio: device '%s' not found, using the default", device_.c_str());
        return found ? found : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }
    void refill() {
        std::vector<int16_t> silence(static_cast<std::size_t>(kPrefillFrames) * 2, 0);
        SDL_PutAudioStreamData(stream_, silence.data(), static_cast<int>(silence.size() * 2));
    }

public:
    // Moves everything the SPU produced since the last call to the device (or discards it when closed).
    void pump(dw3::Spu& spu) {
        for (;;) {
            std::size_t n = spu.drain_samples(buf_);
            if (n == 0) break;
            if (stream_ && !muted_ && !discard_) {
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
                if (stretch_.speed() > 1.0) {
                    stretched_.clear();
                    stretch_.process(buf_.data(), n, stretched_);
                    SDL_PutAudioStreamData(stream_, stretched_.data(), static_cast<int>(stretched_.size() * 2));
                } else {
                    SDL_PutAudioStreamData(stream_, buf_.data(), static_cast<int>(n * 4));
                }
            }
            if (n < buf_.size() / 2) break;
        }
    }

    void close() { stop(); }
    uint64_t drops() const { return drops_; }
    uint64_t underruns() const { return underruns_; }

private:
    void stop() {
        if (stream_) {
            SDL_Log("audio: device buffer underruns=%llu overrun drops=%llu max queued=%d ms",
                    static_cast<unsigned long long>(underruns_), static_cast<unsigned long long>(drops_),
                    max_queued_ / 4 * 1000 / kRate);
            SDL_DestroyAudioStream(stream_);
        }
        stream_ = nullptr;
    }

    bool forced_silent_ = false, muted_ = false, discard_ = false;
    std::string device_;
    int volume_ = 100;
    SDL_AudioStream* stream_ = nullptr;
    TimeStretch stretch_;
    std::vector<int16_t> stretched_;
    std::vector<int16_t> buf_ = std::vector<int16_t>(4096);
    uint64_t drops_ = 0;
    uint64_t underruns_ = 0;
    uint64_t pumps_ = 0;
    int max_queued_ = 0;  // bytes
};

}  // namespace dw3app
