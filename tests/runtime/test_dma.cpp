#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "dw3/dma.h"
#include "hw_test_util.h"

using dw3::Dma;
using dw3::DmaChannel;
using dw3::DmaDevice;
using dw3::IrqController;

namespace {
constexpr uint32_t kBase = 0x1F801080u, kDpcr = 0x1F8010F0u, kDicr = 0x1F8010F4u, kStat = 0x1F801070u;

struct FakeDevice : DmaDevice {
    bool ready = true;
    uint32_t cpw = 1;
    std::vector<std::vector<uint32_t>> writes;  // one entry per dma_write call
    std::vector<size_t> read_sizes;
    uint32_t next_word = 0x1000;  // dma_read yields next_word, next_word+1, ...
    bool dma_ready(bool) const override { return ready; }
    void dma_write(std::span<const uint32_t> w) override { writes.emplace_back(w.begin(), w.end()); }
    void dma_read(std::span<uint32_t> out) override {
        read_sizes.push_back(out.size());
        for (auto& w : out) w = next_word++;
    }
    uint32_t dma_cycles_per_word() const override { return cpw; }
    std::vector<uint32_t> all_written() const {
        std::vector<uint32_t> v;
        for (auto& w : writes) v.insert(v.end(), w.begin(), w.end());
        return v;
    }
};

struct Rig : dw3test::Clock {
    IrqController irq{bus};
    Dma dma{bus, sched, irq};
    std::vector<std::pair<uint32_t, uint32_t>> written;
    std::vector<std::string> errors;

    Rig() {
        dma.on_ram_written = [this](uint32_t a, uint32_t l) { written.emplace_back(a, l); };
        dma.on_error = [this](const std::string& m) { errors.push_back(m); };
    }
    static uint32_t ch_reg(int ch, int r) { return kBase + ch * 16 + r * 4; }
    void madr(int ch, uint32_t v) { w32(ch_reg(ch, 0), v); }
    void bcr(int ch, uint32_t v) { w32(ch_reg(ch, 1), v); }
    void chcr(int ch, uint32_t v) { w32(ch_reg(ch, 2), v); }
    void enable(int ch) { w32(kDpcr, r32(kDpcr) | (8u << (4 * ch))); }
    void enable_all() { w32(kDpcr, 0x0FFFFFFFu); }
    void poke(uint32_t addr, uint32_t v) { std::memcpy(bus.ram() + addr, &v, 4); }
    uint32_t peek(uint32_t addr) {
        uint32_t v;
        std::memcpy(&v, bus.ram() + addr, 4);
        return v;
    }
    void irq_unmask() { w32(0x1F801074u, 0x7FF); }
};
}  // namespace

// ---- register file ----

TEST(DmaRegs, DpcrResetValue) {
    Rig r;
    EXPECT_EQ(r.r32(kDpcr), 0x07654321u);
    EXPECT_EQ(r.dma.dpcr(), 0x07654321u);
}

TEST(DmaRegs, DpcrReadWrite) {
    Rig r;
    r.w32(kDpcr, 0x0BADF00Du);
    EXPECT_EQ(r.r32(kDpcr), 0x0BADF00Du);
}

TEST(DmaRegs, DicrResetZero) {
    Rig r;
    EXPECT_EQ(r.r32(kDicr), 0u);
}

class DmaChan : public ::testing::TestWithParam<int> {};

TEST_P(DmaChan, MadrIs24Bit) {
    Rig r;
    r.madr(GetParam(), 0xFFFFFFFFu);
    EXPECT_EQ(r.r32(Rig::ch_reg(GetParam(), 0)), 0x00FFFFFFu);
    EXPECT_EQ(r.dma.madr(static_cast<DmaChannel>(GetParam())), 0x00FFFFFFu);
}

TEST_P(DmaChan, BcrRoundTrip) {
    Rig r;
    r.bcr(GetParam(), 0x00120034u);
    EXPECT_EQ(r.r32(Rig::ch_reg(GetParam(), 1)), 0x00120034u);
}

TEST_P(DmaChan, MadrPartialWrites) {
    Rig r;
    r.madr(GetParam(), 0x00112233u);
    r.w16(Rig::ch_reg(GetParam(), 0), 0xAABB);
    EXPECT_EQ(r.r32(Rig::ch_reg(GetParam(), 0)), 0x0011AABBu);
    r.w8(Rig::ch_reg(GetParam(), 0) + 2, 0x77);
    EXPECT_EQ(r.r32(Rig::ch_reg(GetParam(), 0)), 0x0077AABBu);
}

TEST_P(DmaChan, RegisterBlocksDoNotAlias) {
    Rig r;
    int ch = GetParam();
    r.madr(ch, 0x1234);
    for (int o = 0; o < 7; ++o)
        if (o != ch) EXPECT_EQ(r.r32(Rig::ch_reg(o, 0)), 0u);
}

TEST_P(DmaChan, DpcrEnableGatesStart) {
    Rig r;
    int ch = GetParam();
    FakeDevice dev;
    if (ch != 6) r.dma.attach(static_cast<DmaChannel>(ch), &dev);
    r.madr(ch, 0x1000);
    r.bcr(ch, 4);
    r.chcr(ch, ch == 6 ? 0x11000002u : 0x11000001u);  // from RAM, manual trigger
    EXPECT_EQ(dev.writes.size(), 0u);
    EXPECT_TRUE(r.dma.busy(static_cast<DmaChannel>(ch)));
    EXPECT_FALSE(r.dma.in_flight(static_cast<DmaChannel>(ch)));
    r.enable(ch);
    EXPECT_TRUE(r.dma.in_flight(static_cast<DmaChannel>(ch)));
    if (ch != 6) EXPECT_EQ(dev.writes.size(), 1u);
}

