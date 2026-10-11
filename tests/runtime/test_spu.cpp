// SPU register-level model: SPUSTAT/SPUCNT handshake, transfer FIFO, DMA channel 4, IRQ9, key on/off.

#include <gtest/gtest.h>

#include "dw3/bus.h"
#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"
#include "dw3/spu.h"

using namespace dw3;

namespace {

struct Rig {
    Bus bus;
    Scheduler sched{bus.ctx()};
    IrqController irq{bus};
    Dma dma{bus, sched, irq};
    Spu spu{bus, irq, dma};

    void w16(uint32_t off, uint16_t v) { bus.write(Spu::kBase + off, v, 2); }
    uint16_t r16(uint32_t off) { return static_cast<uint16_t>(bus.read(Spu::kBase + off, 2)); }
    void run_dma() {
        bus.ctx().cycles = bus.ctx().next_event == INT64_MAX ? bus.ctx().cycles : bus.ctx().next_event;
        sched.run_due();
    }
};

}  // namespace

TEST(Spu, RegistersReadBack) {
    Rig r;
    r.w16(0x180, 0x3FFF);  // main volume L
    r.w16(0x000, 0x1234);  // voice 0 volume L
    EXPECT_EQ(r.r16(0x180), 0x3FFF);
    EXPECT_EQ(r.r16(0x000), 0x1234);
}

TEST(Spu, StatMirrorsControlLowBits) {
    Rig r;
    for (uint16_t v : {0x0000, 0x0001, 0x0015, 0x003F, 0x8000 | 0x002A}) {
        r.w16(Spu::kSpuCnt, v);
        EXPECT_EQ(r.r16(Spu::kSpuStat) & 0x3F, v & 0x3F) << v;
    }
}

TEST(Spu, StatTransferModeBits) {
    Rig r;
    r.w16(Spu::kSpuCnt, 1u << 4);  // manual write
    EXPECT_EQ(r.r16(Spu::kSpuStat) & 0x380, 0);
    r.w16(Spu::kSpuCnt, 2u << 4);  // DMA write
    EXPECT_EQ(r.r16(Spu::kSpuStat) & 0x380, 0x180);
    r.w16(Spu::kSpuCnt, 3u << 4);  // DMA read
    EXPECT_EQ(r.r16(Spu::kSpuStat) & 0x380, 0x280);
    r.w16(Spu::kSpuCnt, 0);
    EXPECT_EQ(r.r16(Spu::kSpuStat) & 0x380, 0);
}

TEST(Spu, TransferAddressIsInEightByteUnits) {
    Rig r;
    r.w16(Spu::kTransferAddr, 0x0200);
    EXPECT_EQ(r.spu.transfer_address(), 0x1000u);
}

TEST(Spu, ManualFifoWritesRamAndAdvances) {
    Rig r;
    r.w16(Spu::kTransferAddr, 0x0010);  // byte 0x80
    r.w16(Spu::kTransferFifo, 0xBEEF);
    r.w16(Spu::kTransferFifo, 0x1234);
    auto ram = r.spu.ram();
    EXPECT_EQ(ram[0x80], 0xEF);
    EXPECT_EQ(ram[0x81], 0xBE);
    EXPECT_EQ(ram[0x82], 0x34);
    EXPECT_EQ(ram[0x83], 0x12);
    EXPECT_EQ(r.spu.transfer_address(), 0x84u);
}

TEST(Spu, ManualFifoReadBack) {
    Rig r;
    r.w16(Spu::kTransferAddr, 0x0001);
    r.w16(Spu::kTransferFifo, 0xCAFE);
    r.w16(Spu::kTransferAddr, 0x0001);
    EXPECT_EQ(r.r16(Spu::kTransferFifo), 0xCAFE);
}

TEST(Spu, TransferAddressWrapsAtEndOfRam) {
    Rig r;
    r.w16(Spu::kTransferAddr, 0xFFFF);  // byte 0x7FFF8
    for (int i = 0; i < 4; ++i) r.w16(Spu::kTransferFifo, 0x1111);
    EXPECT_EQ(r.spu.transfer_address(), 0u);
    r.w16(Spu::kTransferFifo, 0x2222);
    EXPECT_EQ(r.spu.ram()[0], 0x22);
}

TEST(Spu, DmaWriteToSpuRam) {
    Rig r;
    uint32_t src = 0x1000;
    for (uint32_t i = 0; i < 16; ++i) dw3_write32(&r.bus.ctx(), 0x80000000 | (src + i * 4), 0x01020304u * (i + 1));
    r.w16(Spu::kTransferAddr, 0x0040);  // byte 0x200
    r.w16(Spu::kSpuCnt, 2u << 4);
    r.bus.write(0x1F8010F0, 1u << 19, 4);  // DPCR: enable channel 4
    r.bus.write(0x1F8010C0, src, 4);
    r.bus.write(0x1F8010C4, (1u << 16) | 16, 4);  // 1 block of 16 words
    r.bus.write(0x1F8010C8, 0x01000201, 4);       // from RAM, sync mode 1, start
    r.run_dma();
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t w = 0;
        for (int b = 0; b < 4; ++b) w |= static_cast<uint32_t>(r.spu.ram()[0x200 + i * 4 + b]) << (8 * b);
        EXPECT_EQ(w, 0x01020304u * (i + 1)) << i;
    }
}

