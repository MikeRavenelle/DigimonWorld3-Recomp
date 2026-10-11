#pragma once

// Root counters 0-2 at 0x1F801100. Counter values are evaluated lazily from elapsed cycles (including
// HBLANK/VBLANK sync gating); the only scheduled work is one wake-up per counter at the predicted
// cycle of its next IRQ event.

#include <cstdint>

#include "dw3/bus.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"
#include "dw3/state.h"
#include "dw3/video_timing.h"

namespace dw3 {

class Timers {
public:
    static constexpr uint32_t kBase = 0x1F801100u;
    static constexpr int64_t kPulseCycles = 4;      // how long a pulse-mode IRQ keeps mode bit 10 low
    static constexpr int64_t kPlanHorizon = 1 << 20;  // longest single wake-up lookahead (cycles)

    // Mode register bits.
    static constexpr uint16_t kSyncEnable = 1u << 0, kResetOnTarget = 1u << 3, kIrqOnTarget = 1u << 4,
                              kIrqOnFfff = 1u << 5, kRepeat = 1u << 6, kToggle = 1u << 7, kIrqLow = 1u << 10,
                              kReachedTarget = 1u << 11, kReachedFfff = 1u << 12;

    // Installs itself on video.on_before/after_reconfigure (the app/lead must not overwrite those).
    Timers(Bus& bus, Scheduler& sched, IrqController& irq, VideoTiming& video);

    uint16_t read_value(int n);
    uint16_t read_mode(int n);  // clears the reached-target / reached-FFFF flags
    uint16_t read_target(int n);
    void write_value(int n, uint16_t v);
    void write_mode(int n, uint16_t v);  // also resets the counter to 0
    void write_target(int n, uint16_t v);

    uint32_t read(uint32_t off, int size);
    void write(uint32_t off, uint32_t value, int size);

    // Save state: every counter's registers and evaluation state plus its pending wake-up.
    void io(StateIo& s);

private:
    struct State {
        uint16_t value = 0, target = 0, mode = 0;  // bits 0-9 as written (bit 10 is derived)
        bool reached_target = false, reached_ffff = false;
        bool toggled_low = false;  // toggle mode: IRQ flag (bit 10) currently low
        bool irq_used = false;     // one-shot already consumed
        bool sync_open = false;    // sync mode 3 released by its first edge
        bool raise = false;        // IRQ line to push to the controller
        bool pulse_fired = false;
        int64_t last = 0;          // evaluated up to this cycle
        int64_t origin = 0;        // phase origin for divided clocks
        int64_t pulse_until = 0;
        uint32_t events = 0;       // IRQ events so far (used to predict wake-ups)
        EventId wake = kNoEvent;
    };

    int64_t clock_pos(const State& t, int n, int64_t cycle) const;
    int64_t ticks(const State& t, int n, int64_t a, int64_t b) const { return clock_pos(t, n, b) - clock_pos(t, n, a); }
    void advance(State& t, int n, int64_t to) const;
    static void run_ticks(State& t, int64_t n);
    static void tick(State& t);
    static void fire(State& t);

    void sync(int n);
    void sync_all();
    void consume(int n, uint32_t events_before);
    void replan(int n);
    void on_wake(int n, int64_t due);

    Scheduler& sched_;
    IrqController& irq_;
    VideoTiming& video_;
    State t_[3];
};

}  // namespace dw3
