#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "dw3/sio.h"
#include "hw_test_util.h"

using dw3::IrqController;
using dw3::SimplePads;
using dw3::Sio;
using Bytes = std::vector<uint8_t>;

namespace {
constexpr uint32_t kData = 0x1F801040u, kStat = 0x1F801044u, kMode = 0x1F801048u, kCtrl = 0x1F80104Au,
                   kBaud = 0x1F80104Eu, kIStat = 0x1F801070u;
constexpr int64_t kByte = 8 * 0x88;  // baud 0x88, factor x1
constexpr int64_t kSettle = kByte + Sio::kAckDelay + Sio::kAckWidth + 1;
constexpr uint32_t kSel = 0x1003;  // TXEN | JOYn | ACK IRQ enable

struct Rig : dw3test::Clock {
    IrqController irq{bus};
    Sio sio{bus, sched, irq};
    SimplePads pad;
    std::array<std::array<uint8_t, Sio::kCardSize>, 2> mem{};

    Rig() {
        sio.set_pad_source(0, &pad);
        sio.set_pad_source(1, &pad);
        w16(kBaud, 0x88);
        w16(kMode, 0x000D);
    }
    void ctl(uint32_t v) { w16(kCtrl, v); }
    void select(int port = 0) { ctl(kSel | (port ? 0x2000u : 0u)); }
    void deselect(int port = 0) { ctl(0x1000u | (port ? 0x2000u : 0u)); }
    uint32_t stat() { return r32(kStat); }
    void ack_irq() {
        ctl(r16(kCtrl) | 0x10);
        w32(kIStat, ~0x80u);
    }
    // One full exchange as libpad does it: write, wait for the ACK window, read RX. last_irq = IRQ7 pending.
    bool last_irq = false;
    uint8_t xfer(uint8_t tx) {
        w8(kData, tx);
        run_to(ctx().cycles + kSettle);
        last_irq = (irq.stat() & 0x80) != 0;
        uint8_t rx = static_cast<uint8_t>(bus.read(kData, 1));
        ack_irq();
        return rx;
    }
    Bytes xfers(const Bytes& tx) {
        Bytes r;
        for (auto b : tx) r.push_back(xfer(b));
        return r;
    }
    void card(int slot = 0) { ASSERT_TRUE(sio.attach_card(slot, mem[slot])); }
    uint8_t* sec(int slot, uint32_t s) { return mem[slot].data() + s * 128; }

    // Memory card helpers (port == slot).
    Bytes read_sector(uint32_t s, int port = 0) {
        select(port);
        Bytes tx = {0x81, 0x52, 0, 0, uint8_t(s >> 8), uint8_t(s)};
        tx.resize(10 + 128 + 2, 0);
        Bytes r = xfers(tx);
        deselect(port);
        return r;
    }
    Bytes write_sector(uint32_t s, const Bytes& d, int chk_xor = 0, int port = 0) {
        select(port);
        uint8_t chk = uint8_t(s >> 8) ^ uint8_t(s);
        Bytes tx = {0x81, 0x57, 0, 0, uint8_t(s >> 8), uint8_t(s)};
        for (auto b : d) {
            tx.push_back(b);
            chk ^= b;
        }
        tx.push_back(uint8_t(chk ^ chk_xor));
        tx.push_back(0);
        tx.push_back(0);
        tx.push_back(0);
        Bytes r = xfers(tx);
        deselect(port);
        return r;
    }
};

Bytes pattern(int seed) {
    Bytes b(128);
    for (int i = 0; i < 128; ++i) b[i] = uint8_t(i * 7 + seed);
    return b;
}
}  // namespace

// ---- registers ----

TEST(Sio, StatAfterResetIsTxReady) {
    Rig r;
    EXPECT_EQ(r.stat(), 5u);
}
TEST(Sio, StatHasNoRxOrIrqInitially) {
    Rig r;
    EXPECT_EQ(r.stat() & 0x282, 0u);
}
TEST(Sio, ModeReadsBack) {
    Rig r;
    r.w16(kMode, 0x000E);
    EXPECT_EQ(r.r16(kMode), 0x000Eu);
}
TEST(Sio, BaudReadsBack) {
    Rig r;
    r.w16(kBaud, 0x1234);
    EXPECT_EQ(r.r16(kBaud), 0x1234u);
}
TEST(Sio, CtrlReadsBack) {
    Rig r;
    r.ctl(0x2003);
    EXPECT_EQ(r.r16(kCtrl), 0x2003u);
}
TEST(Sio, CtrlAckBitNotStored) {
    Rig r;
    r.ctl(0x0013);
    EXPECT_EQ(r.r16(kCtrl), 0x0003u);
}
TEST(Sio, CtrlResetBitClearsCtrl) {
    Rig r;
    r.ctl(0x1003);
    r.ctl(0x40);
    EXPECT_EQ(r.r16(kCtrl), 0u);
}
TEST(Sio, Mode32BitReadPacksCtrlHigh) {
    Rig r;
    r.w16(kMode, 0x000D);
    r.ctl(0x1003);
    EXPECT_EQ(r.r32(kMode), 0x1003000Du);
}
TEST(Sio, Write32ToModeSetsCtrl) {
    Rig r;
    r.w32(kMode, 0x0003000Eu);
    EXPECT_EQ(r.r16(kMode), 0x000Eu);
    EXPECT_EQ(r.r16(kCtrl), 0x0003u);
}
TEST(Sio, Write8ToCtrlLowKeepsHigh) {
    Rig r;
    r.ctl(0x2000);
    r.w8(kCtrl, 0x03);
    EXPECT_EQ(r.r16(kCtrl), 0x2003u);
}
TEST(Sio, Write8ToCtrlHighKeepsLow) {
    Rig r;
    r.ctl(0x0003);
    r.w8(kCtrl + 1, 0x20);
    EXPECT_EQ(r.r16(kCtrl), 0x2003u);
}
TEST(Sio, Write8ToBaudHigh) {
    Rig r;
    r.w16(kBaud, 0x0088);
    r.w8(kBaud + 1, 0x01);
    EXPECT_EQ(r.r16(kBaud), 0x0188u);
}
TEST(Sio, StatIsReadOnly) {
    Rig r;
    r.w32(kStat, 0xFFFFFFFF);
    EXPECT_EQ(r.stat(), 5u);
}
TEST(Sio, EmptyRxReadsFF) {
    Rig r;
    EXPECT_EQ(r.bus.read(kData, 1), 0xFFu);
}
TEST(Sio, Stat8BitReadsBytes) {
    Rig r;
    EXPECT_EQ(r.bus.read(kStat, 1), 5u);
    EXPECT_EQ(r.bus.read(kStat + 1, 1), 0u);
}
TEST(Sio, Stat16BitRead) {
    Rig r;
    EXPECT_EQ(r.bus.read(kStat, 2), 5u);
}
TEST(Sio, ResetClearsFifoAndIrq) {
    Rig r;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(r.ctx().cycles + kSettle);
    EXPECT_TRUE(r.stat() & 2);
    r.ctl(0x40);
    EXPECT_EQ(r.stat(), 5u);
}
TEST(Sio, ResetCancelsTransfer) {
    Rig r;
    r.select();
    r.w8(kData, 0x01);
    r.ctl(0x40);
    r.run_to(r.ctx().cycles + 10000);
    EXPECT_EQ(r.stat(), 5u);
    EXPECT_EQ(r.sched.pending_count(), 0u);
}
TEST(Sio, ResetKeepsBaudAndMode) {
    Rig r;
    r.ctl(0x40);
    EXPECT_EQ(r.r16(kBaud), 0x88u);
    EXPECT_EQ(r.r16(kMode), 0x0Du);
}
TEST(Sio, UnmappedHighRegisterReadsZero) {
    Rig r;
    EXPECT_EQ(r.bus.read(0x1F80104C, 2), 0u);
}