TEST_P(DmaChan, ManualBurstFromRamReachesDevice) {
    Rig r;
    int ch = GetParam();
    if (ch == 6) GTEST_SKIP();
    FakeDevice dev;
    r.dma.attach(static_cast<DmaChannel>(ch), &dev);
    for (int i = 0; i < 4; ++i) r.poke(0x2000 + i * 4, 100 + i);
    r.enable_all();
    r.madr(ch, 0x2000);
    r.bcr(ch, 4);
    r.chcr(ch, 0x11000001u);
    EXPECT_EQ(dev.all_written(), (std::vector<uint32_t>{100, 101, 102, 103}));
}

TEST_P(DmaChan, ManualBurstToRamFromDevice) {
    Rig r;
    int ch = GetParam();
    if (ch == 6) GTEST_SKIP();
    FakeDevice dev;
    r.dma.attach(static_cast<DmaChannel>(ch), &dev);
    r.enable_all();
    r.madr(ch, 0x3000);
    r.bcr(ch, 3);
    r.chcr(ch, 0x11000000u);
    EXPECT_EQ(r.peek(0x3000), 0x1000u);
    EXPECT_EQ(r.peek(0x3004), 0x1001u);
    EXPECT_EQ(r.peek(0x3008), 0x1002u);
    EXPECT_EQ(r.peek(0x300C), 0u);
}

TEST_P(DmaChan, CompletionRaisesIrq3WhenEnabled) {
    Rig r;
    int ch = GetParam();
    FakeDevice dev;
    if (ch != 6) r.dma.attach(static_cast<DmaChannel>(ch), &dev);
    r.enable_all();
    r.w32(kDicr, (1u << (16 + ch)) | (1u << 23));
    r.madr(ch, 0x4000);
    r.bcr(ch, 8);
    r.chcr(ch, ch == 6 ? 0x11000002u : 0x11000001u);
    EXPECT_EQ(r.irq.stat(), 0u);
    r.run_to(8);
    EXPECT_EQ(r.irq.stat(), 8u);
    EXPECT_NE(r.r32(kDicr) & (1u << (24 + ch)), 0u);
    EXPECT_NE(r.r32(kDicr) & 0x80000000u, 0u);
}

TEST_P(DmaChan, BusyClearsOnCompletion) {
    Rig r;
    int ch = GetParam();
    FakeDevice dev;
    if (ch != 6) r.dma.attach(static_cast<DmaChannel>(ch), &dev);
    r.enable_all();
    r.madr(ch, 0x4000);
    r.bcr(ch, 8);
    r.chcr(ch, ch == 6 ? 0x11000002u : 0x11000001u);
    EXPECT_NE(r.r32(Rig::ch_reg(ch, 2)) & Dma::kStart, 0u);
    r.run_to(7);
    EXPECT_NE(r.r32(Rig::ch_reg(ch, 2)) & Dma::kStart, 0u);
    r.run_to(8);
    EXPECT_EQ(r.r32(Rig::ch_reg(ch, 2)) & (Dma::kStart | Dma::kTrigger), 0u);
}

INSTANTIATE_TEST_SUITE_P(Channels, DmaChan, ::testing::Range(0, 7));

TEST(DmaRegs, ChcrWriteMaskChannel0to5) {
    Rig r;
    r.chcr(0, 0xFFFFFFFFu);  // disabled in DPCR: stays pending
    EXPECT_EQ(r.r32(Rig::ch_reg(0, 2)), 0x71770703u);
}

TEST(DmaRegs, OtcChcrMaskAndFixedStepBit) {
    Rig r;
    r.chcr(6, 0xFFFFFFFFu);
    EXPECT_EQ(r.r32(Rig::ch_reg(6, 2)), 0x51000002u);
    Rig r2;
    EXPECT_EQ(r2.r32(Rig::ch_reg(6, 2)), 0x00000002u);
}

TEST(DmaRegs, ChcrReadsBackSyncAndStep) {
    Rig r;
    r.chcr(2, 0x00000402u);
    EXPECT_EQ(r.r32(Rig::ch_reg(2, 2)), 0x00000402u);
}

TEST(DmaRegs, UnusedWordOfChannelReadsZero) {
    Rig r;
    r.w32(Rig::ch_reg(1, 3), 0xFFFFFFFFu);
    EXPECT_EQ(r.r32(Rig::ch_reg(1, 3)), 0u);
}

TEST(DmaRegs, MappedThroughKseg1) {
    Rig r;
    dw3_write32(&r.ctx(), 0xBF8010F0u, 0x1234567u);
    EXPECT_EQ(dw3_read32(&r.ctx(), 0xBF8010F0u), 0x1234567u);
}

// ---- DICR ----

TEST(DmaDicr, LowBitsReadWrite) {
    Rig r;
    r.w32(kDicr, 0x3Fu);
    EXPECT_EQ(r.r32(kDicr), 0x3Fu);
}