TEST(Spu, DmaReadFromSpuRam) {
    Rig r;
    r.w16(Spu::kTransferAddr, 0x0000);
    for (uint16_t i = 0; i < 8; ++i) r.w16(Spu::kTransferFifo, static_cast<uint16_t>(0xA000 + i));
    r.w16(Spu::kTransferAddr, 0x0000);
    r.w16(Spu::kSpuCnt, 3u << 4);
    r.bus.write(0x1F8010F0, 1u << 19, 4);
    r.bus.write(0x1F8010C0, 0x2000, 4);
    r.bus.write(0x1F8010C4, (1u << 16) | 4, 4);
    r.bus.write(0x1F8010C8, 0x01000200, 4);  // to RAM, sync mode 1, start
    r.run_dma();
    EXPECT_EQ(dw3_read32(&r.bus.ctx(), 0x80002000), 0xA001A000u);
    EXPECT_EQ(dw3_read32(&r.bus.ctx(), 0x8000200C), 0xA007A006u);
}

TEST(Spu, Irq9OnTransferAtIrqAddress) {
    Rig r;
    r.bus.write(0x1F801074, 1u << 9, 4);  // I_MASK: SPU
    r.w16(Spu::kIrqAddr, 0x0002);         // byte 0x10
    r.w16(Spu::kSpuCnt, 0x8040);          // enable, IRQ9 enable
    r.w16(Spu::kTransferAddr, 0x0002);
    r.w16(Spu::kTransferFifo, 1);
    EXPECT_TRUE(r.r16(Spu::kSpuStat) & 0x40);
    EXPECT_TRUE(r.irq.stat() & (1u << 9));
}

TEST(Spu, Irq9NotRaisedWhenDisabled) {
    Rig r;
    r.w16(Spu::kIrqAddr, 0x0002);
    r.w16(Spu::kSpuCnt, 0x8000);
    r.w16(Spu::kTransferAddr, 0x0002);
    r.w16(Spu::kTransferFifo, 1);
    EXPECT_FALSE(r.r16(Spu::kSpuStat) & 0x40);
    EXPECT_FALSE(r.irq.stat() & (1u << 9));
}

TEST(Spu, Irq9AckByClearingEnable) {
    Rig r;
    r.w16(Spu::kIrqAddr, 0x0000);
    r.w16(Spu::kSpuCnt, 0x8040);
    r.w16(Spu::kTransferAddr, 0x0000);
    r.w16(Spu::kTransferFifo, 1);
    ASSERT_TRUE(r.r16(Spu::kSpuStat) & 0x40);
    r.w16(Spu::kSpuCnt, 0x8000);
    EXPECT_FALSE(r.r16(Spu::kSpuStat) & 0x40);
}

TEST(Spu, Irq9OnlyOnceUntilAcked) {
    Rig r;
    r.bus.write(0x1F801074, 1u << 9, 4);
    r.w16(Spu::kIrqAddr, 0x0000);
    r.w16(Spu::kSpuCnt, 0x8040);
    r.w16(Spu::kTransferAddr, 0x0000);
    r.w16(Spu::kTransferFifo, 1);
    r.bus.write(0x1F801070, ~(1u << 9), 4);  // ack I_STAT
    r.w16(Spu::kTransferAddr, 0x0000);
    r.w16(Spu::kTransferFifo, 1);
    EXPECT_FALSE(r.irq.stat() & (1u << 9));
}

TEST(Spu, KeyOnOffTracksVoices) {
    Rig r;
    r.w16(Spu::kKeyOn, 0x0005);
    r.w16(Spu::kKeyOn + 2, 0x0080);
    EXPECT_EQ(r.spu.voices_on(), 0x00800005u);
    r.w16(Spu::kKeyOff, 0x0001);
    EXPECT_EQ(r.spu.voices_on(), 0x00800004u);
    r.w16(Spu::kKeyOff + 2, 0x0080);
    EXPECT_EQ(r.spu.voices_on(), 0x00000004u);
}

TEST(Spu, EndxReportsVoicesNotPlaying) {
    Rig r;
    r.w16(Spu::kKeyOn, 0x0003);
    EXPECT_EQ(r.r16(Spu::kEndx), 0xFFFC);
    EXPECT_EQ(r.r16(Spu::kEndx + 2), 0x00FF);
}

TEST(Spu, ThirtyTwoBitAccessSplitsIntoHalves) {
    Rig r;
    r.bus.write(Spu::kBase + 0x180, 0x11112222u, 4);
    EXPECT_EQ(r.r16(0x180), 0x2222);
    EXPECT_EQ(r.r16(0x182), 0x1111);
    EXPECT_EQ(r.bus.read(Spu::kBase + 0x180, 4), 0x11112222u);
}

TEST(Spu, ByteReads) {
    Rig r;
    r.w16(0x180, 0xAB12);
    EXPECT_EQ(r.bus.read(Spu::kBase + 0x180, 1), 0x12u);
    EXPECT_EQ(r.bus.read(Spu::kBase + 0x181, 1), 0xABu);
}

TEST(Spu, MapsWholeRegisterWindow) {
    Rig r;
    r.w16(0x3FE, 0x7777);
    EXPECT_EQ(r.r16(0x3FE), 0x7777);
    EXPECT_EQ(r.bus.unmapped_writes(), 0u);
}