// ---- timing / IRQ ----

TEST(SioTiming, ByteCyclesFromBaud) {
    Rig r;
    EXPECT_EQ(r.sio.byte_cycles(), kByte);
}
TEST(SioTiming, ByteCyclesScaleWithFactor16) {
    Rig r;
    r.w16(kMode, 0x0E);
    EXPECT_EQ(r.sio.byte_cycles(), kByte * 16);
}
TEST(SioTiming, ByteCyclesScaleWithFactor64) {
    Rig r;
    r.w16(kMode, 0x0F);
    EXPECT_EQ(r.sio.byte_cycles(), kByte * 64);
}
TEST(SioTiming, ZeroBaudDoesNotHang) {
    Rig r;
    r.w16(kBaud, 0);
    EXPECT_GT(r.sio.byte_cycles(), 0);
}
TEST(SioTiming, TxReady2DropsDuringTransfer) {
    Rig r;
    r.select();
    r.w8(kData, 0x01);
    EXPECT_EQ(r.stat() & 4, 0u);
    EXPECT_TRUE(r.sio.busy());
}
TEST(SioTiming, RxNotAvailableBeforeByteTime) {
    Rig r;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kByte - 1);
    EXPECT_EQ(r.stat() & 2, 0u);
}
TEST(SioTiming, RxAvailableAtByteTime) {
    Rig r;
    r.select();
    int64_t t0 = r.ctx().cycles;
    r.w8(kData, 0x01);
    r.run_to(t0 + kByte);
    EXPECT_TRUE(r.stat() & 2);
    EXPECT_TRUE(r.stat() & 4);
}
TEST(SioTiming, AckLowAfterDelay) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    int64_t t0 = r.ctx().cycles;
    r.w8(kData, 0x01);
    r.run_to(t0 + kByte + Sio::kAckDelay - 1);
    EXPECT_EQ(r.stat() & 0x80, 0u);
    r.run_to(t0 + kByte + Sio::kAckDelay);
    EXPECT_TRUE(r.stat() & 0x80);
}
TEST(SioTiming, AckReleasesAfterWidth) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    int64_t t0 = r.ctx().cycles;
    r.w8(kData, 0x01);
    r.run_to(t0 + kByte + Sio::kAckDelay + Sio::kAckWidth);
    EXPECT_EQ(r.stat() & 0x80, 0u);
}
TEST(SioTiming, IrqRaisedAtAckEdge) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    int64_t t0 = r.ctx().cycles;
    r.w8(kData, 0x01);
    r.run_to(t0 + kByte + Sio::kAckDelay - 1);
    EXPECT_EQ(r.irq.stat() & 0x80, 0u);
    r.run_to(t0 + kByte + Sio::kAckDelay);
    EXPECT_EQ(r.irq.stat() & 0x80, 0x80u);
    EXPECT_TRUE(r.stat() & 0x200);
}
TEST(SioTiming, NoIrqWhenAckIrqDisabled) {
    Rig r;
    r.pad.plugged[0] = true;
    r.ctl(0x0003);
    r.w8(kData, 0x01);
    r.run_to(kSettle);
    EXPECT_EQ(r.irq.stat() & 0x80, 0u);
    EXPECT_EQ(r.stat() & 0x200, 0u);
    EXPECT_TRUE(r.stat() & 2);
}
TEST(SioTiming, NoIrqWhenNoAck) {
    Rig r;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kSettle);
    EXPECT_EQ(r.irq.stat() & 0x80, 0u);
}
TEST(SioTiming, CtrlAckClearsStatIrq) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kSettle);
    ASSERT_TRUE(r.stat() & 0x200);
    r.ctl(r.r16(kCtrl) | 0x10);
    EXPECT_EQ(r.stat() & 0x200, 0u);
}
TEST(SioTiming, IStatMustBeClearedSeparately) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kSettle);
    r.ctl(r.r16(kCtrl) | 0x10);
    EXPECT_EQ(r.irq.stat() & 0x80, 0x80u);
    r.w32(kIStat, ~0x80u);
    EXPECT_EQ(r.irq.stat() & 0x80, 0u);
}
TEST(SioTiming, IrqNotReraisedWhileStatIrqSet) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kSettle);
    r.w32(kIStat, ~0x80u);  // clear I_STAT but not JOY_STAT.IRQ
    r.w8(kData, 0x42);
    r.run_to(r.ctx().cycles + kSettle);
    EXPECT_EQ(r.irq.stat() & 0x80, 0u);
}
TEST(SioTiming, IrqReraisedAfterAck) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.xfer(0x01);
    r.w8(kData, 0x42);
    r.run_to(r.ctx().cycles + kSettle);
    EXPECT_EQ(r.irq.stat() & 0x80, 0x80u);
}
TEST(SioTiming, RxFifoReadClearsNotEmpty) {
    Rig r;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kSettle);
    r.bus.read(kData, 1);
    EXPECT_EQ(r.stat() & 2, 0u);
}
TEST(SioTiming, RxFifoHoldsMultipleBytes) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(r.ctx().cycles + kSettle);
    r.w8(kData, 0x42);
    r.run_to(r.ctx().cycles + kSettle);
    EXPECT_EQ(r.sio.rx_count(), 2u);
    EXPECT_EQ(r.bus.read(kData, 1), 0xFFu);
    EXPECT_EQ(r.bus.read(kData, 1), 0x41u);
    EXPECT_EQ(r.sio.rx_count(), 0u);
}
TEST(SioTiming, RxFifoDropsBeyondEight) {
    Rig r;
    r.select();
    for (int i = 0; i < 10; ++i) {
        r.w8(kData, 0x01);
        r.run_to(r.ctx().cycles + kSettle);
    }
    EXPECT_EQ(r.sio.rx_count(), 8u);
}
TEST(SioTiming, Data32BitReadReturnsHeadByteOnly) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.xfer(0x01);  // consumed
    r.w8(kData, 0x42);
    r.run_to(r.ctx().cycles + kSettle);
    EXPECT_EQ(r.r32(kData), 0x41u);
}
TEST(SioTiming, SecondWriteWhileBusyIsQueued) {
    Rig r;
    r.select();
    r.w8(kData, 0x01);
    r.w8(kData, 0x02);
    EXPECT_EQ(r.stat() & 1, 0u);  // holding register full
    r.run_to(kByte * 2 + 1);
    EXPECT_EQ(r.sio.rx_count(), 2u);
    EXPECT_EQ(r.stat() & 1, 1u);
}
TEST(SioTiming, WriteWithoutTxEnableHeldUntilEnabled) {
    Rig r;
    r.ctl(0x1002);
    r.w8(kData, 0x01);
    r.run_to(10000);
    EXPECT_EQ(r.sio.rx_count(), 0u);
    r.ctl(0x1003);
    EXPECT_TRUE(r.sio.busy());
    r.run_to(r.ctx().cycles + kSettle);
    EXPECT_EQ(r.sio.rx_count(), 1u);
}
TEST(SioTiming, SlowBaudDelaysByte) {
    Rig r;
    r.w16(kMode, 0x0E);  // x16
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kByte * 16 - 1);
    EXPECT_EQ(r.sio.rx_count(), 0u);
    r.run_to(kByte * 16);
    EXPECT_EQ(r.sio.rx_count(), 1u);
}
TEST(SioTiming, RxIrqModeFiresOnThreshold) {
    Rig r;
    r.ctl(0x0803 | (1u << 8));  // RX IRQ enable, threshold 2 bytes
    r.w8(kData, 0x01);
    r.run_to(kByte);
    EXPECT_EQ(r.irq.stat() & 0x80, 0u);
    r.w8(kData, 0x01);
    r.run_to(kByte * 2);
    EXPECT_EQ(r.irq.stat() & 0x80, 0x80u);
}
TEST(SioTiming, RxIrqThresholdOneByte) {
    Rig r;
    r.ctl(0x0803);
    r.w8(kData, 0x01);
    r.run_to(kByte);
    EXPECT_EQ(r.irq.stat() & 0x80, 0x80u);
}
TEST(SioTiming, TxIrqFiresWhenTransferEnds) {
    Rig r;
    r.ctl(0x0403);
    r.w8(kData, 0x01);
    r.run_to(kByte - 1);
    EXPECT_EQ(r.irq.stat() & 0x80, 0u);
    r.run_to(kByte);
    EXPECT_EQ(r.irq.stat() & 0x80, 0x80u);
}
TEST(SioTiming, PendingAckCancelledByDeselect) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kByte + 10);
    r.deselect();
    r.run_to(kSettle * 2);
    EXPECT_EQ(r.irq.stat() & 0x80, 0u);
    EXPECT_EQ(r.stat() & 0x80, 0u);
}
TEST(SioTiming, NoSchedulerEventsWhenIdle) {
    Rig r;
    r.select();
    EXPECT_EQ(r.sched.pending_count(), 0u);
}