TEST(DmaDicr, WritableBitsOnly) {
    Rig r;
    r.w32(kDicr, 0x00FFFFFFu);  // includes bits 6-14 which are not backed
    EXPECT_EQ(r.r32(kDicr) & 0x00FFFFFFu, 0x00FF803Fu);
}

TEST(DmaDicr, ForceBitSetsMasterFlagAndRaisesIrq) {
    Rig r;
    r.w32(kDicr, 1u << 15);
    EXPECT_NE(r.r32(kDicr) & 0x80000000u, 0u);
    EXPECT_EQ(r.irq.stat(), 8u);
}

TEST(DmaDicr, Bit31NotWritable) {
    Rig r;
    r.w32(kDicr, 0x80000000u);
    EXPECT_EQ(r.r32(kDicr), 0u);
    EXPECT_EQ(r.irq.stat(), 0u);
}

TEST(DmaDicr, MasterFlagNeedsMasterEnable) {
    Rig r;
    r.dma.attach(DmaChannel::Gpu, nullptr);
    r.enable_all();
    r.w32(kDicr, 1u << 18);  // channel 2 enable only
    r.madr(2, 0x1000);
    r.bcr(2, 4);
    r.chcr(2, 0x11000001u);
    r.run_to(10);
    EXPECT_NE(r.r32(kDicr) & (1u << 26), 0u);  // flag set
    EXPECT_EQ(r.r32(kDicr) & 0x80000000u, 0u);  // but no master flag
    EXPECT_EQ(r.irq.stat(), 0u);
}

TEST(DmaDicr, FlagNotSetWithoutChannelEnable) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, 1u << 23);
    r.madr(2, 0x1000);
    r.bcr(2, 4);
    r.chcr(2, 0x11000001u);
    r.run_to(10);
    EXPECT_EQ(r.r32(kDicr) & 0x7F000000u, 0u);
    EXPECT_EQ(r.irq.stat(), 0u);
}

TEST(DmaDicr, FlagsClearByWritingOne) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (0x7Fu << 16) | (1u << 23));
    for (int ch : {1, 3}) {
        r.madr(ch, 0x1000);
        r.bcr(ch, 2);
        r.chcr(ch, 0x11000001u);
    }
    r.run_to(10);
    EXPECT_EQ(r.r32(kDicr) & 0x7F000000u, (1u << 25) | (1u << 27));
    r.w32(kDicr, (0x7Fu << 16) | (1u << 23) | (1u << 25));  // ack channel 1 only
    EXPECT_EQ(r.r32(kDicr) & 0x7F000000u, 1u << 27);
}

TEST(DmaDicr, WritingZeroKeepsFlags) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (1u << 16) | (1u << 23));
    r.madr(0, 0x1000);
    r.bcr(0, 2);
    r.chcr(0, 0x11000001u);
    r.run_to(10);
    r.w32(kDicr, (1u << 16) | (1u << 23));
    EXPECT_NE(r.r32(kDicr) & (1u << 24), 0u);
}

TEST(DmaDicr, MasterFlagDropsWhenAllFlagsAcked) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (1u << 16) | (1u << 23));
    r.madr(0, 0x1000);
    r.bcr(0, 2);
    r.chcr(0, 0x11000001u);
    r.run_to(10);
    EXPECT_NE(r.r32(kDicr) & 0x80000000u, 0u);
    r.w32(kDicr, (1u << 16) | (1u << 23) | (1u << 24));
    EXPECT_EQ(r.r32(kDicr) & 0x80000000u, 0u);
}

TEST(DmaDicr, MasterFlagStaysWhileForce) {
    Rig r;
    r.w32(kDicr, (1u << 15));
    r.w32(kStat, 0);
    r.w32(kDicr, (1u << 15) | (1u << 24));
    EXPECT_NE(r.r32(kDicr) & 0x80000000u, 0u);
    EXPECT_EQ(r.irq.stat(), 0u);  // already high: no new edge
}

TEST(DmaDicr, IrqOnlyOnZeroToOneTransition) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (0x7Fu << 16) | (1u << 23));
    r.madr(0, 0x1000);
    r.bcr(0, 2);
    r.chcr(0, 0x11000001u);
    r.run_to(10);
    EXPECT_EQ(r.irq.stat(), 8u);
    r.w32(kStat, 0);  // CPU acks the controller
    r.madr(1, 0x1000);
    r.bcr(1, 2);
    r.chcr(1, 0x11000001u);  // flag 0 still set: master flag already 1
    r.run_to(20);
    EXPECT_EQ(r.irq.stat(), 0u);
    EXPECT_NE(r.r32(kDicr) & (1u << 25), 0u);
    // Clear both flags (master drops), then a new completion raises again.
    r.w32(kDicr, (0x7Fu << 16) | (1u << 23) | (3u << 24));
    EXPECT_EQ(r.r32(kDicr) & 0x80000000u, 0u);
    r.madr(2, 0x1000);
    r.bcr(2, 2);
    r.chcr(2, 0x11000001u);
    r.run_to(30);
    EXPECT_EQ(r.irq.stat(), 8u);
}

TEST(DmaDicr, EnablingMasterWithPendingFlagRaises) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (1u << 16));  // channel enable but master disabled
    r.madr(0, 0x1000);
    r.bcr(0, 2);
    r.chcr(0, 0x11000001u);
    r.run_to(10);
    EXPECT_EQ(r.irq.stat(), 0u);
    r.w32(kDicr, (1u << 16) | (1u << 23));
    EXPECT_EQ(r.irq.stat(), 8u);
}

