#include "dw3/timers.h"

#include <algorithm>

#include "dw3/hw_util.h"

namespace dw3 {

Timers::Timers(Bus& bus, Scheduler& sched, IrqController& irq, VideoTiming& video)
    : sched_(sched), irq_(irq), video_(video) {
    int64_t now = sched_.now();
    for (auto& t : t_) t.last = t.origin = now;
    bus.map_io({kBase, 0x30, [this](uint32_t off, int size) { return read(off, size); },
                [this](uint32_t off, uint32_t v, int size) { write(off, v, size); }});
    video_.on_before_reconfigure = [this] { sync_all(); };
    video_.on_after_reconfigure = [this] {
        t_[0].origin = sched_.now();  // dot clock phase restarts with the new divider
        for (int n = 0; n < 3; ++n) replan(n);
    };
}

// Monotonic position of the counter's clock source: differences are tick counts.
int64_t Timers::clock_pos(const State& t, int n, int64_t cycle) const {
    int src = (t.mode >> 8) & 3;
    switch (n) {
        case 0:
            if (src & 1) return video_.to_video_clocks(cycle - t.origin) / video_.dot_divider();
            return cycle;
        case 1:
            if (src & 1) return static_cast<int64_t>(video_.hblank_count(cycle));
            return cycle;
        default:
            if (src & 2) return (cycle - t.origin) / 8;
            return cycle;
    }
}

void Timers::fire(State& t) {
    if (!(t.mode & kRepeat) && t.irq_used) return;
    t.irq_used = true;
    ++t.events;
    if (t.mode & kToggle) {
        t.toggled_low = !t.toggled_low;
        if (t.toggled_low) t.raise = true;  // IRQ on the high->low flag transition
    } else {
        t.pulse_fired = true;
        t.raise = true;
    }
}

void Timers::tick(State& t) {
    if ((t.mode & kResetOnTarget) && t.value == t.target) t.value = 0;
    else t.value = static_cast<uint16_t>(t.value + 1);
    bool hit_t = t.value == t.target, hit_f = t.value == 0xFFFF;
    if (hit_t) t.reached_target = true;
    if (hit_f) t.reached_ffff = true;
    if ((hit_t && (t.mode & kIrqOnTarget)) || (hit_f && (t.mode & kIrqOnFfff))) fire(t);
}

void Timers::run_ticks(State& t, int64_t n) {
    while (n > 0) {
        // Ticks until the counter next lands on something interesting (target, 0xFFFF, wrap, reset).
        int64_t d = t.value == 0xFFFF ? 1 : 0xFFFF - t.value;
        if (t.value < t.target) d = std::min<int64_t>(d, t.target - t.value);
        else if (t.value == t.target && (t.mode & kResetOnTarget)) d = 1;
        if (n < d) {
            t.value = static_cast<uint16_t>(t.value + n);
            return;
        }
        t.value = static_cast<uint16_t>(t.value + (d - 1));
        n -= d - 1;
        tick(t);
        --n;
    }
}

void Timers::advance(State& t, int n, int64_t to) const {
    if (to <= t.last) return;
    bool sync = (t.mode & kSyncEnable) != 0;
    int smode = (t.mode >> 1) & 3;
    if (n == 2) {
        if (!(sync && (smode == 0 || smode == 3))) run_ticks(t, ticks(t, n, t.last, to));  // modes 0/3 stop
        t.last = to;
    } else if (!sync) {
        run_ticks(t, ticks(t, n, t.last, to));
        t.last = to;
    } else {
        const bool hb = n == 0;
        while (t.last < to) {
            int64_t a = t.last;
            int64_t edge = hb ? video_.next_hblank_edge(a) : video_.next_vblank_edge(a);
            int64_t e = std::min(edge, to);
            bool blank = hb ? video_.in_hblank(a) : video_.in_vblank(a);
            bool count = smode == 0 ? !blank : smode == 1 ? true : smode == 2 ? blank : t.sync_open;
            if (count) run_ticks(t, ticks(t, n, a, e));
            t.last = e;
            if (e == edge && !blank) {  // entered blanking
                if (smode == 1 || smode == 2) t.value = 0;
                else if (smode == 3) t.sync_open = true;
            }
        }
    }
    if (t.pulse_fired) {
        t.pulse_fired = false;
        t.pulse_until = to + kPulseCycles;
    }
}

void Timers::consume(int n, uint32_t events_before) {
    State& t = t_[n];
    if (t.raise) {
        t.raise = false;
        irq_.raise(static_cast<IrqSource>(static_cast<int>(IrqSource::Tmr0) + n));
    }
    if (t.events != events_before) replan(n);
}

void Timers::sync(int n) {
    uint32_t ev = t_[n].events;
    advance(t_[n], n, sched_.now());
    consume(n, ev);
}

void Timers::sync_all() {
    for (int n = 0; n < 3; ++n) sync(n);
}

void Timers::replan(int n) {
    State& t = t_[n];
    sched_.cancel(t.wake);
    t.wake = kNoEvent;
    if (!(t.mode & (kIrqOnTarget | kIrqOnFfff))) return;
    if (!(t.mode & kRepeat) && t.irq_used) return;
    int64_t base = t.last, end = base + kPlanHorizon;
    auto fires_by = [&](int64_t when) {
        State c = t;
        c.events = 0;
        advance(c, n, when);
        return c.events > 0;
    };
    int64_t at = end;  // no event within the horizon: just look again then
    if (fires_by(end)) {
        int64_t lo = base, hi = end;
        while (hi - lo > 1) {
            int64_t mid = lo + (hi - lo) / 2;
            (fires_by(mid) ? hi : lo) = mid;
        }
        at = hi;
    }
    t.wake = sched_.schedule(at, [this, n](int64_t due) { on_wake(n, due); });
}

void Timers::on_wake(int n, int64_t due) {
    State& t = t_[n];
    t.wake = kNoEvent;
    advance(t, n, due);
    if (t.raise) {
        t.raise = false;
        irq_.raise(static_cast<IrqSource>(static_cast<int>(IrqSource::Tmr0) + n));
    }
    replan(n);
}

uint16_t Timers::read_value(int n) {
    sync(n);
    return t_[n].value;
}

uint16_t Timers::read_target(int n) { return t_[n].target; }

uint16_t Timers::read_mode(int n) {
    sync(n);
    State& t = t_[n];
    bool low = (t.mode & kToggle) ? t.toggled_low : sched_.now() < t.pulse_until;
    uint16_t v = static_cast<uint16_t>((t.mode & 0x3FF & ~kIrqLow) | (low ? 0 : kIrqLow));
    if (t.reached_target) v |= kReachedTarget;
    if (t.reached_ffff) v |= kReachedFfff;
    t.reached_target = t.reached_ffff = false;
    return v;
}

void Timers::write_value(int n, uint16_t v) {
    sync(n);
    t_[n].value = v;
    replan(n);
}

void Timers::write_target(int n, uint16_t v) {
    sync(n);
    t_[n].target = v;
    replan(n);
}

void Timers::write_mode(int n, uint16_t v) {
    sync(n);
    State& t = t_[n];
    t.mode = static_cast<uint16_t>(v & 0x3FF & ~kIrqLow);
    t.value = 0;
    t.reached_target = t.reached_ffff = false;
    t.toggled_low = t.irq_used = t.sync_open = false;
    t.pulse_until = 0;
    t.last = t.origin = sched_.now();
    replan(n);
}

uint32_t Timers::read(uint32_t off, int size) {
    int n = static_cast<int>(off >> 4);
    uint32_t reg = 0;
    switch ((off >> 2) & 3) {
        case 0: reg = read_value(n); break;
        case 1: reg = read_mode(n); break;
        case 2: reg = read_target(n); break;
        default: break;
    }
    return lane_read(reg, size, off);
}

void Timers::write(uint32_t off, uint32_t value, int size) {
    if ((lane_mask(size, off) & 0xFFFFu) == 0) return;  // upper halfword is not backed
    int n = static_cast<int>(off >> 4);
    sync(n);  // partial-width merges need the current value
    switch ((off >> 2) & 3) {
        case 0: write_value(n, static_cast<uint16_t>(lane_merge(t_[n].value, value, size, off))); break;
        case 1: write_mode(n, static_cast<uint16_t>(lane_merge(t_[n].mode, value, size, off))); break;
        case 2: write_target(n, static_cast<uint16_t>(lane_merge(t_[n].target, value, size, off))); break;
        default: break;
    }
}

void Timers::io(StateIo& s) {
    StateIo::Section sec(s, "TMRS");
    for (int n = 0; n < 3; ++n) {
        State& t = t_[n];
        s.io(t.value);
        s.io(t.target);
        s.io(t.mode);
        s.io(t.reached_target);
        s.io(t.reached_ffff);
        s.io(t.toggled_low);
        s.io(t.irq_used);
        s.io(t.sync_open);
        s.io(t.raise);
        s.io(t.pulse_fired);
        s.io(t.last);
        s.io(t.origin);
        s.io(t.pulse_until);
        s.io(t.events);
        sched_.io_event(s, t.wake, [this, n] { return Scheduler::Callback([this, n](int64_t due) { on_wake(n, due); }); });
    }
}

}  // namespace dw3