// ---- digital pad ----

TEST(SioPad, FullPollNoButtons) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    EXPECT_EQ(r.xfers({0x01, 0x42, 0x00, 0x00, 0x00}), (Bytes{0xFF, 0x41, 0x5A, 0xFF, 0xFF}));
}
TEST(SioPad, AckAfterEveryByteExceptLast) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    Bytes tx = {0x01, 0x42, 0x00, 0x00, 0x00};
    std::vector<bool> acks;
    for (auto b : tx) {
        r.xfer(b);
        acks.push_back(r.last_irq);
    }
    EXPECT_EQ(acks, (std::vector<bool>{true, true, true, true, false}));
}
TEST(SioPad, ButtonsLowThenHigh) {
    Rig r;
    r.pad.plugged[0] = true;
    r.pad.pressed[0] = 0x1234;
    r.select();
    Bytes rx = r.xfers({0x01, 0x42, 0, 0, 0});
    EXPECT_EQ(rx[3], uint8_t(~0x34));
    EXPECT_EQ(rx[4], uint8_t(~0x12));
}
TEST(SioPad, PortOneUsesPortOneState) {
    Rig r;
    r.pad.plugged = {true, true};
    r.pad.pressed = {0x0001, 0x0100};
    r.select(1);
    Bytes rx = r.xfers({0x01, 0x42, 0, 0, 0});
    EXPECT_EQ(rx[3], 0xFFu);
    EXPECT_EQ(rx[4], uint8_t(~0x01));
}
TEST(SioPad, PortZeroUsesPortZeroState) {
    Rig r;
    r.pad.plugged = {true, true};
    r.pad.pressed = {0x0001, 0x0100};
    r.select(0);
    Bytes rx = r.xfers({0x01, 0x42, 0, 0, 0});
    EXPECT_EQ(rx[3], uint8_t(~0x01));
    EXPECT_EQ(rx[4], 0xFFu);
}
TEST(SioPad, ButtonsSampledAtPollTime) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.xfers({0x01, 0x42, 0});
    r.pad.pressed[0] = dw3::kPadStart;  // changed between byte 3 and 4 -> low byte not yet sampled
    Bytes rx = r.xfers({0, 0});
    EXPECT_EQ(rx[0], uint8_t(~0x08));
}
TEST(SioPad, ExtraBytesAfterEndAreSilent) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.xfers({0x01, 0x42, 0, 0, 0});
    EXPECT_EQ(r.xfer(0), 0xFFu);
    EXPECT_FALSE(r.last_irq);
}
TEST(SioPad, DeselectRestartsSequence) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.xfers({0x01, 0x42});
    r.deselect();
    r.select();
    EXPECT_EQ(r.xfers({0x01, 0x42, 0}), (Bytes{0xFF, 0x41, 0x5A}));
}
TEST(SioPad, WithoutDeselectSequenceContinues) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.xfers({0x01, 0x42});
    EXPECT_EQ(r.xfer(0), 0x5Au);
}
TEST(SioPad, PortChangeRestartsSequence) {
    Rig r;
    r.pad.plugged = {true, true};
    r.select(0);
    r.xfers({0x01, 0x42});
    r.select(1);
    EXPECT_EQ(r.xfers({0x01, 0x42}), (Bytes{0xFF, 0x41}));
}
TEST(SioPad, UnconnectedNoAckAndFF) {
    Rig r;
    r.select();
    EXPECT_EQ(r.xfer(0x01), 0xFFu);
    EXPECT_FALSE(r.last_irq);
    EXPECT_EQ(r.stat() & 0x80, 0u);
}
TEST(SioPad, UnconnectedWholeSequenceFF) {
    Rig r;
    r.select();
    EXPECT_EQ(r.xfers({0x01, 0x42, 0, 0, 0}), (Bytes(5, 0xFF)));
}
TEST(SioPad, NullSourceIsUnconnected) {
    Rig r;
    r.sio.set_pad_source(0, nullptr);
    r.select();
    EXPECT_EQ(r.xfers({0x01, 0x42}), (Bytes{0xFF, 0xFF}));
}
TEST(SioPad, OnlyOnePortConnected) {
    Rig r;
    r.pad.plugged = {false, true};
    r.select(0);
    EXPECT_EQ(r.xfer(0x01), 0xFFu);
    EXPECT_FALSE(r.last_irq);
    r.deselect(0);
    r.select(1);
    r.xfer(0x01);
    EXPECT_TRUE(r.last_irq);
}
TEST(SioPad, UnknownAddressByteIgnored) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    EXPECT_EQ(r.xfers({0x55, 0x42, 0}), (Bytes(3, 0xFF)));
}
TEST(SioPad, NotSelectedNoResponse) {
    Rig r;
    r.pad.plugged[0] = true;
    r.ctl(0x1001);  // TXEN without /JOYn
    EXPECT_EQ(r.xfer(0x01), 0xFFu);
    EXPECT_FALSE(r.last_irq);
}
TEST(SioPad, RepeatedPollsAfterDeselectAllWork) {
    Rig r;
    r.pad.plugged[0] = true;
    for (int i = 0; i < 5; ++i) {
        r.pad.pressed[0] = uint16_t(1u << i);
        r.select();
        Bytes rx = r.xfers({0x01, 0x42, 0, 0, 0});
        r.deselect();
        EXPECT_EQ(rx[3], uint8_t(~(1u << i)));
    }
}
TEST(SioPad, DifferentPadBothPortsInSuccession) {
    Rig r;
    r.pad.plugged = {true, true};
    r.pad.pressed = {0x00F0, 0x0F00};
    r.select(0);
    Bytes a = r.xfers({0x01, 0x42, 0, 0, 0});
    r.deselect(0);
    r.select(1);
    Bytes b = r.xfers({0x01, 0x42, 0, 0, 0});
    EXPECT_EQ(a[3], 0x0Fu);
    EXPECT_EQ(a[4], 0xFFu);
    EXPECT_EQ(b[3], 0xFFu);
    EXPECT_EQ(b[4], 0xF0u);
}