TEST(DmaDicr, SixteenBitWriteToUpperHalfAcksFlags) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (0x7Fu << 16) | (1u << 23));
    r.madr(0, 0x1000);
    r.bcr(0, 2);
    r.chcr(0, 0x11000001u);
    r.run_to(10);
    r.w16(kDicr + 2, 0x80FF | 0x0100);  // write 1 to flag bit 24 (and keep enables 16-23)
    EXPECT_EQ(r.r32(kDicr) & 0x7F000000u, 0u);
    EXPECT_EQ(r.r32(kDicr) & 0x00FF0000u, 0x00FF0000u & (0x7Fu << 16 | 1u << 23));
}

TEST(DmaDicr, SixteenBitWriteToLowHalfDoesNotAckFlags) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (1u << 16) | (1u << 23));
    r.madr(0, 0x1000);
    r.bcr(0, 2);
    r.chcr(0, 0x11000001u);
    r.run_to(10);
    r.w16(kDicr, 0xFFFF);
    EXPECT_NE(r.r32(kDicr) & (1u << 24), 0u);
}

TEST(DmaDicr, EightBitWriteAcksOnlyItsLane) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (0x7Fu << 16) | (1u << 23));
    r.madr(0, 0x1000);
    r.bcr(0, 2);
    r.chcr(0, 0x11000001u);
    r.madr(1, 0x1000);
    r.bcr(1, 2);
    r.chcr(1, 0x11000001u);
    r.run_to(10);
    r.w8(kDicr + 3, 0x01);  // ack flag bit 24 via byte lane 3
    EXPECT_EQ(r.r32(kDicr) & 0x7F000000u, 1u << 25);
}

// ---- OTC ----

TEST(DmaOtc, TableContentsMatchHardware) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x1000);
    r.bcr(6, 4);
    r.chcr(6, 0x11000002u);
    EXPECT_EQ(r.peek(0x1000), 0x0FFCu);
    EXPECT_EQ(r.peek(0x0FFC), 0x0FF8u);
    EXPECT_EQ(r.peek(0x0FF8), 0x0FF4u);
    EXPECT_EQ(r.peek(0x0FF4), 0x00FFFFFFu);
    EXPECT_EQ(r.peek(0x0FF0), 0u);
    EXPECT_EQ(r.peek(0x1004), 0u);
}

TEST(DmaOtc, SingleEntryIsJustEndMarker) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x2000);
    r.bcr(6, 1);
    r.chcr(6, 0x11000002u);
    EXPECT_EQ(r.peek(0x2000), 0x00FFFFFFu);
    EXPECT_EQ(r.peek(0x1FFC), 0u);
}

TEST(DmaOtc, ZeroCountMeans0x10000Entries) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x100000);
    r.bcr(6, 0);
    r.chcr(6, 0x11000002u);
    EXPECT_EQ(r.peek(0x100000), 0x0FFFFCu);
    EXPECT_EQ(r.peek(0x100000 - 0x3FFFC), 0x00FFFFFFu);
    EXPECT_EQ(r.peek(0x100000 - 0x40000), 0u);
}

TEST(DmaOtc, WrapsAroundRamTop) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x4);
    r.bcr(6, 3);
    r.chcr(6, 0x11000002u);
    EXPECT_EQ(r.peek(0x4), 0x0u);
    EXPECT_EQ(r.peek(0x0), 0xFFFFFCu);
    EXPECT_EQ(r.peek(0x1FFFFC), 0x00FFFFFFu);
}

TEST(DmaOtc, NeedsTriggerBit) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x1000);
    r.bcr(6, 4);
    r.chcr(6, 0x01000002u);  // start without trigger
    EXPECT_EQ(r.peek(0x1000), 0u);
    EXPECT_FALSE(r.dma.in_flight(DmaChannel::Otc));
    r.chcr(6, 0x11000002u);
    EXPECT_NE(r.peek(0x1000), 0u);
}

TEST(DmaOtc, MadrEndsBelowTable) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x1000);
    r.bcr(6, 4);
    r.chcr(6, 0x11000002u);
    EXPECT_EQ(r.dma.madr(DmaChannel::Otc), 0x0FF0u);
}

TEST(DmaOtc, ReportsRamWrittenRange) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x1000);
    r.bcr(6, 4);
    r.chcr(6, 0x11000002u);
    ASSERT_EQ(r.written.size(), 1u);
    EXPECT_EQ(r.written[0], std::make_pair(0x0FF4u, 16u));
}

TEST(DmaOtc, WrapReportsTwoRanges) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x4);
    r.bcr(6, 3);
    r.chcr(6, 0x11000002u);
    ASSERT_EQ(r.written.size(), 2u);
    EXPECT_EQ(r.written[0], std::make_pair(0x1FFFFCu, 4u));
    EXPECT_EQ(r.written[1], std::make_pair(0u, 8u));
}

TEST(DmaOtc, IgnoresKseg0AddressBits) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x80001000u);  // upper byte is dropped (24-bit register)
    r.bcr(6, 2);
    r.chcr(6, 0x11000002u);
    EXPECT_EQ(r.peek(0x1000), 0x0FFCu);
    EXPECT_EQ(r.peek(0x0FFC), 0x00FFFFFFu);
}

