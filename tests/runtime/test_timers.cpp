#include <gtest/gtest.h>

#include <vector>

#include "dw3/timers.h"
#include "hw_test_util.h"

using dw3::IrqController;
using dw3::Timers;
using dw3::VideoTiming;

namespace {
constexpr uint32_t kBase = 0x1F801100u;
constexpr uint32_t kStat = 0x1F801070u;

struct Rig : dw3test::Clock {
    IrqController irq{bus};
    VideoTiming video{sched, irq};
    Timers tm{bus, sched, irq, video};

    static uint32_t reg(int n, int r) { return kBase + n * 16 + r * 4; }
    void mode(int n, uint32_t m) { w32(reg(n, 1), m); }
    void target(int n, uint32_t t) { w32(reg(n, 2), t); }
    void value(int n, uint32_t v) { w32(reg(n, 0), v); }
    uint32_t val(int n) { return r32(reg(n, 0)); }
    uint32_t rmode(int n) { return r32(reg(n, 1)); }
    void at(int64_t c) { ctx().cycles = c; }
    bool stat_bit(int n) { return (irq.stat() >> (4 + n)) & 1; }
};

int64_t ceil_cycle(int64_t vc) { return (vc * 451584 + 715909 - 1) / 715909; }  // exact NTSC ratio
int64_t vclk(int64_t cycles) { return cycles * 715909 / 451584; }
}  // namespace

// ---- basic counting, all three timers ----

class TimerN : public ::testing::TestWithParam<int> {};

TEST_P(TimerN, ResetValues) {
    Rig r;
    EXPECT_EQ(r.val(GetParam()), 0u);
    EXPECT_EQ(r.r32(Rig::reg(GetParam(), 2)), 0u);
}

TEST_P(TimerN, SysclockCountsCycles) {
    Rig r;
    int n = GetParam();
    r.mode(n, 0);
    for (int64_t c : {1, 2, 7, 100, 1000, 65535, 65536, 65537, 70000, 1000000}) {
        r.at(c);
        EXPECT_EQ(r.val(n), static_cast<uint32_t>(c & 0xFFFF)) << c;
    }
}

TEST_P(TimerN, ValueReadIs16BitZeroExtended) {
    Rig r;
    int n = GetParam();
    r.value(n, 0xFFFF);
    EXPECT_EQ(r.val(n), 0xFFFFu);
    EXPECT_EQ(r.r16(Rig::reg(n, 0) + 2), 0u);
}

TEST_P(TimerN, ValueWriteThenCount) {
    Rig r;
    int n = GetParam();
    r.at(100);
    r.value(n, 1000);
    r.at(150);
    EXPECT_EQ(r.val(n), 1050u);
}

TEST_P(TimerN, ValueWrite32BitIgnoresUpperHalf) {
    Rig r;
    int n = GetParam();
    r.value(n, 0xABCD1234u);
    EXPECT_EQ(r.val(n), 0x1234u);
}

TEST_P(TimerN, ModeWriteResetsCounter) {
    Rig r;
    int n = GetParam();
    r.at(500);
    EXPECT_EQ(r.val(n), 500u);
    r.mode(n, 0);
    EXPECT_EQ(r.val(n), 0u);
    r.at(600);
    EXPECT_EQ(r.val(n), 100u);
}

TEST_P(TimerN, ModeReadsBackWrittenBitsWithIrqFlagSet) {
    Rig r;
    int n = GetParam();
    r.mode(n, 0x0158);
    EXPECT_EQ(r.rmode(n), 0x0158u | 0x400u);
}

TEST_P(TimerN, ModeWriteMasksTo10Bits) {
    Rig r;
    int n = GetParam();
    r.mode(n, 0xFFFFFFFFu);
    EXPECT_EQ(r.rmode(n), 0x7FFu);
}

TEST_P(TimerN, TargetRoundTrip) {
    Rig r;
    int n = GetParam();
    r.target(n, 0x1234);
    EXPECT_EQ(r.r32(Rig::reg(n, 2)), 0x1234u);
    r.target(n, 0xFFFFFFFFu);
    EXPECT_EQ(r.r32(Rig::reg(n, 2)), 0xFFFFu);
}

TEST_P(TimerN, TargetDoesNotResetCounter) {
    Rig r;
    int n = GetParam();
    r.at(300);
    r.target(n, 5000);
    EXPECT_EQ(r.val(n), 300u);
}

