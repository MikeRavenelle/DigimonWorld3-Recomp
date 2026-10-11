// SPU save-state round trip: a loaded instance must continue sample-for-sample like the original.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "dw3/bus.h"
#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"
#include "dw3/spu.h"
#include "dw3/state.h"

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

    // Advances emulated time, running scheduler events (SPU batches) on the way.
    void run_cycles(int64_t n) {
        int64_t target = bus.ctx().cycles + n;
        while (bus.ctx().next_event <= target) {
            bus.ctx().cycles = bus.ctx().next_event;
            sched.run_due();
        }
        bus.ctx().cycles = target;
    }

    std::vector<uint8_t> save() {
        StateIo s;
        sched.io(s);
        spu.io(s);
        return s.take();
    }
    void load(const std::vector<uint8_t>& data) {
        StateIo s{std::span<const uint8_t>(data)};
        sched.io(s);
        spu.io(s);
    }
};

// Fills a looping ADPCM sample (several blocks of varied data) at `addr`.
void put_sample(Rig& r, uint32_t addr, uint8_t seed) {
    auto ram = r.spu.ram_mut();
    for (uint32_t b = 0; b < 4; ++b) {
        uint8_t* p = &ram[addr + b * 16];
        p[0] = static_cast<uint8_t>(0x04 + b);  // shift 4.., filter 0
        p[1] = b == 0 ? 4 : b == 3 ? 3 : 0;     // loop start / loop end + repeat
        for (uint32_t i = 2; i < 16; ++i) p[i] = static_cast<uint8_t>(seed * 37 + b * 11 + i * 53);
    }
}

void program(Rig& r) {
    r.spu.start(r.sched);
    r.w16(Spu::kSpuCnt, 0xC080);  // enabled, unmuted, reverb writes on
    r.w16(Spu::kMainVolL, 0x3FFF);
    r.w16(Spu::kMainVolR, 0x3FFF);
    r.w16(Spu::kReverbVolL, 0x3000);
    r.w16(Spu::kReverbVolR, 0x3000);
    r.w16(Spu::kReverbStart, 0x7000 / 8);
    const uint16_t rv[32] = {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB,
                             0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0,
                             0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000};
    for (uint32_t i = 0; i < 32; ++i) r.w16(Spu::kReverbRegs + i * 2, rv[i]);
    for (uint32_t v = 0; v < 6; ++v) {
        uint32_t addr = 0x1000 + v * 0x100;
        put_sample(r, addr, static_cast<uint8_t>(v + 1));
        uint32_t base = v * 16;
        r.w16(base + 0x0, v == 4 ? 0x8200 : 0x1FFF);  // voice 4: sweep volume
        r.w16(base + 0x2, 0x1FFF);
        r.w16(base + 0x4, static_cast<uint16_t>(0x0400 + v * 0x180));
        r.w16(base + 0x6, static_cast<uint16_t>(addr / 8));
        r.w16(base + 0x8, static_cast<uint16_t>(0x80FF | (v << 4)));
        r.w16(base + 0xA, 0x5FC0);
    }
    r.w16(Spu::kReverbOn, 0x0015);
    r.w16(Spu::kNoiseOn, 0x0008);
    r.w16(Spu::kPitchMod, 0x0004);
    r.w16(Spu::kKeyOn, 0x003F);
}

std::vector<int16_t> drain(Rig& r) {
    std::vector<int16_t> out(r.spu.audio().size() * 2);
    r.spu.drain_samples(out);
    return out;
}

}  // namespace

TEST(StateSpu, RoundTripContinuesIdentically) {
    Rig a;
    program(a);
    a.run_cycles(768 * 256 * 5 + 300);  // stop mid-batch
    a.w16(Spu::kKeyOff, 0x0002);
    a.run_cycles(768 * 100);
    drain(a);

    auto saved = a.save();

    Rig b;
    b.spu.start(b.sched);
    b.bus.ctx().cycles = a.bus.ctx().cycles;
    b.spu.audio().push(1, 2);  // stale queued audio must be discarded
    b.load(saved);
    EXPECT_EQ(b.spu.audio().size(), 0u);
    EXPECT_EQ(b.save(), saved);

    // About 300 ms of emulated time.
    for (int i = 0; i < 12; ++i) {
        a.run_cycles(768 * 256);
        b.run_cycles(768 * 256);
        ASSERT_EQ(drain(a), drain(b)) << "step " << i;
    }
    EXPECT_EQ(a.spu.frames_generated(), b.spu.frames_generated());
    for (uint32_t off = 0; off < Spu::kSize; off += 2) ASSERT_EQ(a.r16(off), b.r16(off)) << std::hex << off;
    EXPECT_EQ(a.save(), b.save());
}

TEST(StateSpu, AudioIsNonTrivial) {
    Rig a;
    program(a);
    a.run_cycles(768 * 256 * 4);
    bool any = false;
    for (int16_t s : drain(a)) any = any || s != 0;
    EXPECT_TRUE(any);
}

TEST(StateSpu, LoadsWithoutBatching) {
    Rig a;
    a.w16(Spu::kSpuCnt, 0xC000);
    a.w16(Spu::kTransferAddr, 0x100);
    a.w16(Spu::kTransferFifo, 0xBEEF);
    auto saved = a.save();

    Rig b;
    b.load(saved);
    EXPECT_EQ(b.spu.transfer_address(), a.spu.transfer_address());
    EXPECT_EQ(b.spu.ram()[0x800], 0xEF);
    EXPECT_EQ(b.save(), saved);
}

TEST(StateSpu, BatchingStateNeedsStartedSpu) {
    Rig a;
    program(a);
    a.run_cycles(768 * 300);
    auto saved = a.save();

    Rig b;  // start() not called
    EXPECT_THROW(b.load(saved), StateError);
}

TEST(StateSpu, TruncatedDataThrows) {
    Rig a;
    program(a);
    a.run_cycles(768 * 300);
    auto saved = a.save();
    for (std::size_t n : {std::size_t{0}, std::size_t{10}, saved.size() / 2, saved.size() - 1}) {
        Rig b;
        b.spu.start(b.sched);
        std::vector<uint8_t> cut(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(n));
        EXPECT_THROW(b.load(cut), StateError) << n;
    }
}

// The ring capacity is configuration: a state loads into a ring of another size, keeping the newest frames.
TEST(StateSpu, AudioRingLoadsIntoAnotherCapacity) {
    AudioRing big(8);
    for (int16_t i = 1; i <= 6; ++i) big.push(i, static_cast<int16_t>(-i));
    StateIo w;
    big.io(w);
    std::vector<uint8_t> data = w.take();

    AudioRing same(8), small(4);
    StateIo r1{std::span<const uint8_t>(data)};
    same.io(r1);
    StateIo r2{std::span<const uint8_t>(data)};
    small.io(r2);

    int16_t out[16] = {};
    ASSERT_EQ(same.pop(out), 6u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[11], -6);
    ASSERT_EQ(small.pop(out), 4u);
    EXPECT_EQ(out[0], 3);
    EXPECT_EQ(out[1], -3);
    EXPECT_EQ(out[6], 6);
}