class PadButtonTest : public ::testing::TestWithParam<std::tuple<int, int>> {};
TEST_P(PadButtonTest, SingleButtonActiveLow) {
    auto [port, bit] = GetParam();
    Rig r;
    r.pad.plugged = {true, true};
    r.pad.pressed[port] = uint16_t(1u << bit);
    r.select(port);
    Bytes rx = r.xfers({0x01, 0x42, 0, 0, 0});
    uint16_t wire = uint16_t(rx[3] | rx[4] << 8);
    EXPECT_EQ(wire, uint16_t(~(1u << bit)));
}
INSTANTIATE_TEST_SUITE_P(Sio, PadButtonTest, ::testing::Combine(::testing::Values(0, 1), ::testing::Range(0, 16)));

// ---- memory card ----

TEST(SioCard, NoCardNoAck) {
    Rig r;
    r.select();
    EXPECT_EQ(r.xfer(0x81), 0xFFu);
    EXPECT_FALSE(r.last_irq);
}
TEST(SioCard, NoCardWholeSequenceFF) {
    Rig r;
    EXPECT_EQ(r.read_sector(0)[5], 0xFFu);
    auto rx = r.read_sector(0);
    EXPECT_EQ(rx, Bytes(rx.size(), 0xFF));
}
TEST(SioCard, AttachWrongSizeRejected) {
    Rig r;
    std::vector<uint8_t> small(1000);
    EXPECT_FALSE(r.sio.attach_card(0, small));
    EXPECT_FALSE(r.sio.has_card(0));
}
TEST(SioCard, AttachSetsHasCard) {
    Rig r;
    r.card(0);
    EXPECT_TRUE(r.sio.has_card(0));
    EXPECT_FALSE(r.sio.has_card(1));
}
TEST(SioCard, DetachRemoves) {
    Rig r;
    r.card(0);
    r.sio.detach_card(0);
    r.select();
    EXPECT_EQ(r.xfer(0x81), 0xFFu);
    EXPECT_FALSE(r.last_irq);
}
TEST(SioCard, FreshCardFlagIsNewCard) {
    Rig r;
    r.card(0);
    EXPECT_EQ(r.sio.card_flag(0), 0x08u);
}
TEST(SioCard, AckOnAddressByte) {
    Rig r;
    r.card(0);
    r.select();
    EXPECT_EQ(r.xfer(0x81), 0xFFu);
    EXPECT_TRUE(r.last_irq);
}
TEST(SioCard, PadAddressNotAnsweredByCard) {
    Rig r;
    r.card(0);
    r.select();
    EXPECT_EQ(r.xfer(0x01), 0xFFu);
    EXPECT_FALSE(r.last_irq);
}
TEST(SioCard, CardAddressNotAnsweredByPad) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.xfer(0x81);
    EXPECT_FALSE(r.last_irq);
}
TEST(SioCard, GetIdSequence) {
    Rig r;
    r.card(0);
    r.select();
    Bytes rx = r.xfers({0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(rx, (Bytes{0xFF, 0x08, 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80}));
}
TEST(SioCard, GetIdLastByteNoAck) {
    Rig r;
    r.card(0);
    r.select();
    Bytes tx = {0x81, 0x53, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < tx.size(); ++i) {
        r.xfer(tx[i]);
        EXPECT_EQ(r.last_irq, i + 1 != tx.size()) << i;
    }
}
TEST(SioCard, UnknownCommandSilent) {
    Rig r;
    r.card(0);
    r.select();
    r.xfer(0x81);
    EXPECT_EQ(r.xfer(0x99), 0xFFu);
    EXPECT_FALSE(r.last_irq);
    EXPECT_EQ(r.xfer(0), 0xFFu);
}
TEST(SioCard, ReadHeaderBytes) {
    Rig r;
    r.card(0);
    Bytes rx = r.read_sector(0x0123);
    EXPECT_EQ(Bytes(rx.begin(), rx.begin() + 10),
              (Bytes{0xFF, 0x08, 0x5A, 0x5D, 0x00, 0x01, 0x5C, 0x5D, 0x01, 0x23}));
}
TEST(SioCard, ReadReturnsSectorData) {
    Rig r;
    r.card(0);
    Bytes p = pattern(3);
    std::copy(p.begin(), p.end(), r.sec(0, 5));
    Bytes rx = r.read_sector(5);
    EXPECT_EQ(Bytes(rx.begin() + 10, rx.begin() + 138), p);
}
TEST(SioCard, ReadChecksumAndGood) {
    Rig r;
    r.card(0);
    Bytes p = pattern(9);
    std::copy(p.begin(), p.end(), r.sec(0, 0x102));
    Bytes rx = r.read_sector(0x102);
    uint8_t chk = 0x01 ^ 0x02;
    for (auto b : p) chk ^= b;
    EXPECT_EQ(rx[138], chk);
    EXPECT_EQ(rx[139], 0x47u);
}
TEST(SioCard, ReadAckPattern) {
    Rig r;
    r.card(0);
    r.select();
    Bytes tx(140, 0);
    tx[0] = 0x81;
    tx[1] = 0x52;
    for (std::size_t i = 0; i < tx.size(); ++i) {
        r.xfer(tx[i]);
        EXPECT_EQ(r.last_irq, i + 1 != tx.size()) << i;
    }
}
TEST(SioCard, ReadSectorZero) {
    Rig r;
    r.card(0);
    r.sec(0, 0)[0] = 0x4D;
    Bytes rx = r.read_sector(0);
    EXPECT_EQ(rx[10], 0x4Du);
    EXPECT_EQ(rx[138], 0x4Du);  // msb ^ lsb == 0
}
TEST(SioCard, ReadLastSector) {
    Rig r;
    r.card(0);
    r.sec(0, 1023)[127] = 0x77;
    Bytes rx = r.read_sector(1023);
    EXPECT_EQ(rx[137], 0x77u);
    EXPECT_EQ(rx[139], 0x47u);
}
TEST(SioCard, ReadBadSectorConfirmsFFFF) {
    Rig r;
    r.card(0);
    Bytes rx = r.read_sector(1024);
    EXPECT_EQ(rx[8], 0xFFu);
    EXPECT_EQ(rx[9], 0xFFu);
    EXPECT_EQ(rx[10], 0xFFu);  // transfer ended; rest silent
}
TEST(SioCard, ReadBadSectorLastAckAbsent) {
    Rig r;
    r.card(0);
    r.select();
    Bytes tx = {0x81, 0x52, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0};
    for (std::size_t i = 0; i < tx.size(); ++i) {
        r.xfer(tx[i]);
        EXPECT_EQ(r.last_irq, i + 1 != tx.size()) << i;
    }
}
TEST(SioCard, ReadDoesNotChangeFlag) {
    Rig r;
    r.card(0);
    r.read_sector(0);
    EXPECT_EQ(r.sio.card_flag(0), 0x08u);
}
TEST(SioCard, ReadDoesNotModifyStorage) {
    Rig r;
    r.card(0);
    r.sec(0, 7)[3] = 5;
    r.read_sector(7);
    EXPECT_EQ(r.sec(0, 7)[3], 5u);
}
TEST(SioCard, WriteStoresData) {
    Rig r;
    r.card(0);
    Bytes p = pattern(11);
    r.write_sector(0x20, p);
    EXPECT_EQ(Bytes(r.sec(0, 0x20), r.sec(0, 0x20) + 128), p);
}
TEST(SioCard, WriteResponseSequence) {
    Rig r;
    r.card(0);
    Bytes p = pattern(1);
    Bytes rx = r.write_sector(0x0102, p);
    ASSERT_EQ(rx.size(), 6u + 128 + 4);
    EXPECT_EQ(Bytes(rx.begin(), rx.begin() + 6), (Bytes{0xFF, 0x08, 0x5A, 0x5D, 0x00, 0x01}));
    EXPECT_EQ(rx[6], 0x02u);  // echo of LSB
    for (int i = 1; i < 128; ++i) EXPECT_EQ(rx[6 + i], p[i - 1]) << i;
    EXPECT_EQ(rx[134], p[127]);
    EXPECT_EQ(rx[135], 0x5Cu);
    EXPECT_EQ(rx[136], 0x5Du);
    EXPECT_EQ(rx[137], 0x47u);
}
TEST(SioCard, WriteClearsNewCardFlag) {
    Rig r;
    r.card(0);
    r.write_sector(1, pattern(0));
    EXPECT_EQ(r.sio.card_flag(0), 0x00u);
}
TEST(SioCard, FlagReportedAfterWrite) {
    Rig r;
    r.card(0);
    r.write_sector(1, pattern(0));
    EXPECT_EQ(r.read_sector(1)[1], 0x00u);
}
TEST(SioCard, FlagReportedBeforeWrite) {
    Rig r;
    r.card(0);
    EXPECT_EQ(r.read_sector(1)[1], 0x08u);
}
TEST(SioCard, ReattachResetsFlag) {
    Rig r;
    r.card(0);
    r.write_sector(1, pattern(0));
    r.card(0);
    EXPECT_EQ(r.sio.card_flag(0), 0x08u);
}
TEST(SioCard, WriteBadChecksumRejected) {
    Rig r;
    r.card(0);
    Bytes rx = r.write_sector(2, pattern(5), 0x01);
    EXPECT_EQ(rx.back(), 0x4Eu);
    EXPECT_EQ(Bytes(r.sec(0, 2), r.sec(0, 2) + 128), Bytes(128, 0));
}
TEST(SioCard, WriteBadChecksumKeepsFlag) {
    Rig r;
    r.card(0);
    r.write_sector(2, pattern(5), 0x80);
    EXPECT_EQ(r.sio.card_flag(0), 0x08u);
}
TEST(SioCard, WriteBadSectorStatusFF) {
    Rig r;
    r.card(0);
    Bytes rx = r.write_sector(1024, pattern(5));
    EXPECT_EQ(rx.back(), 0xFFu);
    EXPECT_EQ(r.sio.card_flag(0), 0x08u);
}
TEST(SioCard, WriteBadSectorModifiesNothing) {
    Rig r;
    r.card(0);
    r.write_sector(0xFFFF, pattern(5));
    for (auto b : r.mem[0]) ASSERT_EQ(b, 0);
}
TEST(SioCard, WriteLastSector) {
    Rig r;
    r.card(0);
    Bytes p = pattern(40);
    Bytes rx = r.write_sector(1023, p);
    EXPECT_EQ(rx.back(), 0x47u);
    EXPECT_EQ(Bytes(r.sec(0, 1023), r.sec(0, 1023) + 128), p);
}
TEST(SioCard, WriteLeavesNeighboursAlone) {
    Rig r;
    r.card(0);
    r.write_sector(10, pattern(1));
    EXPECT_EQ(r.sec(0, 9)[127], 0u);
    EXPECT_EQ(r.sec(0, 11)[0], 0u);
}
TEST(SioCard, WriteCallbackFires) {
    Rig r;
    r.card(0);
    std::vector<std::pair<int, uint32_t>> got;
    r.sio.on_card_write = [&](int slot, uint32_t s) { got.push_back({slot, s}); };
    r.write_sector(77, pattern(1));
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].second, 77u);
    EXPECT_EQ(got[0].first, 0);
}
TEST(SioCard, WriteCallbackNotFiredOnBadChecksum) {
    Rig r;
    r.card(0);
    int n = 0;
    r.sio.on_card_write = [&](int, uint32_t) { ++n; };
    r.write_sector(77, pattern(1), 1);
    EXPECT_EQ(n, 0);
}
TEST(SioCard, WriteThenReadRoundTrip) {
    Rig r;
    r.card(0);
    Bytes p = pattern(99);
    r.write_sector(0x155, p);
    Bytes rx = r.read_sector(0x155);
    EXPECT_EQ(Bytes(rx.begin() + 10, rx.begin() + 138), p);
    EXPECT_EQ(rx[139], 0x47u);
}
TEST(SioCard, WriteAckPattern) {
    Rig r;
    r.card(0);
    r.select();
    Bytes tx(6 + 128 + 4, 0);
    tx[0] = 0x81;
    tx[1] = 0x57;
    tx[4] = 0;
    tx[5] = 0;
    for (std::size_t i = 0; i < tx.size(); ++i) {
        r.xfer(tx[i]);
        EXPECT_EQ(r.last_irq, i + 1 != tx.size()) << i;
    }
}
TEST(SioCard, SecondCardSlotIndependent) {
    Rig r;
    r.card(0);
    r.card(1);
    r.write_sector(3, pattern(7), 0, 1);
    EXPECT_EQ(Bytes(r.sec(1, 3), r.sec(1, 3) + 128), pattern(7));
    EXPECT_EQ(Bytes(r.sec(0, 3), r.sec(0, 3) + 128), Bytes(128, 0));
    EXPECT_EQ(r.sio.card_flag(0), 0x08u);
    EXPECT_EQ(r.sio.card_flag(1), 0x00u);
}
TEST(SioCard, CardOnlyInSlotOne) {
    Rig r;
    r.card(1);
    EXPECT_EQ(r.read_sector(0, 0)[1], 0xFFu);
    EXPECT_EQ(r.read_sector(0, 1)[1], 0x08u);
}
TEST(SioCard, DeselectMidReadRestarts) {
    Rig r;
    r.card(0);
    r.select();
    r.xfers({0x81, 0x52, 0, 0});
    r.deselect();
    r.select();
    EXPECT_EQ(r.xfers({0x81, 0x53}), (Bytes{0xFF, 0x08}));
}
TEST(SioCard, DeselectMidWriteDoesNotCommit) {
    Rig r;
    r.card(0);
    r.select();
    Bytes tx = {0x81, 0x57, 0, 0, 0, 5};
    for (int i = 0; i < 128; ++i) tx.push_back(0xAA);
    r.xfers(tx);
    r.deselect();
    EXPECT_EQ(r.sec(0, 5)[0], 0u);
    EXPECT_EQ(r.sio.card_flag(0), 0x08u);
}
TEST(SioCard, ReadAfterAbortedReadWorks) {
    Rig r;
    r.card(0);
    r.sec(0, 2)[0] = 0x31;
    r.select();
    r.xfers({0x81, 0x52, 0, 0, 0});
    r.deselect();
    EXPECT_EQ(r.read_sector(2)[10], 0x31u);
}

