#pragma once

// Fixed-capacity ring of interleaved stereo int16 frames (single thread). When full the oldest frames are
// dropped (overrun); asking for more than is queued yields fewer frames (underrun) and the caller pads with silence.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dw3/state.h"

namespace dw3 {

class AudioRing {
public:
    explicit AudioRing(std::size_t capacity_frames = 1u << 16) : buf_(capacity_frames * 2) {}

    std::size_t capacity() const { return buf_.size() / 2; }
    std::size_t size() const { return count_; }
    uint64_t overruns() const { return overruns_; }    // frames dropped because the ring was full
    uint64_t underruns() const { return underruns_; }  // frames requested by pop() that were not available

    void push(int16_t l, int16_t r) {
        if (count_ == capacity()) {
            head_ = (head_ + 1) % capacity();
            --count_;
            ++overruns_;
        }
        std::size_t tail = (head_ + count_) % capacity();
        buf_[tail * 2] = l;
        buf_[tail * 2 + 1] = r;
        ++count_;
    }

    // Pops up to out.size()/2 frames; returns the number of frames written.
    std::size_t pop(std::span<int16_t> out) {
        std::size_t want = out.size() / 2;
        std::size_t n = std::min(want, count_);
        for (std::size_t i = 0; i < n; ++i) {
            out[i * 2] = buf_[head_ * 2];
            out[i * 2 + 1] = buf_[head_ * 2 + 1];
            head_ = (head_ + 1) % capacity();
        }
        count_ -= n;
        underruns_ += want - n;
        return n;
    }

    void clear() { head_ = count_ = 0; }

    // Save state. Only the queued frames are stored, oldest first. The capacity is configuration, not state: a
    // ring smaller than the stored queue keeps its newest frames, as if it had overrun.
    void io(StateIo& s) {
        StateIo::Section sec(s, "ARNG");
        uint64_t cap = capacity(), n = count_;
        s.io(cap);
        s.io(n);
        if (n > cap) throw StateError("audio ring holds more frames than its capacity");
        if (s.loading()) {
            clear();
            for (uint64_t i = 0; i < n; ++i) {
                int16_t f[2] = {};
                s.io(f[0]);
                s.io(f[1]);
                push(f[0], f[1]);
            }
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                std::size_t at = (head_ + i) % capacity();
                s.io(buf_[at * 2]);
                s.io(buf_[at * 2 + 1]);
            }
        }
        s.io(overruns_);
        s.io(underruns_);
    }

private:
    std::vector<int16_t> buf_;
    std::size_t head_ = 0, count_ = 0;
    uint64_t overruns_ = 0, underruns_ = 0;
};

}  // namespace dw3