TEST(DmaOtc, OrderingTableWalksBackToEndMarker) {
    Rig r;
    r.enable(6);
    r.madr(6, 0x8000);
    r.bcr(6, 64);
    r.chcr(6, 0x11000002u);
    uint32_t a = 0x8000;
    int hops = 0;
    while (!(r.peek(a) & 0x800000u) && hops < 100) {
        a = r.peek(a);
        ++hops;
    }
    EXPECT_EQ(hops, 63);
}

// ---- sync mode 0 ----

TEST(DmaSync0, StartNeedsTriggerBit) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x1000);
    r.bcr(4, 4);
    r.chcr(4, 0x01000001u);
    EXPECT_TRUE(dev.writes.empty());
    EXPECT_TRUE(r.dma.busy(DmaChannel::Spu));
    r.chcr(4, 0x11000001u);
    EXPECT_EQ(dev.writes.size(), 1u);
}

TEST(DmaSync0, ZeroBcrMeans0x10000Words) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x1000);
    r.bcr(4, 0);
    r.chcr(4, 0x11000001u);
    ASSERT_EQ(dev.writes.size(), 1u);
    EXPECT_EQ(dev.writes[0].size(), 0x10000u);
}

TEST(DmaSync0, BcrUpperHalfIgnored) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x1000);
    r.bcr(4, 0x00050003u);
    r.chcr(4, 0x11000001u);
    EXPECT_EQ(dev.writes[0].size(), 3u);
}

TEST(DmaSync0, DecrementingStepReadsDownwards) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    for (int i = 0; i < 4; ++i) r.poke(0x2000 + 4 * i, 10 + i);
    r.madr(4, 0x200C);
    r.bcr(4, 4);
    r.chcr(4, 0x11000003u);  // from RAM, step -4
    EXPECT_EQ(dev.all_written(), (std::vector<uint32_t>{13, 12, 11, 10}));
    EXPECT_EQ(r.dma.madr(DmaChannel::Spu), 0x1FFCu);
}

TEST(DmaSync0, IncrementingStepAdvancesMadr) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x2000);
    r.bcr(4, 4);
    r.chcr(4, 0x11000001u);
    EXPECT_EQ(r.dma.madr(DmaChannel::Spu), 0x2010u);
}

TEST(DmaSync0, ToRamReportsRange) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Cdrom, &dev);
    r.enable_all();
    r.madr(3, 0x5000);
    r.bcr(3, 8);
    r.chcr(3, 0x11000000u);
    ASSERT_EQ(r.written.size(), 1u);
    EXPECT_EQ(r.written[0], std::make_pair(0x5000u, 32u));
}

TEST(DmaSync0, ToRamDecrementingReportsLowRange) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Cdrom, &dev);
    r.enable_all();
    r.madr(3, 0x5010);
    r.bcr(3, 5);
    r.chcr(3, 0x11000002u);  // to RAM, step -4
    ASSERT_EQ(r.written.size(), 1u);
    EXPECT_EQ(r.written[0], std::make_pair(0x5000u, 20u));
    EXPECT_EQ(r.peek(0x5010), 0x1000u);  // first word lands at MADR
    EXPECT_EQ(r.peek(0x5000), 0x1004u);
}

TEST(DmaSync0, FromRamDoesNotReportRamWritten) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x1000);
    r.bcr(4, 4);
    r.chcr(4, 0x11000001u);
    EXPECT_TRUE(r.written.empty());
}

TEST(DmaSync0, ToRamWrapsAtRamEnd) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Cdrom, &dev);
    r.enable_all();
    r.madr(3, 0x1FFFF8);
    r.bcr(3, 4);
    r.chcr(3, 0x11000000u);
    EXPECT_EQ(r.peek(0x1FFFF8), 0x1000u);
    EXPECT_EQ(r.peek(0x1FFFFC), 0x1001u);
    EXPECT_EQ(r.peek(0x0), 0x1002u);
    EXPECT_EQ(r.peek(0x4), 0x1003u);
    ASSERT_EQ(r.written.size(), 2u);
    EXPECT_EQ(r.written[0], std::make_pair(0x1FFFF8u, 8u));
    EXPECT_EQ(r.written[1], std::make_pair(0u, 8u));
}

TEST(DmaSync0, NoDeviceFromRamIsDiscarded) {
    Rig r;
    r.enable_all();
    r.madr(5, 0x1000);
    r.bcr(5, 4);
    r.chcr(5, 0x11000001u);
    EXPECT_TRUE(r.dma.in_flight(DmaChannel::Pio));
    r.run_to(4);
    EXPECT_FALSE(r.dma.busy(DmaChannel::Pio));
}

TEST(DmaSync0, NoDeviceToRamZeroFills) {
    Rig r;
    r.enable_all();
    r.poke(0x1000, 0xDEADBEEF);
    r.madr(5, 0x1000);
    r.bcr(5, 2);
    r.chcr(5, 0x11000000u);
    EXPECT_EQ(r.peek(0x1000), 0u);
}

TEST(DmaSync0, DeviceNotReadyDefers) {
    Rig r;
    FakeDevice dev;
    dev.ready = false;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    r.madr(2, 0x1000);
    r.bcr(2, 4);
    r.chcr(2, 0x11000001u);
    EXPECT_TRUE(dev.writes.empty());
    EXPECT_TRUE(r.dma.busy(DmaChannel::Gpu));
    dev.ready = true;
    r.dma.device_ready(DmaChannel::Gpu);
    EXPECT_EQ(dev.writes.size(), 1u);
    EXPECT_TRUE(r.dma.in_flight(DmaChannel::Gpu));
}

