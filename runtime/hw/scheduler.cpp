#include "dw3/scheduler.h"

namespace dw3 {

Scheduler::Scheduler(RecompContext& ctx) : ctx_(ctx) { update_next(); }

void Scheduler::update_next() { ctx_.next_event = order_.empty() ? INT64_MAX : order_.begin()->first.first; }

EventId Scheduler::schedule(int64_t at_cycle, Callback cb) {
    EventId id = next_id_++;
    Key k{at_cycle, seq_++};
    order_.emplace(k, id);
    entries_.emplace(id, Entry{k, std::move(cb)});
    update_next();
    return id;
}

bool Scheduler::cancel(EventId id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    order_.erase(it->second.key);
    entries_.erase(it);
    update_next();
    return true;
}

bool Scheduler::reschedule(EventId id, int64_t at_cycle) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    order_.erase(it->second.key);
    it->second.key = Key{at_cycle, seq_++};
    order_.emplace(it->second.key, id);
    update_next();
    return true;
}

int64_t Scheduler::time_of(EventId id) const {
    auto it = entries_.find(id);
    return it == entries_.end() ? INT64_MAX : it->second.key.first;
}

std::size_t Scheduler::run_due() {
    std::size_t n = 0;
    while (!order_.empty() && order_.begin()->first.first <= ctx_.cycles) {
        auto first = order_.begin();
        int64_t due = first->first.first;
        auto eit = entries_.find(first->second);
        Callback cb = std::move(eit->second.cb);
        order_.erase(first);
        entries_.erase(eit);
        update_next();
        cb(due);
        ++n;
    }
    update_next();
    return n;
}

void Scheduler::io(StateIo& s) {
    StateIo::Section sec(s, "SCHD");
    s.io(next_id_);
    s.io(seq_);
    if (s.loading()) {
        order_.clear();
        entries_.clear();
        update_next();
    }
}

void Scheduler::io_event(StateIo& s, EventId& id, const std::function<Callback()>& make) {
    bool pending = !s.loading() && is_pending(id);
    s.io(pending);
    if (!pending) {
        if (s.loading()) id = kNoEvent;
        return;
    }
    ++s.events;
    if (!s.loading()) {
        const Entry& e = entries_.at(id);
        EventId i = id;
        int64_t t = e.key.first;
        uint64_t q = e.key.second;
        s.io(i);
        s.io(t);
        s.io(q);
        return;
    }
    EventId i = 0;
    int64_t t = 0;
    uint64_t q = 0;
    s.io(i);
    s.io(t);
    s.io(q);
    Key k{t, q};
    order_.emplace(k, i);
    entries_.emplace(i, Entry{k, make()});
    update_next();
    id = i;
}

}  // namespace dw3
