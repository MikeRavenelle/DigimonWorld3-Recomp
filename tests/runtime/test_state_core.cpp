#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "dw3/dma.h"
#include "dw3/gpu.h"
#include "dw3/gte.h"
#include "dw3/irq.h"
#include "dw3/soft_rasterizer.h"
#include "dw3/state.h"
#include "dw3/timers.h"
#include "dw3/video_timing.h"
#include "hw_test_util.h"

using namespace dw3;

namespace {

constexpr uint32_t kIStat = 0x1F801070u, kIMask = 0x1F801074u;
constexpr uint32_t kTimerBase = 0x1F801100u;
constexpr uint32_t kDmaBase = 0x1F801080u, kDpcr = 0x1F8010F0u, kDicr = 0x1F8010F4u;

// The scheduler goes first (it restores the id counters and drops queued events), then the blocks.
template <class F>
std::vector<uint8_t> save_with(Scheduler& sched, F&& blocks) {
    StateIo s;
    sched.io(s);
    blocks(s);
    return s.take();
}

template <class F>
void load_with(Scheduler& sched, const std::vector<uint8_t>& data, F&& blocks) {
    StateIo s(data);
    sched.io(s);
    blocks(s);
    EXPECT_TRUE(s.at_end());
}

// Time-dependent blocks share one clock; the copy is brought to the same cycle before loading.
struct Rig : dw3test::Clock {
    IrqController irq{bus};
    VideoTiming video{sched, irq};
    Timers timers{bus, sched, irq, video};
    Dma dma{bus, sched, irq};
    Gpu gpu{bus, irq, video, dma};
    SoftRasterizer raster;
    std::vector<int64_t> vblanks;

    Rig() {
        gpu.set_rasterizer(&raster);
        video.on_frame = [this](uint64_t) { vblanks.push_back(ctx().cycles); };
    }
    void blocks(StateIo& s) {
        irq.io(s);
        video.io(s);
        timers.io(s);
        dma.io(s);
        gpu.io(s);
    }
    std::vector<uint8_t> save() {
        return save_with(sched, [&](StateIo& s) { blocks(s); });
    }
    void load(const std::vector<uint8_t>& data) {
        load_with(sched, data, [&](StateIo& s) { blocks(s); });
    }
};

}  // namespace

// ---- Gte ----

namespace {
void fill_gte(Gte& g) {
    uint32_t x = 0x12345678u;
    auto next = [&] {
        x = x * 1664525u + 1013904223u;
        return x;
    };
    for (unsigned r = 0; r < 32; ++r) {
        if (r == 15 || r == 28 || r == 29 || r == 30 || r == 31) continue;  // write side effects / read-only
        g.write_data(r, next() & 0x0FFF0FFFu);
    }
    for (unsigned r = 0; r < 32; ++r) {
        if (r == 31) continue;
        g.write_ctrl(r, next() & 0x00FF00FFu);
    }
}
}  // namespace

TEST(StateGte, RoundTripAndContinuation) {
    Gte a;
    fill_gte(a);
    a.execute(0x00080001u);  // RTPS, sf=1
    StateIo w;
    a.io(w);
    auto bytes = w.take();

    Gte b;
    StateIo r(bytes);
    b.io(r);
    EXPECT_TRUE(r.at_end());

    StateIo w2;
    b.io(w2);
    EXPECT_EQ(w2.written(), bytes);
    for (unsigned i = 0; i < 32; ++i) {
        EXPECT_EQ(a.read_data(i), b.read_data(i)) << i;
        EXPECT_EQ(a.read_ctrl(i), b.read_ctrl(i)) << i;
    }

    for (uint32_t cmd : {0x00080030u, 0x0008000Cu, 0x0180001Eu, 0x00080006u}) {
        a.execute(cmd);
        b.execute(cmd);
    }
    for (unsigned i = 0; i < 32; ++i) {
        EXPECT_EQ(a.read_data(i), b.read_data(i)) << i;
        EXPECT_EQ(a.read_ctrl(i), b.read_ctrl(i)) << i;
    }
}

TEST(StateGte, TruncatedThrows) {
    Gte a;
    fill_gte(a);
    StateIo w;
    a.io(w);
    auto bytes = w.take();
    bytes.resize(bytes.size() / 2);
    Gte b;
    StateIo r(bytes);
    EXPECT_THROW(b.io(r), StateError);
}

