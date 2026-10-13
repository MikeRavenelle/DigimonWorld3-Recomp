// Fast-forward: which speed applies and how to pace frames at it. Pure logic with explicit times, so it can be
// tested without a clock.
#pragma once

#include <cstdint>

namespace dw3app {

// Emulated-time multiplier for a fast_forward_speed choice (0 means no limit); the index is that of system.fast_forward_speed.
inline double fast_forward_multiplier(int choice) { return choice == 0 ? 2.0 : choice == 1 ? 4.0 : 0.0; }

// Emulated-time multiplier for a gameplay.game_speed choice (1x, 2x, 3x, 4x).
inline double game_speed_multiplier(int choice) { return choice < 0 ? 1.0 : choice > 3 ? 4.0 : choice + 1.0; }

// Fast-forward runs while the hold key or pad combo is down, or while the latch is on.
class FastForwardInput {
public:
    void set_held(bool held) { held_ = held; }
    void toggle_latch() { latched_ = !latched_; }
    bool active() const { return held_ || latched_; }
    bool latched() const { return latched_; }
    bool held() const { return held_; }

private:
    bool held_ = false, latched_ = false;
};

// Paces emulated time against a monotonic clock: the target time of a frame is the anchor time plus the emulated
// time since the anchor divided by the speed. Changing the speed re-anchors so the change neither bursts nor stalls.
class Pacer {
public:
    static constexpr double kCpuHz = 33868800.0;
    static constexpr uint64_t kMaxLagNs = 1000000000;  // after a stall longer than this the anchor is moved up

    void reset(uint64_t now_ns, int64_t cycles) {
        anchor_ns_ = now_ns;
        anchor_cycles_ = cycles;
    }
    // multiplier: 1 is real time, 0 is unlimited.
    void set_speed(double multiplier, uint64_t now_ns, int64_t cycles) {
        if (multiplier == multiplier_) return;
        multiplier_ = multiplier;
        reset(now_ns, cycles);
    }
    double speed() const { return multiplier_; }

    // How long to wait from `now_ns` before the frame that ended at `cycles` is due (0: not at all).
    uint64_t delay_ns(uint64_t now_ns, int64_t cycles) {
        if (multiplier_ == 0.0) return 0;
        const double elapsed = static_cast<double>(cycles - anchor_cycles_) * 1e9 / (kCpuHz * multiplier_);
        const uint64_t target = anchor_ns_ + static_cast<uint64_t>(elapsed);
        if (target > now_ns) return target - now_ns;
        if (now_ns - target > kMaxLagNs) reset(now_ns, cycles);
        return 0;
    }

private:
    uint64_t anchor_ns_ = 0;
    int64_t anchor_cycles_ = 0;
    double multiplier_ = 1.0;
};

}  // namespace dw3app
