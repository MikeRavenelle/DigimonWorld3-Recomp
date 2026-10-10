#include <gtest/gtest.h>

#include <vector>

#include "dw3/irq.h"
#include "hw_test_util.h"

using dw3::IrqController;
using dw3::IrqSource;

namespace {
constexpr uint32_t kStat = 0x1F801070u, kMask = 0x1F801074u;
struct Rig : dw3test::Clock {
    IrqController irq{bus};
};
}  // namespace

TEST(Irq, ResetState) {
    Rig r;
    EXPECT_EQ(r.r32(kStat), 0u);
    EXPECT_EQ(r.r32(kMask), 0u);
    EXPECT_FALSE(r.irq.pending());
}

class IrqSourceTest : public ::testing::TestWithParam<int> {};

TEST_P(IrqSourceTest, RaiseSetsItsBit) {
    Rig r;
    r.irq.raise(static_cast<IrqSource>(GetParam()));
    EXPECT_EQ(r.r32(kStat), 1u << GetParam());
    EXPECT_FALSE(r.irq.pending());  // masked
}

TEST_P(IrqSourceTest, PendingNeedsMatchingMask) {
    Rig r;
    r.w32(kMask, 1u << GetParam());
    r.irq.raise(static_cast<IrqSource>(GetParam()));
    EXPECT_TRUE(r.irq.pending());
    r.w32(kMask, ~(1u << GetParam()));
    EXPECT_FALSE(r.irq.pending());
}

TEST_P(IrqSourceTest, AckClearsOnlyThatBit) {
    Rig r;
    r.irq.raise_mask(0x7FFu);
    r.w32(kStat, ~(1u << GetParam()));
    EXPECT_EQ(r.r32(kStat), 0x7FFu & ~(1u << GetParam()));
}

INSTANTIATE_TEST_SUITE_P(AllSources, IrqSourceTest, ::testing::Range(0, 11));

TEST(Irq, SourceBitAssignments) {
    EXPECT_EQ(static_cast<int>(IrqSource::Vblank), 0);
    EXPECT_EQ(static_cast<int>(IrqSource::Gpu), 1);
    EXPECT_EQ(static_cast<int>(IrqSource::Cdrom), 2);
    EXPECT_EQ(static_cast<int>(IrqSource::Dma), 3);
    EXPECT_EQ(static_cast<int>(IrqSource::Tmr0), 4);
    EXPECT_EQ(static_cast<int>(IrqSource::Tmr1), 5);
    EXPECT_EQ(static_cast<int>(IrqSource::Tmr2), 6);
    EXPECT_EQ(static_cast<int>(IrqSource::PadMemcard), 7);
    EXPECT_EQ(static_cast<int>(IrqSource::Sio), 8);
    EXPECT_EQ(static_cast<int>(IrqSource::Spu), 9);
    EXPECT_EQ(static_cast<int>(IrqSource::Lightpen), 10);
}

TEST(Irq, AckWriteZeroClearsEverything) {
    Rig r;
    r.irq.raise_mask(0x7FF);
    r.w32(kStat, 0);
    EXPECT_EQ(r.irq.stat(), 0u);
}

TEST(Irq, AckWriteAllOnesClearsNothing) {
    Rig r;
    r.irq.raise_mask(0x123);
    r.w32(kStat, 0xFFFFFFFFu);
    EXPECT_EQ(r.irq.stat(), 0x123u);
}

TEST(Irq, AckIsAndNotSet) {
    Rig r;
    r.irq.raise_mask(0x0F0);
    r.w32(kStat, 0x0FF);  // writing a 1 over a set bit keeps it, 0 clears it
    EXPECT_EQ(r.irq.stat(), 0x0F0u);
    r.w32(kStat, 0x0C0);
    EXPECT_EQ(r.irq.stat(), 0x0C0u);
}

TEST(Irq, WriteCannotSetBits) {
    Rig r;
    r.w32(kStat, 0xFFFFFFFFu);
    EXPECT_EQ(r.irq.stat(), 0u);
}

TEST(Irq, Ack16BitLowHalf) {
    Rig r;
    r.irq.raise_mask(0x7FF);
    r.w16(kStat, 0xFFF0);
    EXPECT_EQ(r.irq.stat(), 0x7F0u);
}