// ---- IrqController ----

TEST(StateIrq, RoundTrip) {
    dw3test::Clock a, b;
    IrqController ia{a.bus}, ib{b.bus};
    ia.write(0x4, 0x2A5, 4);
    ia.raise_mask(0x0F3);
    StateIo w;
    ia.io(w);
    auto bytes = w.take();
    StateIo r(bytes);
    ib.io(r);
    EXPECT_EQ(ib.stat(), ia.stat());
    EXPECT_EQ(ib.mask(), ia.mask());
    EXPECT_EQ(ib.pending(), ia.pending());
    StateIo w2;
    ib.io(w2);
    EXPECT_EQ(w2.written(), bytes);

    std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 3);
    StateIo rc(cut);
    EXPECT_THROW(ib.io(rc), StateError);
}

// ---- VideoTiming ----

TEST(StateVideo, VblankFiresAtTheSameCycles) {
    Rig a, b;
    a.video.set_standard(VideoStandard::Pal);
    a.video.set_interlaced(true);
    a.video.set_dot_divider(5);
    a.run_to(1'000'000);
    auto bytes = a.save();
    EXPECT_EQ(a.sched.pending_count(), 1u);  // only the vblank event while the timers are idle

    b.ctx().cycles = a.ctx().cycles;
    b.load(bytes);
    EXPECT_EQ(b.save(), bytes);
    EXPECT_EQ(b.sched.pending_count(), a.sched.pending_count());
    EXPECT_EQ(b.video.frame_count(), a.video.frame_count());
    EXPECT_EQ(b.video.standard(), VideoStandard::Pal);
    EXPECT_TRUE(b.video.interlaced());
    EXPECT_EQ(b.video.dot_divider(), 5);

    a.vblanks.clear();
    b.vblanks.clear();
    a.run_to(5'000'000);
    b.run_to(5'000'000);
    EXPECT_FALSE(a.vblanks.empty());
    EXPECT_EQ(a.vblanks, b.vblanks);
    EXPECT_EQ(a.video.frame_count(), b.video.frame_count());
    for (int64_t c : {5'100'000, 5'123'456})
        EXPECT_EQ(a.video.scanline(c), b.video.scanline(c));
    EXPECT_EQ(a.video.hblank_count(5'200'000), b.video.hblank_count(5'200'000));
}