class CardSectorTest : public ::testing::TestWithParam<uint32_t> {};
TEST_P(CardSectorTest, RoundTrip) {
    uint32_t s = GetParam();
    Rig r;
    r.card(0);
    Bytes p = pattern(int(s));
    EXPECT_EQ(r.write_sector(s, p).back(), 0x47u);
    Bytes rx = r.read_sector(s);
    EXPECT_EQ(Bytes(rx.begin() + 10, rx.begin() + 138), p);
    EXPECT_EQ(rx[138], uint8_t(uint8_t(s >> 8) ^ uint8_t(s) ^ [&] {
                  uint8_t c = 0;
                  for (auto b : p) c ^= b;
                  return c;
              }()));
}
INSTANTIATE_TEST_SUITE_P(Sio, CardSectorTest,
                         ::testing::Values(0u, 1u, 2u, 15u, 16u, 127u, 128u, 255u, 256u, 511u, 512u, 1000u, 1022u, 1023u));

class CardBadSectorTest : public ::testing::TestWithParam<uint32_t> {};
TEST_P(CardBadSectorTest, ReadAndWriteRejected) {
    uint32_t s = GetParam();
    Rig r;
    r.card(0);
    Bytes rx = r.read_sector(s);
    EXPECT_EQ(rx[8], 0xFFu);
    EXPECT_EQ(rx[9], 0xFFu);
    EXPECT_EQ(r.write_sector(s, pattern(1)).back(), 0xFFu);
}
INSTANTIATE_TEST_SUITE_P(Sio, CardBadSectorTest, ::testing::Values(1024u, 1025u, 2048u, 0x8000u, 0xFFFFu));