TEST(Irq, Ack16BitHighHalfDoesNotTouchLow) {
    Rig r;
    r.irq.raise_mask(0x7FF);
    r.w16(kStat + 2, 0x0000);
    EXPECT_EQ(r.irq.stat(), 0x7FFu);
}

TEST(Irq, Ack8BitLanes) {
    Rig r;
    r.irq.raise_mask(0x7FF);
    r.w8(kStat, 0x0F);
    EXPECT_EQ(r.irq.stat(), 0x70Fu);
    r.w8(kStat + 1, 0x00);
    EXPECT_EQ(r.irq.stat(), 0x00Fu);
}

TEST(Irq, ReadWidths) {
    Rig r;
    r.irq.raise_mask(0x7A5);
    EXPECT_EQ(r.r32(kStat), 0x7A5u);
    EXPECT_EQ(r.r16(kStat), 0x7A5u);
    EXPECT_EQ(r.r16(kStat + 2), 0u);
    EXPECT_EQ(r.bus.read(kStat, 1), 0xA5u);
    EXPECT_EQ(r.bus.read(kStat + 1, 1), 0x07u);
}

TEST(Irq, MaskRoundTrip) {
    Rig r;
    r.w32(kMask, 0x5A5);
    EXPECT_EQ(r.r32(kMask), 0x5A5u);
    EXPECT_EQ(r.irq.mask(), 0x5A5u);
}

TEST(Irq, MaskKeepsOnly11Bits) {
    Rig r;
    r.w32(kMask, 0xFFFFFFFFu);
    EXPECT_EQ(r.r32(kMask), 0x7FFu);
}

TEST(Irq, Mask16BitWrites) {
    Rig r;
    r.w32(kMask, 0x7FF);
    r.w16(kMask, 0x0123);
    EXPECT_EQ(r.irq.mask(), 0x123u);
    r.w16(kMask + 2, 0xFFFF);  // upper half is not backed
    EXPECT_EQ(r.irq.mask(), 0x123u);
}

TEST(Irq, Mask8BitWrites) {
    Rig r;
    r.w8(kMask, 0xFF);
    EXPECT_EQ(r.irq.mask(), 0xFFu);
    r.w8(kMask + 1, 0x05);
    EXPECT_EQ(r.irq.mask(), 0x5FFu);
}

TEST(Irq, MaskDoesNotAffectStat) {
    Rig r;
    r.irq.raise(IrqSource::Gpu);
    r.w32(kMask, 0);
    EXPECT_EQ(r.irq.stat(), 2u);
}

TEST(Irq, RaiseMasksUnknownBits) {
    Rig r;
    r.irq.raise_mask(0xFFFFFFFFu);
    EXPECT_EQ(r.irq.stat(), 0x7FFu);
}

TEST(Irq, ReRaiseWhileSetKeepsBit) {
    Rig r;
    r.irq.raise(IrqSource::Vblank);
    r.irq.raise(IrqSource::Vblank);
    EXPECT_EQ(r.irq.stat(), 1u);
}

TEST(Irq, RaiseAfterAckSetsAgain) {
    Rig r;
    r.irq.raise(IrqSource::Dma);
    r.w32(kStat, 0);
    EXPECT_EQ(r.irq.stat(), 0u);
    r.irq.raise(IrqSource::Dma);
    EXPECT_EQ(r.irq.stat(), 8u);
}

TEST(Irq, LineCallbackOnRisingEdge) {
    Rig r;
    std::vector<bool> events;
    r.irq.on_line_change = [&](bool p) { events.push_back(p); };
    r.w32(kMask, 1);
    r.irq.raise(IrqSource::Vblank);
    EXPECT_EQ(events, (std::vector<bool>{true}));
}

TEST(Irq, LineCallbackNotRepeatedWhileHigh) {
    Rig r;
    int calls = 0;
    r.irq.on_line_change = [&](bool) { ++calls; };
    r.w32(kMask, 0x3);
    r.irq.raise(IrqSource::Vblank);
    r.irq.raise(IrqSource::Gpu);
    EXPECT_EQ(calls, 1);
}

