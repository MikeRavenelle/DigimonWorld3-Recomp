#pragma once

#include <algorithm>
#include <cstdint>

#include "dw3/bus.h"
#include "dw3/scheduler.h"

namespace dw3test {

// Bus + scheduler with a helper that advances time the way the real run loop does: stop at each
// event time, service due events, continue.
struct Clock {
    dw3::Bus bus;
    dw3::Scheduler sched{bus.ctx()};

    RecompContext& ctx() { return bus.ctx(); }
    void run_to(int64_t t) {
        auto& c = ctx();
        while (c.next_event <= t) {
            c.cycles = std::max(c.cycles, c.next_event);
            sched.run_due();
        }
        c.cycles = std::max(c.cycles, t);
        sched.run_due();
    }
    uint32_t r32(uint32_t a) { return bus.read(a, 4); }
    uint32_t r16(uint32_t a) { return bus.read(a, 2); }
    void w32(uint32_t a, uint32_t v) { bus.write(a, v, 4); }
    void w16(uint32_t a, uint32_t v) { bus.write(a, v, 2); }
    void w8(uint32_t a, uint32_t v) { bus.write(a, v, 1); }
};

}  // namespace dw3test