// ---- libpad-style end-to-end ----

TEST(SioLibpad, PollLoopUsingIrqAndStatOnly) {
    Rig r;
    r.pad.plugged[0] = true;
    r.pad.pressed[0] = dw3::kPadCross | dw3::kPadStart;
    r.irq.write(4, 0x80, 4);  // I_MASK: IRQ7
    r.ctl(0x40);
    r.w16(kMode, 0x000D);
    r.w16(kBaud, 0x88);
    r.select();
    Bytes tx = {0x01, 0x42, 0, 0, 0}, rx;
    for (std::size_t i = 0; i < tx.size(); ++i) {
        r.w8(kData, tx[i]);
        if (i + 1 < tx.size()) {  // wait for the ACK IRQ
            int guard = 0;
            while (!r.irq.pending() && guard++ < 100000) r.run_to(r.ctx().cycles + 50);
            ASSERT_TRUE(r.irq.pending());
        } else {  // last byte is not acknowledged: wait for RX
            int guard = 0;
            while (!(r.stat() & 2) && guard++ < 100000) r.run_to(r.ctx().cycles + 50);
        }
        ASSERT_TRUE(r.stat() & 2);
        rx.push_back(uint8_t(r.bus.read(kData, 1)));
        r.run_to(r.ctx().cycles + 200);  // let /ACK finish before acknowledging
        r.ack_irq();
    }
    r.deselect();
    EXPECT_EQ(rx, (Bytes{0xFF, 0x41, 0x5A, uint8_t(~0x08), uint8_t(~0x40)}));
}
TEST(SioLibpad, IrqLineDrivesCpuPending) {
    Rig r;
    r.pad.plugged[0] = true;
    r.irq.write(4, 0x80, 4);
    r.select();
    EXPECT_FALSE(r.irq.pending());
    r.w8(kData, 0x01);
    r.run_to(kSettle);
    EXPECT_TRUE(r.irq.pending());
    r.ack_irq();
    EXPECT_FALSE(r.irq.pending());
}
TEST(SioLibpad, TwoPortsInterleavedPolls) {
    Rig r;
    r.pad.plugged = {true, true};
    r.pad.pressed = {dw3::kPadUp, dw3::kPadDown};
    for (int frame = 0; frame < 3; ++frame) {
        for (int port = 0; port < 2; ++port) {
            r.select(port);
            Bytes rx = r.xfers({0x01, 0x42, 0, 0, 0});
            r.deselect(port);
            uint16_t wire = uint16_t(rx[3] | rx[4] << 8);
            EXPECT_EQ(uint16_t(~wire), r.pad.pressed[port]);
        }
    }
}
TEST(SioLibpad, MemcardAndPadSharePort) {
    Rig r;
    r.pad.plugged[0] = true;
    r.card(0);
    r.select();
    EXPECT_EQ(r.xfers({0x01, 0x42})[1], 0x41u);
    r.deselect();
    EXPECT_EQ(r.read_sector(0)[1], 0x08u);
    r.select();
    EXPECT_EQ(r.xfers({0x01, 0x42})[1], 0x41u);
}
TEST(SioLibpad, DetectsMissingPadViaMissingAck) {
    Rig r;
    r.irq.write(4, 0x80, 4);
    r.select();
    r.w8(kData, 0x01);
    r.run_to(kSettle * 3);
    EXPECT_FALSE(r.irq.pending());
    EXPECT_TRUE(r.stat() & 2);
    EXPECT_EQ(r.bus.read(kData, 1), 0xFFu);
}
TEST(SioLibpad, ReselectAfterResetPolls) {
    Rig r;
    r.pad.plugged[0] = true;
    r.select();
    r.xfers({0x01, 0x42});
    r.ctl(0x40);
    r.w16(kBaud, 0x88);
    r.select();
    EXPECT_EQ(r.xfers({0x01, 0x42, 0}), (Bytes{0xFF, 0x41, 0x5A}));
}