TEST(Irq, LineCallbackFallsOnAck) {
    Rig r;
    std::vector<bool> events;
    r.irq.on_line_change = [&](bool p) { events.push_back(p); };
    r.w32(kMask, 1);
    r.irq.raise(IrqSource::Vblank);
    r.w32(kStat, 0);
    EXPECT_EQ(events, (std::vector<bool>{true, false}));
}

TEST(Irq, LineCallbackWhenMaskEnablesPendingStat) {
    Rig r;
    std::vector<bool> events;
    r.irq.on_line_change = [&](bool p) { events.push_back(p); };
    r.irq.raise(IrqSource::Cdrom);  // masked: no callback
    EXPECT_TRUE(events.empty());
    r.w32(kMask, 4);
    EXPECT_EQ(events, (std::vector<bool>{true}));
    r.w32(kMask, 0);
    EXPECT_EQ(events, (std::vector<bool>{true, false}));
}

TEST(Irq, MaskedRaiseDoesNotCallBack) {
    Rig r;
    int calls = 0;
    r.irq.on_line_change = [&](bool) { ++calls; };
    r.irq.raise(IrqSource::Spu);
    EXPECT_EQ(calls, 0);
}

TEST(Irq, PartialAckKeepsLineHigh) {
    Rig r;
    std::vector<bool> events;
    r.irq.on_line_change = [&](bool p) { events.push_back(p); };
    r.w32(kMask, 3);
    r.irq.raise_mask(3);
    r.w32(kStat, ~1u);  // clear bit 0 only
    EXPECT_TRUE(r.irq.pending());
    EXPECT_EQ(events, (std::vector<bool>{true}));
}

TEST(Irq, ReachableThroughKseg1AndRecompAccessors) {
    Rig r;
    r.irq.raise(IrqSource::Tmr1);
    EXPECT_EQ(dw3_read32(&r.ctx(), 0xBF801070u), 0x20u);
    dw3_write32(&r.ctx(), 0x9F801074u, 0x20u);
    EXPECT_EQ(r.irq.mask(), 0x20u);
    EXPECT_EQ(dw3_read16(&r.ctx(), 0xBF801074u), 0x20u);
}

TEST(IrqCpu, EnabledNeedsBothIecAndIm2) {
    RecompContext c{};
    EXPECT_FALSE(dw3::cpu_interrupt_enabled(c));
    c.cop0[12] = 1;
    EXPECT_FALSE(dw3::cpu_interrupt_enabled(c));
    c.cop0[12] = 1u << 10;
    EXPECT_FALSE(dw3::cpu_interrupt_enabled(c));
    c.cop0[12] = 1u | (1u << 10);
    EXPECT_TRUE(dw3::cpu_interrupt_enabled(c));
}

TEST(IrqCpu, OtherImBitsDoNotEnable) {
    RecompContext c{};
    c.cop0[12] = 1u | ((0xFFu << 8) & ~(1u << 10));
    EXPECT_FALSE(dw3::cpu_interrupt_enabled(c));
    c.cop0[12] = 0xFFFFFFFFu & ~1u;
    EXPECT_FALSE(dw3::cpu_interrupt_enabled(c));
}

TEST(IrqCpu, CauseIp2Mirror) {
    RecompContext c{};
    c.cop0[13] = 0x80000000u;
    dw3::set_cause_ip2(c, true);
    EXPECT_EQ(c.cop0[13], 0x80000400u);
    dw3::set_cause_ip2(c, false);
    EXPECT_EQ(c.cop0[13], 0x80000000u);
}

TEST(IrqCpu, DeliverableNeedsLineAndEnable) {
    Rig r;
    r.w32(kMask, 1);
    EXPECT_FALSE(dw3::cpu_interrupt_deliverable(r.ctx(), r.irq));
    r.irq.raise(IrqSource::Vblank);
    EXPECT_FALSE(dw3::cpu_interrupt_deliverable(r.ctx(), r.irq));
    r.ctx().cop0[12] = 0x401;
    EXPECT_TRUE(dw3::cpu_interrupt_deliverable(r.ctx(), r.irq));
    r.w32(kStat, 0);
    EXPECT_FALSE(dw3::cpu_interrupt_deliverable(r.ctx(), r.irq));
}