TEST_P(TimerN, ResetOnTargetPeriodIsTargetPlusOne) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x08);
    for (int64_t c = 1; c < 1000; c += 13) {
        r.at(c);
        EXPECT_EQ(r.val(n), static_cast<uint32_t>(c % 101)) << c;
    }
}

TEST_P(TimerN, ResetOnTargetVisibleAtTarget) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x08);
    r.at(100);
    EXPECT_EQ(r.val(n), 100u);
    r.at(101);
    EXPECT_EQ(r.val(n), 0u);
}

TEST_P(TimerN, NoResetCountsPastTarget) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0);
    r.at(150);
    EXPECT_EQ(r.val(n), 150u);
}

TEST_P(TimerN, OverflowWrapsToZero) {
    Rig r;
    int n = GetParam();
    r.mode(n, 0);
    r.at(65535);
    EXPECT_EQ(r.val(n), 0xFFFFu);
    r.at(65536);
    EXPECT_EQ(r.val(n), 0u);
}

TEST_P(TimerN, TargetZeroWithResetStaysZero) {
    Rig r;
    int n = GetParam();
    r.target(n, 0);
    r.mode(n, 0x08);
    r.at(37);
    EXPECT_EQ(r.val(n), 0u);
    EXPECT_NE(r.rmode(n) & 0x800u, 0u);
}

TEST_P(TimerN, ResetOnTargetFlagSetAtTarget) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x08);
    r.at(99);
    EXPECT_EQ(r.rmode(n) & 0x800u, 0u);
    r.at(100);
    EXPECT_NE(r.rmode(n) & 0x800u, 0u);
}

TEST_P(TimerN, ReachedTargetFlagClearsOnRead) {
    Rig r;
    int n = GetParam();
    r.target(n, 10);
    r.mode(n, 0);
    r.at(50);
    EXPECT_NE(r.rmode(n) & 0x800u, 0u);
    EXPECT_EQ(r.rmode(n) & 0x800u, 0u);
}

TEST_P(TimerN, ReachedFfffFlagSetAndClearsOnRead) {
    Rig r;
    int n = GetParam();
    r.mode(n, 0);
    r.at(65534);
    EXPECT_EQ(r.rmode(n) & 0x1000u, 0u);
    r.at(65535);
    EXPECT_NE(r.rmode(n) & 0x1000u, 0u);
    EXPECT_EQ(r.rmode(n) & 0x1000u, 0u);
}

TEST_P(TimerN, FlagsIndependent) {
    Rig r;
    int n = GetParam();
    r.target(n, 10);
    r.mode(n, 0);
    r.at(70000);
    uint32_t m = r.rmode(n);
    EXPECT_NE(m & 0x800u, 0u);
    EXPECT_NE(m & 0x1000u, 0u);
    m = r.rmode(n);
    EXPECT_EQ(m & 0x1800u, 0u);
}

TEST_P(TimerN, FlagsClearedByModeWrite) {
    Rig r;
    int n = GetParam();
    r.target(n, 10);
    r.mode(n, 0);
    r.at(50);
    r.mode(n, 0);
    EXPECT_EQ(r.rmode(n) & 0x1800u, 0u);
}

TEST_P(TimerN, FlagsSetWithoutIrqEnable) {
    Rig r;
    int n = GetParam();
    r.target(n, 10);
    r.mode(n, 0);
    r.at(50);
    EXPECT_FALSE(r.stat_bit(n));
    EXPECT_NE(r.rmode(n) & 0x800u, 0u);
}

TEST_P(TimerN, IrqOnTargetFiresAtExactCycle) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    r.run_to(99);
    EXPECT_FALSE(r.stat_bit(n));
    r.run_to(100);
    EXPECT_TRUE(r.stat_bit(n));
    EXPECT_EQ(r.irq.stat() & ~(1u << (4 + n)) & 0x70u, 0u);  // only its own source
}

TEST_P(TimerN, IrqOnFfffFiresAtExactCycle) {
    Rig r;
    int n = GetParam();
    r.mode(n, 0x20);
    r.run_to(65534);
    EXPECT_FALSE(r.stat_bit(n));
    r.run_to(65535);
    EXPECT_TRUE(r.stat_bit(n));
}

TEST_P(TimerN, NoIrqWithoutEnableBits) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x48);
    r.run_to(200000);
    EXPECT_FALSE(r.stat_bit(n));
}

TEST_P(TimerN, OneShotFiresOnlyOnce) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);  // bit 6 clear: one-shot
    r.run_to(100);
    EXPECT_TRUE(r.stat_bit(n));
    r.w32(kStat, 0);
    r.run_to(1000);
    EXPECT_FALSE(r.stat_bit(n));
}