TEST(StateVideo, TruncatedThrows) {
    Rig a, b;
    a.run_to(500'000);
    auto bytes = a.save();
    bytes.resize(bytes.size() - 5);
    EXPECT_THROW(b.load(bytes), StateError);
}

// ---- Timers ----

TEST(StateTimers, WakeEventsAndCountersContinue) {
    Rig a, b;
    a.w32(kIMask, 0x7FF);
    // Counter 0: system clock, IRQ at target 1000, reset on target, repeat.
    a.w16(kTimerBase + 0x8, 1000);
    a.w16(kTimerBase + 0x4, 0x0008 | 0x0010 | 0x0040);
    // Counter 1: hblank-clocked, IRQ at 0xFFFF-ish target without reset.
    a.w16(kTimerBase + 0x18, 300);
    a.w16(kTimerBase + 0x14, 0x0010 | 0x0100 | 0x0040);
    // Counter 2: /8 clock, toggle mode.
    a.w16(kTimerBase + 0x28, 77);
    a.w16(kTimerBase + 0x24, 0x0008 | 0x0010 | 0x0080 | 0x0200 | 0x0040);
    a.run_to(54'321);
    a.w32(kIStat, 0);

    auto bytes = a.save();
    EXPECT_GE(a.sched.pending_count(), 4u);  // vblank + wake-ups

    b.ctx().cycles = a.ctx().cycles;
    b.load(bytes);
    EXPECT_EQ(b.save(), bytes);
    EXPECT_EQ(b.sched.pending_count(), a.sched.pending_count());

    for (int64_t t : {54'400, 60'000, 100'001, 250'000, 400'000}) {
        a.run_to(t);
        b.run_to(t);
        EXPECT_EQ(a.r32(kIStat), b.r32(kIStat)) << t;
        for (int n = 0; n < 3; ++n) {
            EXPECT_EQ(a.timers.read_value(n), b.timers.read_value(n)) << t << " " << n;
            EXPECT_EQ(a.timers.read_mode(n), b.timers.read_mode(n)) << t << " " << n;
        }
        a.w32(kIStat, 0);
        b.w32(kIStat, 0);
    }
}

TEST(StateTimers, TruncatedThrows) {
    Rig a, b;
    a.w16(kTimerBase + 0x8, 10);
    a.w16(kTimerBase + 0x4, 0x18);
    a.run_to(100);
    auto bytes = a.save();
    bytes.resize(bytes.size() - 20);
    EXPECT_THROW(b.load(bytes), StateError);
}

// ---- Dma ----

TEST(StateDma, InFlightTransferCompletesIdentically) {
    Rig a, b;
    a.w32(kIMask, 0x7FF);
    a.w32(kDpcr, 0x0FFFFFFFu);
    a.w32(kDicr, (1u << 23) | (0x7Fu << 16));  // master enable + all channel IRQs
    // Ordering-table clear: many entries so the completion lands well after the save.
    a.w32(kDmaBase + 6 * 16, 0x2000);
    a.w32(kDmaBase + 6 * 16 + 4, 0x4000);
    a.w32(kDmaBase + 6 * 16 + 8, 0x11000002u);
    // A second channel with only registers programmed.
    a.w32(kDmaBase + 3 * 16, 0x1234);
    a.w32(kDmaBase + 3 * 16 + 4, 0x00020010);
    a.run_to(100);
    EXPECT_TRUE(a.dma.in_flight(DmaChannel::Otc));

    auto bytes = a.save();
    b.ctx().cycles = a.ctx().cycles;
    b.load(bytes);
    EXPECT_EQ(b.save(), bytes);
    EXPECT_TRUE(b.dma.in_flight(DmaChannel::Otc));
    EXPECT_EQ(b.dma.dpcr(), a.dma.dpcr());
    EXPECT_EQ(b.dma.dicr(), a.dma.dicr());
    EXPECT_EQ(b.dma.madr(DmaChannel::Cdrom), 0x1234u);

    a.run_to(40'000);
    b.run_to(40'000);
    EXPECT_FALSE(a.dma.in_flight(DmaChannel::Otc));
    EXPECT_FALSE(b.dma.in_flight(DmaChannel::Otc));
    EXPECT_EQ(a.dma.dicr(), b.dma.dicr());
    EXPECT_EQ(a.dma.chcr(DmaChannel::Otc), b.dma.chcr(DmaChannel::Otc));
    EXPECT_EQ(a.r32(kIStat), b.r32(kIStat));
    EXPECT_NE(a.r32(kIStat) & (1u << 3), 0u);
}

TEST(StateDma, TruncatedThrows) {
    Rig a, b;
    a.w32(kDpcr, 0x0FFFFFFFu);
    a.w32(kDmaBase + 6 * 16, 0x2000);
    a.w32(kDmaBase + 6 * 16 + 4, 0x100);
    a.w32(kDmaBase + 6 * 16 + 8, 0x11000002u);
    auto bytes = a.save();
    bytes.resize(bytes.size() - 7);
    EXPECT_THROW(b.load(bytes), StateError);
}

// ---- Gpu ----

namespace {
void drive_gpu(Gpu& g) {
    g.write_gp1(0x00000000u);
    g.write_gp1(0x03000000u);  // display on
    g.write_gp1(0x05000000u | 8u | (4u << 10));
    g.write_gp1(0x08000000u | 0x13u);
    g.write_gp0(0xE1000000u | 0x25u | (1u << 9));  // texpage, dither
    g.write_gp0(0xE3000000u | 3u | (5u << 10));
    g.write_gp0(0xE4000000u | 400u | (300u << 10));
    g.write_gp0(0xE5000000u | 7u | (9u << 11));
    g.write_gp0(0xE6000000u | 1u);
    // Flat triangle and a gouraud quad.
    g.write_gp0(0x20FF8040u);
    g.write_gp0(0x00100010u);
    g.write_gp0(0x00100080u);
    g.write_gp0(0x00800040u);
    g.write_gp0(0x38204060u);
    g.write_gp0(0x00200100u);
    g.write_gp0(0x00FF2080u);
    g.write_gp0(0x00A00140u);
    g.write_gp0(0x00403090u);
    g.write_gp0(0x00B00180u);
    g.write_gp0(0x0050A0A0u);
    g.write_gp0(0x00C001C0u);
    // CPU -> VRAM upload, left half finished.
    g.write_gp0(0xA0000000u);
    g.write_gp0(0x00200010u);  // (16, 32)
    g.write_gp0(0x00040004u);  // 4x4 = 8 words
    for (uint32_t i = 0; i < 3; ++i) g.write_gp0(0x7FFF0000u + i * 0x10001u);
}
}  // namespace

TEST(StateGpu, RoundTripAndContinuation) {
    Rig a, b;
    drive_gpu(a.gpu);
    // Saved in the middle of the 8-word upload (3 words in).
    auto bytes = a.save();
    b.ctx().cycles = a.ctx().cycles;
    b.load(bytes);
    EXPECT_EQ(b.save(), bytes);
    EXPECT_EQ(b.gpu.read_gpustat(), a.gpu.read_gpustat());
    EXPECT_EQ(b.gpu.commands(), a.gpu.commands());
    EXPECT_EQ(b.gpu.primitives(), a.gpu.primitives());
    EXPECT_TRUE(std::equal(a.gpu.vram().begin(), a.gpu.vram().end(), b.gpu.vram().begin()));
    EXPECT_EQ(b.gpu.display().width, a.gpu.display().width);
    EXPECT_EQ(b.gpu.draw_state().off_x, a.gpu.draw_state().off_x);

    // Same continuation on both: finish the upload, draw a rectangle and a poly-line, then read back.
    for (Gpu* g : {&a.gpu, &b.gpu}) {
        for (int i = 0; i < 8; ++i) g->write_gp0(0x03E003E0u);
        g->write_gp0(0x60FFFFFFu);
        g->write_gp0(0x00300020u);
        g->write_gp0(0x00100020u);
        g->write_gp0(0x58FF0000u);
        g->write_gp0(0x00020002u);
        g->write_gp0(0x0000FF00u);
        g->write_gp0(0x00300030u);
        g->write_gp0(0x000000FFu);
        g->write_gp0(0x00100050u);
        g->write_gp0(0x50005000u);
        g->write_gp0(0xC0000000u);  // VRAM -> CPU
        g->write_gp0(0x00200010u);
        g->write_gp0(0x00020002u);
    }
    EXPECT_TRUE(std::equal(a.gpu.vram().begin(), a.gpu.vram().end(), b.gpu.vram().begin()));
    EXPECT_EQ(a.gpu.read_gpustat(), b.gpu.read_gpustat());
    for (int i = 0; i < 3; ++i) EXPECT_EQ(a.gpu.read_gpuread(), b.gpu.read_gpuread());
    EXPECT_EQ(a.save(), b.save());
}

TEST(StateGpu, MidPolylineSurvives) {
    Rig a, b;
    a.gpu.write_gp0(0xE3000000u);
    a.gpu.write_gp0(0xE4000000u | 1023u | (511u << 10));
    a.gpu.write_gp0(0x48FF00FFu);  // gouraud poly-line start
    a.gpu.write_gp0(0x00100010u);
    a.gpu.write_gp0(0x0000FF00u);
    a.gpu.write_gp0(0x00400060u);
    auto bytes = a.save();
    b.load(bytes);
    EXPECT_EQ(b.save(), bytes);
    for (Gpu* g : {&a.gpu, &b.gpu}) {
        g->write_gp0(0x00FF0000u);
        g->write_gp0(0x00700020u);
        g->write_gp0(0x50005000u);
    }
    EXPECT_TRUE(std::equal(a.gpu.vram().begin(), a.gpu.vram().end(), b.gpu.vram().begin()));
    EXPECT_NE(a.gpu.vram()[0x10 * kVramWidth + 0x10], 0);
    EXPECT_EQ(a.save(), b.save());
}

TEST(StateGpu, TruncatedThrows) {
    Rig a, b;
    drive_gpu(a.gpu);
    auto bytes = a.save();
    bytes.resize(bytes.size() / 2);
    EXPECT_THROW(b.load(bytes), StateError);
}
