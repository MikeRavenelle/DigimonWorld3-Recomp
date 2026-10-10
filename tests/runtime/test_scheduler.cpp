#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "dw3/bus.h"
#include "dw3/scheduler.h"

using dw3::Bus;
using dw3::EventId;
using dw3::Scheduler;

namespace {
struct Rig {
    Bus bus;
    Scheduler s{bus.ctx()};
    RecompContext& c = bus.ctx();
};
}  // namespace

TEST(Scheduler, StartsIdle) {
    Rig r;
    EXPECT_EQ(r.c.next_event, INT64_MAX);
    EXPECT_EQ(r.s.pending_count(), 0u);
    EXPECT_EQ(r.s.run_due(), 0u);
}

TEST(Scheduler, NextEventTracksEarliest) {
    Rig r;
    r.s.schedule(500, [](int64_t) {});
    EXPECT_EQ(r.c.next_event, 500);
    r.s.schedule(200, [](int64_t) {});
    EXPECT_EQ(r.c.next_event, 200);
    r.s.schedule(900, [](int64_t) {});
    EXPECT_EQ(r.c.next_event, 200);
}

TEST(Scheduler, NextEventAfterRun) {
    Rig r;
    r.s.schedule(100, [](int64_t) {});
    r.s.schedule(300, [](int64_t) {});
    r.c.cycles = 150;
    r.s.run_due();
    EXPECT_EQ(r.c.next_event, 300);
    r.c.cycles = 300;
    r.s.run_due();
    EXPECT_EQ(r.c.next_event, INT64_MAX);
}