TEST_P(TimerN, RepeatFiresEveryPeriod) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x58);
    for (int i = 0; i < 5; ++i) {
        int64_t fire = 100 + 101 * i;
        r.run_to(fire - 1);
        EXPECT_FALSE(r.stat_bit(n)) << i;
        r.run_to(fire);
        EXPECT_TRUE(r.stat_bit(n)) << i;
        r.w32(kStat, 0);
    }
}

TEST_P(TimerN, RepeatWithoutResetFiresOncePerWrap) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x50);
    r.run_to(100);
    EXPECT_TRUE(r.stat_bit(n));
    r.w32(kStat, 0);
    r.run_to(65535 + 100);
    EXPECT_FALSE(r.stat_bit(n));
    r.run_to(65536 + 100);
    EXPECT_TRUE(r.stat_bit(n));
}

TEST_P(TimerN, ModeWriteRearmsOneShot) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    r.run_to(100);
    r.w32(kStat, 0);
    r.run_to(500);
    r.mode(n, 0x18);
    r.run_to(500 + 99);
    EXPECT_FALSE(r.stat_bit(n));
    r.run_to(500 + 100);
    EXPECT_TRUE(r.stat_bit(n));
}

TEST_P(TimerN, ModeWriteCancelsPendingIrq) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    r.run_to(50);
    r.mode(n, 0);
    r.run_to(500);
    EXPECT_FALSE(r.stat_bit(n));
}

TEST_P(TimerN, TargetChangeMovesIrq) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    r.run_to(50);
    r.target(n, 200);
    r.run_to(100);
    EXPECT_FALSE(r.stat_bit(n));
    r.run_to(199);
    EXPECT_FALSE(r.stat_bit(n));
    r.run_to(200);
    EXPECT_TRUE(r.stat_bit(n));
}

TEST_P(TimerN, ValueWriteMovesIrq) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    r.run_to(50);
    r.value(n, 90);
    r.run_to(59);
    EXPECT_FALSE(r.stat_bit(n));
    r.run_to(60);
    EXPECT_TRUE(r.stat_bit(n));
}

TEST_P(TimerN, IrqRaisedLazilyOnRead) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    r.at(150);  // scheduler not serviced
    EXPECT_EQ(r.val(n), 49u);
    EXPECT_TRUE(r.stat_bit(n));
}

TEST_P(TimerN, NextEventReflectsWake) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    EXPECT_EQ(r.ctx().next_event, 100);
}

TEST_P(TimerN, OneShotLeavesNoWakeAfterFiring) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    r.run_to(100);
    EXPECT_EQ(r.sched.pending_count(), 1u);  // just the VBLANK event
}

TEST_P(TimerN, PulseModeBit10GoesLowBriefly) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x18);
    r.run_to(100);
    EXPECT_EQ(r.rmode(n) & 0x400u, 0u);
    r.run_to(100 + Timers::kPulseCycles + 1);
    EXPECT_NE(r.rmode(n) & 0x400u, 0u);
}

TEST_P(TimerN, ToggleModeAlternatesIrqAndBit10) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0xD8);  // toggle + repeat + reset + irq on target
    r.run_to(100);
    EXPECT_TRUE(r.stat_bit(n));
    EXPECT_EQ(r.rmode(n) & 0x400u, 0u);
    r.w32(kStat, 0);
    r.run_to(201);
    EXPECT_FALSE(r.stat_bit(n));
    EXPECT_NE(r.rmode(n) & 0x400u, 0u);
    r.run_to(302);
    EXPECT_TRUE(r.stat_bit(n));
    EXPECT_EQ(r.rmode(n) & 0x400u, 0u);
}

TEST_P(TimerN, ToggleBit10StaysAfterModeReadWhileLow) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0xD8);
    r.run_to(150);
    EXPECT_EQ(r.rmode(n) & 0x400u, 0u);
    EXPECT_EQ(r.rmode(n) & 0x400u, 0u);
}

TEST_P(TimerN, ToggleOneShotHasOneEvent) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x98);
    r.run_to(100);
    EXPECT_TRUE(r.stat_bit(n));
    r.w32(kStat, 0);
    r.run_to(5000);
    EXPECT_FALSE(r.stat_bit(n));
    EXPECT_EQ(r.rmode(n) & 0x400u, 0u);  // stays low
}