TEST(DmaSync0, DeviceReadyWithoutPendingTransferDoesNothing) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    r.dma.device_ready(DmaChannel::Gpu);
    EXPECT_TRUE(dev.writes.empty());
}

TEST(DmaSync0, DeviceReadyDirectionPassed) {
    struct DirDev : DmaDevice {
        mutable std::vector<bool> asked;
        bool dma_ready(bool to_ram) const override {
            asked.push_back(to_ram);
            return true;
        }
    } dev;
    Rig r;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x1000);
    r.bcr(4, 1);
    r.chcr(4, 0x11000000u);
    r.run_to(5);
    r.chcr(4, 0x11000001u);
    EXPECT_EQ(dev.asked, (std::vector<bool>{true, false}));
}

// ---- sync mode 1 ----

TEST(DmaSync1, BlocksArriveAsSeparateWrites) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::MdecIn, &dev);
    r.enable_all();
    for (int i = 0; i < 12; ++i) r.poke(0x1000 + 4 * i, i);
    r.madr(0, 0x1000);
    r.bcr(0, (3u << 16) | 4u);
    r.chcr(0, 0x01000201u);  // sync 1, from RAM; no trigger needed
    ASSERT_EQ(dev.writes.size(), 3u);
    EXPECT_EQ(dev.writes[0], (std::vector<uint32_t>{0, 1, 2, 3}));
    EXPECT_EQ(dev.writes[1], (std::vector<uint32_t>{4, 5, 6, 7}));
    EXPECT_EQ(dev.writes[2], (std::vector<uint32_t>{8, 9, 10, 11}));
}

TEST(DmaSync1, BcrBlockCountCountsDownToZero) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::MdecIn, &dev);
    r.enable_all();
    r.madr(0, 0x1000);
    r.bcr(0, (3u << 16) | 4u);
    r.chcr(0, 0x01000201u);
    EXPECT_EQ(r.dma.bcr(DmaChannel::MdecIn), 4u);
}

TEST(DmaSync1, MadrAdvancesPastAllBlocks) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::MdecIn, &dev);
    r.enable_all();
    r.madr(0, 0x1000);
    r.bcr(0, (3u << 16) | 4u);
    r.chcr(0, 0x01000201u);
    EXPECT_EQ(r.dma.madr(DmaChannel::MdecIn), 0x1000u + 48u);
}

TEST(DmaSync1, ToRamReadsPerBlock) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::MdecOut, &dev);
    r.enable_all();
    r.madr(1, 0x6000);
    r.bcr(1, (2u << 16) | 8u);
    r.chcr(1, 0x01000200u);
    EXPECT_EQ(dev.read_sizes, (std::vector<size_t>{8, 8}));
    EXPECT_EQ(r.peek(0x6000), 0x1000u);
    EXPECT_EQ(r.peek(0x603C), 0x100Fu);
    ASSERT_EQ(r.written.size(), 2u);
    EXPECT_EQ(r.written[0], std::make_pair(0x6000u, 32u));
    EXPECT_EQ(r.written[1], std::make_pair(0x6020u, 32u));
}

TEST(DmaSync1, ZeroBlockSizeMeans0x10000) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::MdecIn, &dev);
    r.enable_all();
    r.madr(0, 0);
    r.bcr(0, 1u << 16);
    r.chcr(0, 0x01000201u);
    ASSERT_EQ(dev.writes.size(), 1u);
    EXPECT_EQ(dev.writes[0].size(), 0x10000u);
}

TEST(DmaSync1, CompletionCostIsTotalWords) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::MdecIn, &dev);
    r.enable_all();
    r.w32(kDicr, (1u << 16) | (1u << 23));
    r.madr(0, 0x1000);
    r.bcr(0, (3u << 16) | 4u);
    r.chcr(0, 0x01000201u);
    r.run_to(11);
    EXPECT_EQ(r.irq.stat(), 0u);
    r.run_to(12);
    EXPECT_EQ(r.irq.stat(), 8u);
}

// ---- sync mode 2 (linked list) ----

namespace {
void make_list(Rig& r) {
    r.poke(0x1000, (2u << 24) | 0x1100u);
    r.poke(0x1004, 0xAAAA0001u);
    r.poke(0x1008, 0xAAAA0002u);
    r.poke(0x1100, (0u << 24) | 0x1200u);
    r.poke(0x1200, (1u << 24) | 0x00FFFFFFu);
    r.poke(0x1204, 0xBBBB0001u);
}
}  // namespace

TEST(DmaSync2, WalksListAndSendsPayloads) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    make_list(r);
    r.madr(2, 0x1000);
    r.chcr(2, 0x01000401u);
    ASSERT_EQ(dev.writes.size(), 2u);  // the empty node sends nothing
    EXPECT_EQ(dev.writes[0], (std::vector<uint32_t>{0xAAAA0001u, 0xAAAA0002u}));
    EXPECT_EQ(dev.writes[1], (std::vector<uint32_t>{0xBBBB0001u}));
}

TEST(DmaSync2, MadrEndsAtEndMarker) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    make_list(r);
    r.madr(2, 0x1000);
    r.chcr(2, 0x01000401u);
    EXPECT_EQ(r.dma.madr(DmaChannel::Gpu), 0x00FFFFFFu);
}