TEST(Scheduler, RunsInTimeOrder) {
    Rig r;
    std::vector<int> order;
    r.s.schedule(300, [&](int64_t) { order.push_back(3); });
    r.s.schedule(100, [&](int64_t) { order.push_back(1); });
    r.s.schedule(200, [&](int64_t) { order.push_back(2); });
    r.c.cycles = 1000;
    EXPECT_EQ(r.s.run_due(), 3u);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(Scheduler, EqualTimesRunInInsertionOrder) {
    Rig r;
    std::vector<int> order;
    for (int i = 0; i < 5; ++i) r.s.schedule(50, [&, i](int64_t) { order.push_back(i); });
    r.c.cycles = 50;
    r.s.run_due();
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(Scheduler, OnlyDueEventsRun) {
    Rig r;
    int ran = 0;
    r.s.schedule(100, [&](int64_t) { ++ran; });
    r.s.schedule(101, [&](int64_t) { ++ran; });
    r.c.cycles = 100;
    EXPECT_EQ(r.s.run_due(), 1u);
    EXPECT_EQ(ran, 1);
    EXPECT_EQ(r.s.pending_count(), 1u);
}

TEST(Scheduler, DueBoundaryIsInclusive) {
    Rig r;
    int ran = 0;
    r.s.schedule(100, [&](int64_t) { ++ran; });
    r.c.cycles = 99;
    r.s.run_due();
    EXPECT_EQ(ran, 0);
    r.c.cycles = 100;
    r.s.run_due();
    EXPECT_EQ(ran, 1);
}

TEST(Scheduler, CallbackReceivesDueCycleNotNow) {
    Rig r;
    int64_t got = -1;
    r.s.schedule(120, [&](int64_t due) { got = due; });
    r.c.cycles = 5000;
    r.s.run_due();
    EXPECT_EQ(got, 120);
}

TEST(Scheduler, ScheduleInIsRelativeToNow) {
    Rig r;
    r.c.cycles = 1000;
    EventId id = r.s.schedule_in(250, [](int64_t) {});
    EXPECT_EQ(r.s.time_of(id), 1250);
    EXPECT_EQ(r.c.next_event, 1250);
}

TEST(Scheduler, CancelRemovesEvent) {
    Rig r;
    int ran = 0;
    EventId id = r.s.schedule(10, [&](int64_t) { ++ran; });
    EXPECT_TRUE(r.s.cancel(id));
    EXPECT_EQ(r.c.next_event, INT64_MAX);
    r.c.cycles = 100;
    r.s.run_due();
    EXPECT_EQ(ran, 0);
}

TEST(Scheduler, CancelUnknownOrTwiceFails) {
    Rig r;
    EventId id = r.s.schedule(10, [](int64_t) {});
    EXPECT_TRUE(r.s.cancel(id));
    EXPECT_FALSE(r.s.cancel(id));
    EXPECT_FALSE(r.s.cancel(dw3::kNoEvent));
    EXPECT_FALSE(r.s.cancel(9999));
}

TEST(Scheduler, CancelEarliestUpdatesNextEvent) {
    Rig r;
    EventId a = r.s.schedule(10, [](int64_t) {});
    r.s.schedule(20, [](int64_t) {});
    r.s.cancel(a);
    EXPECT_EQ(r.c.next_event, 20);
}

TEST(Scheduler, CancelAfterRunFails) {
    Rig r;
    EventId id = r.s.schedule(10, [](int64_t) {});
    r.c.cycles = 10;
    r.s.run_due();
    EXPECT_FALSE(r.s.cancel(id));
    EXPECT_FALSE(r.s.is_pending(id));
}

TEST(Scheduler, CallbackCannotCancelItself) {
    Rig r;
    EventId id = dw3::kNoEvent;
    bool result = true;
    id = r.s.schedule(10, [&](int64_t) { result = r.s.cancel(id); });
    r.c.cycles = 10;
    r.s.run_due();
    EXPECT_FALSE(result);
}

TEST(Scheduler, RescheduleMovesEvent) {
    Rig r;
    int ran = 0;
    EventId id = r.s.schedule(10, [&](int64_t) { ++ran; });
    EXPECT_TRUE(r.s.reschedule(id, 500));
    EXPECT_EQ(r.c.next_event, 500);
    r.c.cycles = 100;
    r.s.run_due();
    EXPECT_EQ(ran, 0);
    r.c.cycles = 500;
    r.s.run_due();
    EXPECT_EQ(ran, 1);
}

TEST(Scheduler, RescheduleEarlier) {
    Rig r;
    EventId id = r.s.schedule(1000, [](int64_t) {});
    r.s.reschedule(id, 5);
    EXPECT_EQ(r.c.next_event, 5);
    EXPECT_EQ(r.s.time_of(id), 5);
}

TEST(Scheduler, RescheduleKeepsId) {
    Rig r;
    EventId id = r.s.schedule(1000, [](int64_t) {});
    r.s.reschedule(id, 5);
    EXPECT_TRUE(r.s.is_pending(id));
    EXPECT_TRUE(r.s.cancel(id));
}

TEST(Scheduler, RescheduleUnknownFails) {
    Rig r;
    EXPECT_FALSE(r.s.reschedule(42, 10));
}

TEST(Scheduler, RescheduleGoesAfterEqualTimeEvents) {
    Rig r;
    std::vector<int> order;
    EventId a = r.s.schedule(10, [&](int64_t) { order.push_back(1); });
    r.s.schedule(10, [&](int64_t) { order.push_back(2); });
    r.s.reschedule(a, 10);
    r.c.cycles = 10;
    r.s.run_due();
    EXPECT_EQ(order, (std::vector<int>{2, 1}));
}

TEST(Scheduler, CallbackSchedulesFutureEventNotRunYet) {
    Rig r;
    int ran = 0;
    r.s.schedule(10, [&](int64_t) { r.s.schedule(100, [&](int64_t) { ++ran; }); });
    r.c.cycles = 50;
    EXPECT_EQ(r.s.run_due(), 1u);
    EXPECT_EQ(ran, 0);
    EXPECT_EQ(r.c.next_event, 100);
}

TEST(Scheduler, CallbackSchedulesDueEventRunsSameCall) {
    Rig r;
    std::vector<int64_t> dues;
    r.s.schedule(10, [&](int64_t d) {
        dues.push_back(d);
        r.s.schedule(20, [&](int64_t d2) { dues.push_back(d2); });
    });
    r.c.cycles = 50;
    EXPECT_EQ(r.s.run_due(), 2u);
    EXPECT_EQ(dues, (std::vector<int64_t>{10, 20}));
}

TEST(Scheduler, CallbackCancelsLaterEvent) {
    Rig r;
    int ran = 0;
    EventId later = r.s.schedule(30, [&](int64_t) { ++ran; });
    r.s.schedule(10, [&](int64_t) { r.s.cancel(later); });
    r.c.cycles = 100;
    r.s.run_due();
    EXPECT_EQ(ran, 0);
}

TEST(Scheduler, PeriodicEventDoesNotDrift) {
    Rig r;
    std::vector<int64_t> dues;
    std::function<void(int64_t)> tick = [&](int64_t due) {
        dues.push_back(due);
        r.s.schedule(due + 1000, tick);  // reschedule relative to the due time, not to now
    };
    r.s.schedule(1000, tick);
    r.c.cycles = 5350;  // serviced late
    r.s.run_due();
    EXPECT_EQ(dues, (std::vector<int64_t>{1000, 2000, 3000, 4000, 5000}));
    EXPECT_EQ(r.c.next_event, 6000);
}

TEST(Scheduler, TimeOfUnknownIsMax) {
    Rig r;
    EXPECT_EQ(r.s.time_of(77), INT64_MAX);
}

TEST(Scheduler, ManyEventsSortedExecution) {
    Rig r;
    std::vector<int64_t> got;
    for (int i = 0; i < 1000; ++i) {
        int64_t t = (i * 7919) % 1000;
        r.s.schedule(t, [&](int64_t d) { got.push_back(d); });
    }
    r.c.cycles = 2000;
    EXPECT_EQ(r.s.run_due(), 1000u);
    EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
}

TEST(Scheduler, PendingCount) {
    Rig r;
    EventId a = r.s.schedule(1, [](int64_t) {});
    r.s.schedule(2, [](int64_t) {});
    EXPECT_EQ(r.s.pending_count(), 2u);
    r.s.cancel(a);
    EXPECT_EQ(r.s.pending_count(), 1u);
}

TEST(Scheduler, IdsAreUniqueAndNonZero) {
    Rig r;
    EventId a = r.s.schedule(1, [](int64_t) {});
    EventId b = r.s.schedule(1, [](int64_t) {});
    EXPECT_NE(a, dw3::kNoEvent);
    EXPECT_NE(a, b);
}

TEST(Scheduler, PastEventRunsImmediately) {
    Rig r;
    r.c.cycles = 1000;
    int ran = 0;
    r.s.schedule(5, [&](int64_t d) { ran = static_cast<int>(d); });
    r.s.run_due();
    EXPECT_EQ(ran, 5);
}