// ---- DualShock configuration protocol (libpad probes with 0x43/0x45/...) ----

namespace {
Bytes pad_cmd(Rig& r, const Bytes& tx) {
    r.select();
    Bytes rx = r.xfers(tx);
    r.deselect();
    return rx;
}
void enter_config(Rig& r) { pad_cmd(r, {0x01, 0x43, 0x00, 0x01, 0x00}); }
void exit_config(Rig& r) { pad_cmd(r, {0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); }
}  // namespace

TEST(SioDualShock, StartsDigital) {
    Rig r;
    r.pad.plugged[0] = true;
    Bytes rx = pad_cmd(r, {0x01, 0x42, 0x00, 0x00, 0x00});
    EXPECT_EQ(rx[1], 0x41);
    EXPECT_FALSE(r.sio.pad_analog(0));
}

TEST(SioDualShock, EnterConfigFromDigitalRepliesWithPollData) {
    Rig r;
    r.pad.plugged[0] = true;
    r.pad.pressed[0] = dw3::kPadCross;
    r.select();
    Bytes rx = r.xfers({0x01, 0x43, 0x00, 0x01, 0x00});
    r.deselect();
    EXPECT_EQ(rx[1], 0x41);
    EXPECT_EQ(rx[2], 0x5A);
    EXPECT_EQ(rx[4], uint8_t(~(dw3::kPadCross >> 8)));
    EXPECT_TRUE(r.sio.pad_config_mode(0));
}

TEST(SioDualShock, ConfigModeIdIsF3WithSixBytePayload) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    r.select();
    Bytes rx = r.xfers({0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(rx[1], 0xF3);
    EXPECT_EQ(r.xfer(0), 0xFFu);  // transfer ended after the 6th payload byte
    EXPECT_FALSE(r.last_irq);
    r.deselect();
}

TEST(SioDualShock, QueryModelReportsDualShockAndLed) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    Bytes rx = pad_cmd(r, {0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(rx[3], 0x01);
    EXPECT_EQ(rx[4], 0x02);
    EXPECT_EQ(rx[5], 0x00);  // LED off (digital)
    EXPECT_EQ(rx[6], 0x02);
    EXPECT_EQ(rx[7], 0x01);
    EXPECT_EQ(rx[8], 0x00);
}

TEST(SioDualShock, SetAnalogModeChangesIdAfterExit) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    pad_cmd(r, {0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00});
    EXPECT_TRUE(r.sio.pad_analog(0));
    exit_config(r);
    EXPECT_FALSE(r.sio.pad_config_mode(0));
    Bytes rx = pad_cmd(r, {0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(rx[1], 0x73);
    EXPECT_EQ(rx[5], 0x80);  // sticks centred
    EXPECT_EQ(rx[8], 0x80);
}

TEST(SioDualShock, QueryModelLedReflectsAnalog) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    pad_cmd(r, {0x01, 0x44, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});
    Bytes rx = pad_cmd(r, {0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(rx[5], 0x01);
}

TEST(SioDualShock, ConstantQueries) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    Bytes a = pad_cmd(r, {0x01, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ((Bytes{a.begin() + 3, a.end()}), (Bytes{0x00, 0x00, 0x01, 0x02, 0x00, 0x0A}));
    Bytes b = pad_cmd(r, {0x01, 0x46, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ((Bytes{b.begin() + 3, b.end()}), (Bytes{0x00, 0x00, 0x01, 0x01, 0x01, 0x14}));
    Bytes c = pad_cmd(r, {0x01, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ((Bytes{c.begin() + 3, c.end()}), (Bytes{0x00, 0x00, 0x02, 0x00, 0x01, 0x00}));
    Bytes d = pad_cmd(r, {0x01, 0x4C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(d[6], 0x07);
}

TEST(SioDualShock, RumbleMappingReturnsPreviousAndStoresNew) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    Bytes a = pad_cmd(r, {0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ((Bytes{a.begin() + 3, a.end()}), (Bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
    Bytes b = pad_cmd(r, {0x01, 0x4D, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ((Bytes{b.begin() + 3, b.end()}), (Bytes{0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(SioDualShock, ConfigCommandsOutsideConfigActAsPoll) {
    Rig r;
    r.pad.plugged[0] = true;
    Bytes rx = pad_cmd(r, {0x01, 0x45, 0x00, 0x00, 0x00});
    EXPECT_EQ(rx[1], 0x41);
    EXPECT_EQ(rx[3], 0xFF);  // buttons released
    EXPECT_FALSE(r.sio.pad_config_mode(0));
}

TEST(SioDualShock, ExitConfigReturnsToDigitalId) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    exit_config(r);
    Bytes rx = pad_cmd(r, {0x01, 0x42, 0x00, 0x00, 0x00});
    EXPECT_EQ(rx[1], 0x41);
}

TEST(SioDualShock, PortsHaveIndependentState) {
    Rig r;
    r.pad.plugged[0] = r.pad.plugged[1] = true;
    enter_config(r);
    EXPECT_TRUE(r.sio.pad_config_mode(0));
    EXPECT_FALSE(r.sio.pad_config_mode(1));
}

// ---- Vibration commands ----

TEST(PadRumble, DecodesMappedMotorBytes) {
    const std::array<uint8_t, 6> map{0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(dw3::decode_rumble(map, {0x01, 0x80, 0xFF, 0xFF, 0xFF, 0xFF}), (dw3::PadRumble{true, 0x80}));
    EXPECT_EQ(dw3::decode_rumble(map, {0x00, 0xFF, 0, 0, 0, 0}), (dw3::PadRumble{false, 0xFF}));
    EXPECT_FALSE(dw3::decode_rumble(map, {0, 0, 0, 0, 0, 0}).any());
}

TEST(PadRumble, SmallMotorUsesBitZeroOnly) {
    const std::array<uint8_t, 6> map{0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(dw3::decode_rumble(map, {0xFE, 0, 0, 0, 0, 0}).small_on);
    EXPECT_TRUE(dw3::decode_rumble(map, {0x41, 0, 0, 0, 0, 0}).small_on);
}

TEST(PadRumble, UnmappedBytesDriveNothing) {
    const std::array<uint8_t, 6> map{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_FALSE(dw3::decode_rumble(map, {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}).any());
}

TEST(PadRumble, MapMayPutMotorsOnOtherBytes) {
    const std::array<uint8_t, 6> map{0xFF, 0x01, 0x00, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(dw3::decode_rumble(map, {0, 0x40, 0x01, 0, 0, 0}), (dw3::PadRumble{true, 0x40}));
}

TEST(SioDualShock, PollAfterMotorMapSetsRumble) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    pad_cmd(r, {0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
    exit_config(r);
    EXPECT_FALSE(r.sio.pad_rumble(0).any());
    pad_cmd(r, {0x01, 0x42, 0x00, 0x01, 0xC0});
    EXPECT_EQ(r.sio.pad_rumble(0), (dw3::PadRumble{true, 0xC0}));
    pad_cmd(r, {0x01, 0x42, 0x00, 0x00, 0x00});
    EXPECT_FALSE(r.sio.pad_rumble(0).any());
}

TEST(SioDualShock, NoRumbleBeforeMotorsAreMapped) {
    Rig r;
    r.pad.plugged[0] = true;
    pad_cmd(r, {0x01, 0x42, 0x00, 0x01, 0xFF});
    EXPECT_FALSE(r.sio.pad_rumble(0).any());
}

TEST(SioDualShock, EnteringConfigModeDoesNotRumble) {
    Rig r;
    r.pad.plugged[0] = true;
    enter_config(r);
    pad_cmd(r, {0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});
    exit_config(r);
    enter_config(r);  // its parameter byte (1) sits where a motor byte would
    EXPECT_FALSE(r.sio.pad_rumble(0).any());
}
