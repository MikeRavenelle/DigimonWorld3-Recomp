// Pitch-preserving speed-up of a stereo 16-bit stream (waveform-similarity overlap-add). While the game runs at N
// times normal speed the SPU produces N times as many samples per second as the device plays; this shortens the
// stream by the same factor without raising its pitch, so music and effects play fast but at their normal pitch.
// Pure logic with no clock or device, so it can be tested on its own.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dw3app {

class TimeStretch {
public:
    static constexpr std::size_t kWindow = 1102;     // frames: 25 ms at 44.1 kHz
    static constexpr std::size_t kHop = kWindow / 2; // frames of output per window
    static constexpr std::size_t kSearch = 220;      // frames (5 ms) a window may move to line up with the last

    TimeStretch() {
        window_.resize(kWindow);
        for (std::size_t i = 0; i < kWindow; ++i)
            window_[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * (static_cast<float>(i) + 0.5f) / static_cast<float>(kWindow));
        reset();
    }

    // A speed of 1 passes the stream through untouched. Changing it restarts the analysis at the current position.
    void set_speed(double speed) {
        if (speed == speed_) return;
        speed_ = speed;
        reset();
    }
    double speed() const { return speed_; }

    // Interleaved stereo frames in; the stretched frames that are ready are appended to `out`.
    void process(const int16_t* stereo, std::size_t frames, std::vector<int16_t>& out) {
        if (speed_ <= 1.0) {
            out.insert(out.end(), stereo, stereo + frames * 2);
            return;
        }
        for (std::size_t i = 0; i < frames * 2; ++i) in_.push_back(static_cast<float>(stereo[i]));
        const std::size_t end = base_ + in_.size() / 2;
        while (static_cast<std::size_t>(position_) + kSearch + kWindow + 1 <= end &&
               (!have_previous_ || previous_start_ + kHop + kWindow <= end)) {
            emit_window(out);
            position_ += speed_ * static_cast<double>(kHop);
        }
        const std::size_t keep_from = have_previous_ ? std::min(previous_start_ + kHop, static_cast<std::size_t>(position_) - kSearch)
                                                     : static_cast<std::size_t>(position_) - kSearch;
        if (keep_from > base_) {
            in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>((keep_from - base_) * 2));
            base_ = keep_from;
        }
    }

private:
    void reset() {
        in_.clear();
        base_ = 0;
        position_ = static_cast<double>(kSearch);
        have_previous_ = false;
        previous_start_ = 0;
        acc_.assign(kWindow * 2, 0.0f);
    }

    float mono(std::size_t frame) const {
        const std::size_t i = (frame - base_) * 2;
        return in_[i] + in_[i + 1];
    }

    void emit_window(std::vector<int16_t>& out) {
        const std::size_t center = static_cast<std::size_t>(position_);
        std::size_t start = center;
        if (have_previous_) {
            const std::size_t target = previous_start_ + kHop;
            float best = -1e30f;
            for (std::size_t k = center - kSearch; k <= center + kSearch; ++k) {
                float dot = 0, energy = 0;
                for (std::size_t i = 0; i < kWindow; i += 4) {
                    const float c = mono(k + i);
                    dot += mono(target + i) * c;
                    energy += c * c;
                }
                const float score = dot / std::sqrt(energy + 1.0f);
                if (score > best) {
                    best = score;
                    start = k;
                }
            }
        }
        for (std::size_t i = 0; i < kWindow; ++i) {
            const std::size_t at = (start + i - base_) * 2;
            acc_[i * 2] += in_[at] * window_[i];
            acc_[i * 2 + 1] += in_[at + 1] * window_[i];
        }
        for (std::size_t i = 0; i < kHop * 2; ++i)
            out.push_back(static_cast<int16_t>(std::clamp(acc_[i], -32768.0f, 32767.0f)));
        std::copy(acc_.begin() + static_cast<std::ptrdiff_t>(kHop * 2), acc_.end(), acc_.begin());
        std::fill(acc_.end() - static_cast<std::ptrdiff_t>(kHop * 2), acc_.end(), 0.0f);
        previous_start_ = start;
        have_previous_ = true;
    }

    double speed_ = 1.0;
    std::vector<float> window_, in_, acc_;
    std::size_t base_ = 0, previous_start_ = 0;
    double position_ = 0;
    bool have_previous_ = false;
};

}  // namespace dw3app