TEST(DmaSync2, NeedsNoTriggerBit) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    make_list(r);
    r.madr(2, 0x1000);
    r.chcr(2, 0x01000401u);
    EXPECT_FALSE(dev.writes.empty());
}

TEST(DmaSync2, SingleNodeEndMarkerOnly) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    r.poke(0x2000, (3u << 24) | 0x00FFFFFFu);
    r.poke(0x2004, 1);
    r.poke(0x2008, 2);
    r.poke(0x200C, 3);
    r.madr(2, 0x2000);
    r.chcr(2, 0x01000401u);
    ASSERT_EQ(dev.writes.size(), 1u);
    EXPECT_EQ(dev.writes[0], (std::vector<uint32_t>{1, 2, 3}));
}

TEST(DmaSync2, AnyNextWithBit23EndsList) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    r.poke(0x2000, (1u << 24) | 0x00800000u);
    r.poke(0x2004, 7);
    r.madr(2, 0x2000);
    r.chcr(2, 0x01000401u);
    EXPECT_EQ(dev.all_written(), (std::vector<uint32_t>{7}));
    EXPECT_EQ(r.dma.loop_guard_trips(), 0u);
}

TEST(DmaSync2, OtcTableIsAValidList) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    r.madr(6, 0x3000);
    r.bcr(6, 3);
    r.chcr(6, 0x11000002u);
    r.run_to(10);
    r.poke(0x3000, (1u << 24) | r.peek(0x3000));  // node at 0x3000 carries one payload word
    r.poke(0x3004, 0x77);
    r.madr(2, 0x3000);
    r.chcr(2, 0x01000401u);
    ASSERT_EQ(dev.writes.size(), 1u);
    EXPECT_EQ(dev.writes[0], (std::vector<uint32_t>{0x77}));
    EXPECT_EQ(r.dma.madr(DmaChannel::Gpu), 0x00FFFFFFu);
    r.run_to(100);
    EXPECT_FALSE(r.dma.busy(DmaChannel::Gpu));
}

TEST(DmaSync2, SelfLoopIsGuarded) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    r.poke(0x2000, 0x2000);  // count 0, next = itself
    r.madr(2, 0x2000);
    r.chcr(2, 0x01000401u);
    EXPECT_EQ(r.dma.loop_guard_trips(), 1u);
    ASSERT_EQ(r.errors.size(), 1u);
    EXPECT_NE(r.errors[0].find("limit"), std::string::npos);
    EXPECT_TRUE(r.dma.in_flight(DmaChannel::Gpu));  // still completes normally
}

TEST(DmaSync2, TwoNodeLoopIsGuarded) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    r.poke(0x2000, 0x2100);
    r.poke(0x2100, 0x2000);
    r.madr(2, 0x2000);
    r.chcr(2, 0x01000401u);
    EXPECT_EQ(r.dma.loop_guard_trips(), 1u);
}

TEST(DmaSync2, LoopGuardTransferStillCompletesAndRaisesIrq) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (1u << 18) | (1u << 23));
    r.poke(0x2000, 0x2000);
    r.madr(2, 0x2000);
    r.chcr(2, 0x01000401u);
    r.run_to(Dma::kMaxLinkedNodes);
    EXPECT_EQ(r.irq.stat(), 8u);
    EXPECT_FALSE(r.dma.busy(DmaChannel::Gpu));
}

TEST(DmaSync2, LongLegalListBelowCapIsNotGuarded) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    const uint32_t kNodes = 50000;
    for (uint32_t i = 0; i < kNodes; ++i)
        r.poke(0x10000 + 8 * i, (1u << 24) | (i + 1 == kNodes ? 0xFFFFFFu : 0x10000 + 8 * (i + 1)));
    r.madr(2, 0x10000);
    r.chcr(2, 0x01000401u);
    EXPECT_EQ(r.dma.loop_guard_trips(), 0u);
    EXPECT_EQ(dev.writes.size(), kNodes);
}

TEST(DmaSync2, ToRamDirectionRejected) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    make_list(r);
    r.madr(2, 0x1000);
    r.chcr(2, 0x01000400u);
    EXPECT_TRUE(dev.writes.empty());
    ASSERT_EQ(r.errors.size(), 1u);
}

TEST(DmaSync2, DoesNotReportRamWritten) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    make_list(r);
    r.madr(2, 0x1000);
    r.chcr(2, 0x01000401u);
    EXPECT_TRUE(r.written.empty());
}

TEST(DmaSync2, CostCountsNodesAndWords) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Gpu, &dev);
    r.enable_all();
    r.w32(kDicr, (1u << 18) | (1u << 23));
    make_list(r);  // 3 nodes, 3 payload words
    r.madr(2, 0x1000);
    r.chcr(2, 0x01000401u);
    r.run_to(5);
    EXPECT_EQ(r.irq.stat(), 0u);
    r.run_to(6);
    EXPECT_EQ(r.irq.stat(), 8u);
}

TEST(DmaSync3, ReservedModeReportsError) {
    Rig r;
    r.enable_all();
    r.madr(2, 0x1000);
    r.chcr(2, 0x01000601u);
    EXPECT_EQ(r.errors.size(), 1u);
}

// ---- completion, timing, abort ----

