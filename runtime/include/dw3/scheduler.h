#pragma once

// Cycle-based event queue. Times are absolute counts of RecompContext::cycles. The scheduler keeps
// ctx.next_event equal to the earliest pending time (INT64_MAX when idle) so generated code calls
// dw3_service_events() exactly when something is due.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

#include "dw3/recomp.h"
#include "dw3/state.h"

namespace dw3 {

using EventId = uint64_t;
constexpr EventId kNoEvent = 0;

class Scheduler {
public:
    // Callbacks receive the cycle they were due at (not ctx.cycles), so periodic events never drift.
    using Callback = std::function<void(int64_t due)>;

    explicit Scheduler(RecompContext& ctx);

    RecompContext& ctx() { return ctx_; }
    int64_t now() const { return ctx_.cycles; }

    EventId schedule(int64_t at_cycle, Callback cb);
    EventId schedule_in(int64_t delta, Callback cb) { return schedule(ctx_.cycles + delta, std::move(cb)); }
    bool cancel(EventId id);                         // false if unknown / already ran
    bool reschedule(EventId id, int64_t at_cycle);   // keeps callback and id; re-queued after equal-time events
    bool is_pending(EventId id) const { return entries_.count(id) != 0; }
    int64_t time_of(EventId id) const;               // INT64_MAX if not pending
    std::size_t pending_count() const { return entries_.size(); }

    // Runs every event with time <= ctx.cycles in (time, insertion) order. Callbacks may schedule more
    // (including already-due ones, which also run) or cancel others. Returns the number executed.
    std::size_t run_due();

    // Save states. Callbacks cannot be serialized, so each device writes its own pending events: io() restores the
    // id counters (and, when loading, drops every queued event); then every owner calls io_event() for each EventId
    // it holds. `make` builds the callback for an event that is being restored. A device that schedules events it
    // does not keep an id for must stop doing so or save them in its own state; Machine checks the counts.
    void io(StateIo& s);
    void io_event(StateIo& s, EventId& id, const std::function<Callback()>& make);

private:
    using Key = std::pair<int64_t, uint64_t>;  // (time, sequence)
    struct Entry {
        Key key;
        Callback cb;
    };
    void update_next();

    RecompContext& ctx_;
    std::map<Key, EventId> order_;
    std::unordered_map<EventId, Entry> entries_;
    EventId next_id_ = 1;
    uint64_t seq_ = 0;
};

}  // namespace dw3