TEST_P(TimerN, ModeWriteResetsBit10AfterToggle) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0xD8);
    r.run_to(150);
    r.mode(n, 0xD8);
    EXPECT_NE(r.rmode(n) & 0x400u, 0u);
}

TEST_P(TimerN, BothIrqBitsOneEventWhenSimultaneous) {
    Rig r;
    int n = GetParam();
    r.target(n, 0xFFFF);
    r.mode(n, 0x78);  // target == FFFF: both conditions on the same tick; repeat, reset
    r.run_to(0xFFFF);
    EXPECT_TRUE(r.stat_bit(n));
    r.w32(kStat, 0);
    r.run_to(0xFFFF + 1);  // reset-to-0, nothing
    EXPECT_FALSE(r.stat_bit(n));
}

TEST_P(TimerN, IrqFfffAfterTargetRepeat) {
    Rig r;
    int n = GetParam();
    r.target(n, 100);
    r.mode(n, 0x70);
    r.run_to(100);
    EXPECT_TRUE(r.stat_bit(n));
    r.w32(kStat, 0);
    r.run_to(65534);
    EXPECT_FALSE(r.stat_bit(n));
    r.run_to(65535);
    EXPECT_TRUE(r.stat_bit(n));
}

TEST_P(TimerN, WideIdleGapKeepsCountExact) {
    Rig r;
    int n = GetParam();
    r.mode(n, 0);
    r.at(1000000000LL);
    EXPECT_EQ(r.val(n), static_cast<uint32_t>(1000000000LL & 0xFFFF));
}

INSTANTIATE_TEST_SUITE_P(AllTimers, TimerN, ::testing::Values(0, 1, 2));

// ---- register access widths ----

TEST(TimerRegs, Mode16BitWriteResetsCounter) {
    Rig r;
    r.at(500);
    r.w16(Rig::reg(0, 1), 0x0008);
    EXPECT_EQ(r.val(0), 0u);
    EXPECT_EQ(r.rmode(0) & 0xFFu, 0x08u);
}

TEST(TimerRegs, WriteToUpperHalfwordIgnored) {
    Rig r;
    r.at(500);
    r.w16(Rig::reg(0, 1) + 2, 0x0008);
    EXPECT_EQ(r.val(0), 500u);  // no mode write happened
    r.w16(Rig::reg(0, 0) + 2, 0x1234);
    EXPECT_EQ(r.val(0), 500u);
    r.w16(Rig::reg(0, 2) + 2, 0x1234);
    EXPECT_EQ(r.r32(Rig::reg(0, 2)), 0u);
}

TEST(TimerRegs, Target8BitByteLanes) {
    Rig r;
    r.w8(Rig::reg(1, 2), 0x34);
    r.w8(Rig::reg(1, 2) + 1, 0x12);
    EXPECT_EQ(r.r32(Rig::reg(1, 2)), 0x1234u);
}

TEST(TimerRegs, Value8BitHighByte) {
    Rig r;
    r.value(2, 0x00FF);
    r.w8(Rig::reg(2, 0) + 1, 0x12);
    EXPECT_EQ(r.val(2), 0x12FFu);
}

TEST(TimerRegs, ReadWidths) {
    Rig r;
    r.value(0, 0xBEEF);
    EXPECT_EQ(r.r16(Rig::reg(0, 0)), 0xBEEFu);
    EXPECT_EQ(r.bus.read(Rig::reg(0, 0), 1), 0xEFu);
    EXPECT_EQ(r.bus.read(Rig::reg(0, 0) + 1, 1), 0xBEu);
}

TEST(TimerRegs, UnusedWordReadsZero) {
    Rig r;
    EXPECT_EQ(r.r32(Rig::reg(0, 3)), 0u);
    r.w32(Rig::reg(0, 3), 0xFFFFFFFFu);
    EXPECT_EQ(r.r32(Rig::reg(0, 3)), 0u);
}

TEST(TimerRegs, TimersAreIndependent) {
    Rig r;
    r.value(0, 10);
    r.value(1, 20);
    r.value(2, 30);
    r.target(1, 99);
    EXPECT_EQ(r.val(0), 10u);
    EXPECT_EQ(r.val(1), 20u);
    EXPECT_EQ(r.val(2), 30u);
    EXPECT_EQ(r.r32(Rig::reg(0, 2)), 0u);
}