TEST(DmaTiming, CostIsWordsTimesDeviceRate) {
    Rig r;
    FakeDevice dev;
    dev.cpw = 4;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.w32(kDicr, (1u << 20) | (1u << 23));
    r.madr(4, 0x1000);
    r.bcr(4, 10);
    r.chcr(4, 0x11000001u);
    r.run_to(39);
    EXPECT_EQ(r.irq.stat(), 0u);
    r.run_to(40);
    EXPECT_EQ(r.irq.stat(), 8u);
}

TEST(DmaTiming, CompletionIsRelativeToStartCycle) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (1u << 21) | (1u << 23));
    r.run_to(1000);
    r.madr(5, 0x1000);
    r.bcr(5, 16);
    r.chcr(5, 0x11000001u);
    EXPECT_EQ(r.ctx().next_event, 1016);
}

TEST(DmaTiming, MinimumCostIsOneCycle) {
    EXPECT_EQ(Dma::transfer_cycles(0, 0, 1), 1);
    EXPECT_EQ(Dma::transfer_cycles(1, 0, 1), 1);
    EXPECT_EQ(Dma::transfer_cycles(10, 3, 2), 23);
}

TEST(DmaTiming, DataMovesImmediatelyBusyClearsLater) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Cdrom, &dev);
    r.enable_all();
    r.madr(3, 0x1000);
    r.bcr(3, 100);
    r.chcr(3, 0x11000000u);
    EXPECT_EQ(r.peek(0x1000), 0x1000u);
    EXPECT_TRUE(r.dma.busy(DmaChannel::Cdrom));
    r.run_to(100);
    EXPECT_FALSE(r.dma.busy(DmaChannel::Cdrom));
}

TEST(DmaTiming, AbortCancelsCompletionAndIrq) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (1u << 21) | (1u << 23));
    r.madr(5, 0x1000);
    r.bcr(5, 100);
    r.chcr(5, 0x11000001u);
    r.run_to(10);
    r.chcr(5, 0);
    EXPECT_FALSE(r.dma.busy(DmaChannel::Pio));
    EXPECT_FALSE(r.dma.in_flight(DmaChannel::Pio));
    r.run_to(1000);
    EXPECT_EQ(r.irq.stat(), 0u);
    EXPECT_EQ(r.r32(kDicr) & 0x7F000000u, 0u);
}

TEST(DmaTiming, RestartWhileRunningIgnored) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x1000);
    r.bcr(4, 10);
    r.chcr(4, 0x11000001u);
    r.chcr(4, 0x11000001u);
    EXPECT_EQ(dev.writes.size(), 1u);
}

TEST(DmaTiming, CanRestartAfterCompletion) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x1000);
    r.bcr(4, 10);
    r.chcr(4, 0x11000001u);
    r.run_to(10);
    r.madr(4, 0x1000);
    r.chcr(4, 0x11000001u);
    EXPECT_EQ(dev.writes.size(), 2u);
}

TEST(DmaTiming, OneEventPerRunningChannel) {
    Rig r;
    r.enable_all();
    for (int ch : {1, 3, 5}) {
        r.madr(ch, 0x1000);
        r.bcr(ch, 4 * (ch + 1));
        r.chcr(ch, 0x11000001u);
    }
    EXPECT_EQ(r.sched.pending_count(), 3u);
    r.run_to(8);
    EXPECT_EQ(r.sched.pending_count(), 2u);
}

TEST(DmaTiming, ChannelsCompleteIndependently) {
    Rig r;
    r.enable_all();
    r.w32(kDicr, (0x7Fu << 16) | (1u << 23));
    for (int ch : {1, 3}) {
        r.madr(ch, 0x1000);
        r.bcr(ch, ch == 1 ? 5 : 20);
        r.chcr(ch, 0x11000001u);
    }
    r.run_to(5);
    EXPECT_EQ(r.r32(kDicr) & 0x7F000000u, 1u << 25);
    r.run_to(20);
    EXPECT_EQ(r.r32(kDicr) & 0x7F000000u, (1u << 25) | (1u << 27));
}

TEST(DmaTiming, DpcrEnableAfterBusyStartsTransfer) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Cdrom, &dev);
    r.madr(3, 0x1000);
    r.bcr(3, 4);
    r.chcr(3, 0x11000000u);
    EXPECT_EQ(r.peek(0x1000), 0u);
    r.w32(kDpcr, r.r32(kDpcr) | (1u << 15));
    EXPECT_EQ(r.peek(0x1000), 0x1000u);
}

TEST(DmaTiming, DpcrDisableDoesNotAbortRunning) {
    Rig r;
    r.enable_all();
    r.madr(5, 0x1000);
    r.bcr(5, 10);
    r.chcr(5, 0x11000001u);
    r.w32(kDpcr, 0);
    EXPECT_TRUE(r.dma.in_flight(DmaChannel::Pio));
}

TEST(DmaTiming, DpcrBitsForOtherChannelsDontEnable) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.w32(kDpcr, 8u << 12);  // enable channel 3 only
    r.madr(4, 0x1000);
    r.bcr(4, 4);
    r.chcr(4, 0x11000001u);
    EXPECT_TRUE(dev.writes.empty());
}

TEST(DmaTiming, WritingChcrWithoutStartBitDoesNothing) {
    Rig r;
    FakeDevice dev;
    r.dma.attach(DmaChannel::Spu, &dev);
    r.enable_all();
    r.madr(4, 0x1000);
    r.bcr(4, 4);
    r.chcr(4, 0x10000001u);
    EXPECT_TRUE(dev.writes.empty());
    EXPECT_EQ(r.sched.pending_count(), 0u);
}