TEST(TimerRegs, MappedThroughKseg1) {
    Rig r;
    dw3_write32(&r.ctx(), 0xBF801108u, 0x4321);
    EXPECT_EQ(dw3_read32(&r.ctx(), 0xBF801108u), 0x4321u);
    EXPECT_EQ(dw3_read16(&r.ctx(), 0x9F801108u), 0x4321u);
}

TEST(TimerRegs, Mode8BitWriteKeepsOtherByte) {
    Rig r;
    r.mode(0, 0x0258);
    r.w8(Rig::reg(0, 1), 0x08);  // low byte only; high byte (clock source bits) kept
    EXPECT_EQ(r.rmode(0), 0x0208u | 0x400u);
}

// ---- clock sources ----

class DotDivider : public ::testing::TestWithParam<int> {};

TEST_P(DotDivider, Timer0DotclockCountsVideoClocksPerDot) {
    Rig r;
    r.video.set_dot_divider(GetParam());
    r.mode(0, 0x100);
    for (int64_t c : {1, 7, 8, 100, 7000, 123456}) {
        r.at(c);
        EXPECT_EQ(r.val(0), static_cast<uint32_t>(((c * 715909 / 451584) / GetParam()) & 0xFFFF)) << c;
    }
}

TEST_P(DotDivider, Timer0DotclockSrc3SameAsSrc1) {
    Rig r;
    r.video.set_dot_divider(GetParam());
    r.mode(0, 0x300);
    r.at(7000);
    EXPECT_EQ(r.val(0), static_cast<uint32_t>(vclk(7000) / GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Dividers, DotDivider, ::testing::Values(4, 5, 7, 8, 10));

TEST(TimerSource, Timer0DefaultDotDivider8) {
    Rig r;
    r.mode(0, 0x100);
    r.at(7700);
    EXPECT_EQ(r.val(0), static_cast<uint32_t>(vclk(7700) / 8));
}

TEST(TimerSource, Timer0SysclockSources0And2) {
    for (uint32_t src : {0u, 2u}) {
        Rig r;
        r.mode(0, src << 8);
        r.at(5000);
        EXPECT_EQ(r.val(0), 5000u) << src;
    }
}

TEST(TimerSource, Timer1SysclockSources0And2) {
    for (uint32_t src : {0u, 2u}) {
        Rig r;
        r.mode(1, src << 8);
        r.at(5000);
        EXPECT_EQ(r.val(1), 5000u) << src;
    }
}

TEST(TimerSource, Timer1HblankCountsScanlines) {
    for (uint32_t src : {1u, 3u}) {
        Rig r;
        r.mode(1, src << 8);
        r.at(ceil_cycle(2560) - 1);
        EXPECT_EQ(r.val(1), 0u);
        r.at(ceil_cycle(2560));
        EXPECT_EQ(r.val(1), 1u);
        r.at(ceil_cycle(5 * 3413 + 2560));
        EXPECT_EQ(r.val(1), 6u);
    }
}

TEST(TimerSource, Timer1HblankMatchesVideoCountOverManyCycles) {
    Rig r;
    r.mode(1, 0x100);
    for (int64_t c = 5; c < 3000000; c += 40009) {
        r.at(c);
        EXPECT_EQ(r.val(1), r.video.hblank_count(c) & 0xFFFF) << c;
    }
}

TEST(TimerSource, Timer1Counts263LinesPerFrame) {
    Rig r;
    r.mode(1, 0x100);
    r.at(r.video.vblank_cycle(0));
    uint32_t a = r.val(1);
    r.at(r.video.vblank_cycle(1));
    EXPECT_EQ((r.val(1) - a) & 0xFFFF, 263u);
}

TEST(TimerSource, Timer2SysclockSources0And1) {
    for (uint32_t src : {0u, 1u}) {
        Rig r;
        r.mode(2, src << 8);
        r.at(5000);
        EXPECT_EQ(r.val(2), 5000u) << src;
    }
}

TEST(TimerSource, Timer2DivBy8Sources2And3) {
    for (uint32_t src : {2u, 3u}) {
        Rig r;
        r.mode(2, src << 8);
        r.at(7);
        EXPECT_EQ(r.val(2), 0u);
        r.at(8);
        EXPECT_EQ(r.val(2), 1u);
        r.at(800);
        EXPECT_EQ(r.val(2), 100u);
    }
}

TEST(TimerSource, Timer2DivPhaseStartsAtModeWrite) {
    Rig r;
    r.at(5);
    r.mode(2, 0x200);
    r.at(5 + 7);
    EXPECT_EQ(r.val(2), 0u);
    r.at(5 + 8);
    EXPECT_EQ(r.val(2), 1u);
}

TEST(TimerSource, Timer2IrqWithDivBy8) {
    Rig r;
    r.target(2, 10);
    r.mode(2, 0x218);
    r.run_to(79);
    EXPECT_FALSE(r.stat_bit(2));
    r.run_to(80);
    EXPECT_TRUE(r.stat_bit(2));
}

TEST(TimerSource, Timer0DotIrqTiming) {
    Rig r;
    r.target(0, 20);
    r.mode(0, 0x118);  // dotclock, div 8
    // 20 dots = 160 video clocks -> ceil(160 * 451584 / 715909) cycles.
    int64_t fire = ceil_cycle(160);
    r.run_to(fire - 1);
    EXPECT_FALSE(r.stat_bit(0));
    r.run_to(fire);
    EXPECT_TRUE(r.stat_bit(0));
}

TEST(TimerSource, Timer1HblankIrqTiming) {
    Rig r;
    r.target(1, 3);
    r.mode(1, 0x118);
    int64_t fire = ceil_cycle(2 * 3413 + 2560);  // third hblank start
    r.run_to(fire - 1);
    EXPECT_FALSE(r.stat_bit(1));
    r.run_to(fire);
    EXPECT_TRUE(r.stat_bit(1));
}

TEST(TimerSource, DotDividerChangeKeepsValueAndChangesRate) {
    Rig r;
    r.mode(0, 0x100);
    r.at(7000);
    const uint32_t before = static_cast<uint32_t>(vclk(7000) / 8);
    EXPECT_EQ(r.val(0), before);
    r.video.set_dot_divider(4);
    EXPECT_EQ(r.val(0), before);
    r.at(7000 + 70);  // ~111 video clocks at the new divider of 4
    EXPECT_EQ(r.val(0), before + static_cast<uint32_t>(vclk(70) / 4));
}

TEST(TimerSource, DotDividerChangeReplansIrq) {
    Rig r;
    r.target(0, 100);
    r.mode(0, 0x118);
    r.run_to(100);
    r.video.set_dot_divider(4);
    // Remaining dots: counter at 100 cycles = (157 / 8) = 19 dots; need 81 more at 4 clocks each.
    int64_t fire = 100 + ceil_cycle(81 * 4);
    r.run_to(fire - 3);
    EXPECT_FALSE(r.stat_bit(0));
    r.run_to(fire + 3);
    EXPECT_TRUE(r.stat_bit(0));
}

TEST(TimerSource, VideoReconfigureKeepsSysclockTimerRunning) {
    Rig r;
    r.mode(2, 0);
    r.at(1000);
    r.video.set_interlaced(true);
    r.at(2000);
    EXPECT_EQ(r.val(2), 2000u);
}

// ---- sync modes: lazy evaluation compared against a per-cycle reference ----

namespace {
struct SyncCase {
    int n, smode, reset, sync;
};

std::string sync_name(const ::testing::TestParamInfo<SyncCase>& i) {
    return "T" + std::to_string(i.param.n) + "_mode" + std::to_string(i.param.smode) + (i.param.reset ? "_reset" : "_free") +
           (i.param.sync ? "_sync" : "_nosync");
}

struct Ref {
    SyncCase c;
    uint16_t target;
    uint16_t v = 0;
    bool open = false;

    bool blank(Rig& r, int64_t cyc) const { return c.n == 0 ? r.video.in_hblank(cyc) : r.video.in_vblank(cyc); }
    void tick() {
        if (c.reset && v == target) v = 0;
        else v = static_cast<uint16_t>(v + 1);
    }
    // Advances one cycle (cycle `cyc`), returns true if the counter landed on target.
    bool step(Rig& r, int64_t cyc) {
        bool count = true;
        bool pb = c.n == 2 ? false : blank(r, cyc - 1), cb = c.n == 2 ? false : blank(r, cyc);
        if (c.sync) {
            if (c.n == 2) count = !(c.smode == 0 || c.smode == 3);
            else count = c.smode == 0 ? !pb : c.smode == 1 ? true : c.smode == 2 ? pb : open;
        }
        bool hit = false;
        if (count) {
            tick();
            hit = v == target;
        }
        if (c.sync && c.n != 2 && !pb && cb) {
            if (c.smode == 1 || c.smode == 2) v = 0;
            else if (c.smode == 3) open = true;
        }
        return hit;
    }
};

uint32_t mode_of(const SyncCase& c) { return (c.sync ? 1u : 0u) | (c.smode << 1) | (c.reset ? 8u : 0u); }
}  // namespace

class SyncModes : public ::testing::TestWithParam<SyncCase> {};

TEST_P(SyncModes, LazyValueMatchesPerCycleReference) {
    SyncCase c = GetParam();
    Rig r;
    r.target(c.n, 700);
    r.mode(c.n, mode_of(c));
    Ref ref{c, 700};
    const std::vector<int64_t> checks = {1,      2,      700,    1000,   1628,   1629,   1630,   2172,   2173,   3000,
                                         5000,   20000,  100000, 300000, 521258, 521259, 521260, 522000, 600000, 900000,
                                         1100000, 1200000};
    size_t ci = 0;
    for (int64_t cyc = 1; cyc <= checks.back(); ++cyc) {
        ref.step(r, cyc);
        if (cyc == checks[ci]) {
            r.at(cyc);
            EXPECT_EQ(r.val(c.n), ref.v) << "cycle " << cyc;
            ++ci;
        }
    }
}

TEST_P(SyncModes, FirstTargetIrqCycleMatchesReference) {
    SyncCase c = GetParam();
    Rig r;
    r.target(c.n, 500);
    r.mode(c.n, mode_of(c) | 0x50);  // irq on target, repeat
    Ref ref{c, 500};
    int64_t first = -1;
    for (int64_t cyc = 1; cyc <= 1200000 && first < 0; ++cyc)
        if (ref.step(r, cyc)) first = cyc;
    if (first < 0) {
        r.run_to(1200000);
        EXPECT_FALSE(r.stat_bit(c.n));
    } else {
        r.run_to(first - 1);
        EXPECT_FALSE(r.stat_bit(c.n)) << first;
        r.run_to(first);
        EXPECT_TRUE(r.stat_bit(c.n)) << first;
    }
}

std::vector<SyncCase> all_sync_cases() {
    std::vector<SyncCase> v;
    for (int n = 0; n < 3; ++n)
        for (int s = 0; s < 4; ++s)
            for (int reset = 0; reset < 2; ++reset) v.push_back({n, s, reset, 1});
    for (int n = 0; n < 3; ++n) v.push_back({n, 2, 0, 0});  // sync disabled: mode bits 1-2 ignored
    return v;
}

INSTANTIATE_TEST_SUITE_P(All, SyncModes, ::testing::ValuesIn(all_sync_cases()), sync_name);

TEST(TimerSync, Timer2Mode0And3StopAtCurrentValue) {
    for (uint32_t sm : {0u, 3u}) {
        Rig r;
        r.mode(2, 1 | (sm << 1));
        r.value(2, 123);
        r.at(10000);
        EXPECT_EQ(r.val(2), 123u) << sm;
    }
}

TEST(TimerSync, Timer2Mode1And2FreeRun) {
    for (uint32_t sm : {1u, 2u}) {
        Rig r;
        r.mode(2, 1 | (sm << 1));
        r.at(10000);
        EXPECT_EQ(r.val(2), 10000u) << sm;
    }
}

TEST(TimerSync, Timer2StoppedNeverFiresIrq) {
    Rig r;
    r.target(2, 10);
    r.mode(2, 0x19);
    r.run_to(100000);
    EXPECT_FALSE(r.stat_bit(2));
}

TEST(TimerSync, Timer0Mode0PausesDuringHblank) {
    Rig r;
    r.mode(0, 0x1);  // pause in hblank
    int64_t hb_start = ceil_cycle(2560), hb_end = ceil_cycle(3413);
    r.at(hb_start);
    uint32_t at_start = r.val(0);
    EXPECT_EQ(at_start, static_cast<uint32_t>(hb_start));
    r.at(hb_end - 1);
    EXPECT_EQ(r.val(0), at_start);
    r.at(hb_end + 10);
    EXPECT_EQ(r.val(0), at_start + 10);
}

TEST(TimerSync, Timer0Mode1ResetsAtHblankStart) {
    Rig r;
    r.mode(0, 0x3);
    int64_t hb = ceil_cycle(2560);
    r.at(hb - 1);
    EXPECT_EQ(r.val(0), static_cast<uint32_t>(hb - 1));
    r.at(hb);
    EXPECT_EQ(r.val(0), 0u);
    r.at(hb + 5);
    EXPECT_EQ(r.val(0), 5u);
}

TEST(TimerSync, Timer0Mode2CountsOnlyInsideHblank) {
    Rig r;
    r.mode(0, 0x5);
    int64_t hb = ceil_cycle(2560), end = ceil_cycle(3413);
    r.at(hb - 1);
    EXPECT_EQ(r.val(0), 0u);
    r.at(hb + 100);
    EXPECT_EQ(r.val(0), 100u);
    r.at(end + 50);
    uint32_t v = r.val(0);
    r.at(end + 500);
    EXPECT_EQ(r.val(0), v);  // paused outside the blank
}

TEST(TimerSync, Timer0Mode3WaitsForFirstHblankThenFreeRuns) {
    Rig r;
    r.mode(0, 0x7);
    int64_t hb = ceil_cycle(2560);
    r.at(hb);
    EXPECT_EQ(r.val(0), 0u);
    r.at(hb + 3000);
    EXPECT_EQ(r.val(0), 3000u);
    r.at(hb + 100000);
    EXPECT_EQ(r.val(0), 100000u & 0xFFFFu);
}

TEST(TimerSync, Timer1Mode1ResetsAtVblank) {
    Rig r;
    r.mode(1, 0x3);
    int64_t vb = r.video.vblank_cycle(0);
    r.at(vb - 1);
    EXPECT_EQ(r.val(1), static_cast<uint32_t>(vb - 1) & 0xFFFF);
    r.at(vb + 9);
    EXPECT_EQ(r.val(1), 9u);
}

TEST(TimerSync, Timer1Mode0PausesDuringVblank) {
    Rig r;
    r.mode(1, 0x1);
    r.value(1, 0);
    int64_t vb = r.video.vblank_cycle(0);
    uint32_t pre = static_cast<uint32_t>(vb) & 0xFFFF;
    r.at(vb + 1000);
    EXPECT_EQ(r.val(1), pre);
}

TEST(TimerSync, Timer1Mode2CountsOnlyInVblank) {
    Rig r;
    r.mode(1, 0x5);
    int64_t vb = r.video.vblank_cycle(0);
    r.at(vb - 1);
    EXPECT_EQ(r.val(1), 0u);
    r.at(vb + 77);
    EXPECT_EQ(r.val(1), 77u);
}

TEST(TimerSync, Timer1HblankSourceWithVblankReset) {
    Rig r;
    r.mode(1, 0x103);  // hblank clock, reset at vblank
    int64_t vb = r.video.vblank_cycle(0);
    r.at(vb - 1);
    EXPECT_EQ(r.val(1), 240u);  // hblank starts of lines 0..239 precede the VBLANK line
    r.at(vb + 1);
    EXPECT_EQ(r.val(1), 0u);
    r.at(r.video.vblank_cycle(1) - 1);
    EXPECT_EQ(r.val(1), 263u);  // a full frame of hblanks since the reset
}

TEST(TimerSync, SyncDisabledIgnoresModeBits) {
    Rig r;
    r.mode(0, 0x6);  // sync mode 3 bits set but sync enable clear
    r.at(5000);
    EXPECT_EQ(r.val(0), 5000u);
}

TEST(TimerSync, ModeWriteMidHblankSyncMode3WaitsForNextEdge) {
    Rig r;
    int64_t in_blank = ceil_cycle(2560) + 10;
    r.at(in_blank);
    r.mode(0, 0x7);
    r.at(ceil_cycle(3413) + 100);
    EXPECT_EQ(r.val(0), 0u);  // still waiting: next hblank start not reached
    r.at(ceil_cycle(3413 + 2560) + 100);
    EXPECT_EQ(r.val(0), 100u);
}

TEST(TimerSync, ModeRewriteRestartsGate) {
    Rig r;
    r.mode(0, 0x7);
    r.at(ceil_cycle(2560) + 500);
    EXPECT_EQ(r.val(0), 500u);
    r.mode(0, 0x7);
    EXPECT_EQ(r.val(0), 0u);
    r.at(r.ctx().cycles + 50);
    EXPECT_EQ(r.val(0), 0u);  // closed again until the next hblank
}

TEST(TimerSync, LongIdleGapWithSyncStaysConsistent) {
    Rig r;
    r.mode(0, 0x3);  // reset at each hblank start
    r.at(50000000);
    int64_t cyc = r.ctx().cycles;
    // value = cycles since the last hblank start
    int64_t last_edge = cyc;
    while (!r.video.in_hblank(last_edge) || r.video.in_hblank(last_edge - 1)) --last_edge;
    EXPECT_EQ(r.val(0), static_cast<uint32_t>(cyc - last_edge));
}
