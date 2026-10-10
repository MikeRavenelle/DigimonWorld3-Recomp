#include <gtest/gtest.h>

#include <vector>

#include "dw3/video_timing.h"
#include "hw_test_util.h"

using dw3::IrqController;
using dw3::VideoStandard;
using dw3::VideoTiming;

namespace {
struct Rig : dw3test::Clock {
    IrqController irq{bus};
    VideoTiming v{sched, irq};
};

// Independent closed form: cycle of the k-th NTSC progressive VBLANK start (origin 0).
// Exact hardware ratios: video clocks per CPU cycle = 715909/451584 (NTSC), 709379/451584 (PAL).
int64_t ceil_ratio(int64_t vc, int64_t num) { return (vc * 451584 + num - 1) / num; }
int64_t ceil_cycle(int64_t vc) { return ceil_ratio(vc, 715909); }
int64_t ceil_pal(int64_t vc) { return ceil_ratio(vc, 709379); }
int64_t ntsc_vblank(int64_t k) { return ceil_cycle(k * 263 * 3413 + 240 * 3413); }
}  // namespace

TEST(Video, Defaults) {
    Rig r;
    EXPECT_EQ(r.v.standard(), VideoStandard::Ntsc);
    EXPECT_FALSE(r.v.interlaced());
    EXPECT_EQ(r.v.dot_divider(), 8);
    EXPECT_EQ(r.v.frame_count(), 0u);
    EXPECT_EQ(r.v.line_clocks(), 3413);
    EXPECT_EQ(r.v.lines_per_frame(), 263);
    EXPECT_EQ(r.v.vblank_line(), 240);
}

TEST(Video, FirstVblankCycle) {
    Rig r;
    EXPECT_EQ(r.v.vblank_cycle(0), ntsc_vblank(0));
    EXPECT_EQ(r.ctx().next_event, ntsc_vblank(0));
}

TEST(Video, NoVblankBeforeFirstCycle) {
    Rig r;
    r.run_to(ntsc_vblank(0) - 1);
    EXPECT_EQ(r.v.frame_count(), 0u);
    EXPECT_EQ(r.irq.stat(), 0u);
}

TEST(Video, VblankRaisesIrq0AtExactCycle) {
    Rig r;
    r.run_to(ntsc_vblank(0));
    EXPECT_EQ(r.v.frame_count(), 1u);
    EXPECT_EQ(r.irq.stat(), 1u);
}

TEST(Video, VblankCadenceExact100Frames) {
    Rig r;
    for (int64_t k = 0; k < 100; ++k) {
        r.run_to(ntsc_vblank(k) - 1);
        EXPECT_EQ(r.v.frame_count(), static_cast<uint64_t>(k)) << k;
        r.run_to(ntsc_vblank(k));
        EXPECT_EQ(r.v.frame_count(), static_cast<uint64_t>(k + 1)) << k;
    }
}

TEST(Video, NtscFramePeriodMatchesHardware) {
    // 263 lines * 3413 clocks at 53.693175 MHz = 566203.78 CPU cycles (59.817 Hz).
    Rig r;
    for (uint64_t k = 0; k < 50; ++k) {
        int64_t d = r.v.vblank_cycle(k + 1) - r.v.vblank_cycle(k);
        EXPECT_TRUE(d == 566203 || d == 566204) << d;
    }
    EXPECT_NEAR(static_cast<double>(r.v.vblank_cycle(1000) - r.v.vblank_cycle(0)) / 1000.0, 566203.775, 0.01);
}

TEST(Video, NoDriftOverManyFrames) {
    Rig r;
    // 263 * 3413 video clocks per frame: 600 frames take 600 * 897619 * 451584 / 715909 cycles within 1.
    int64_t span = r.v.vblank_cycle(600) - r.v.vblank_cycle(0);
    EXPECT_NEAR(static_cast<double>(span), 600.0 * 897619.0 * 451584.0 / 715909.0, 1.0);
}

TEST(Video, IrqRaisedOncePerFrame) {
    Rig r;
    int raises = 0;
    r.irq.on_line_change = [&](bool p) { raises += p; };
    r.w32(0x1F801074u, 1);
    for (int i = 0; i < 5; ++i) {
        r.run_to(r.v.vblank_cycle(i));
        EXPECT_EQ(raises, i + 1);
        r.w32(0x1F801070u, 0);  // game acks
        r.run_to(r.v.vblank_cycle(i) + 100000);
        EXPECT_EQ(raises, i + 1);
    }
}

TEST(Video, UnackedVblankStaysSetNoExtraLineEdge) {
    Rig r;
    int edges = 0;
    r.irq.on_line_change = [&](bool) { ++edges; };
    r.w32(0x1F801074u, 1);
    r.run_to(r.v.vblank_cycle(3));
    EXPECT_EQ(edges, 1);
    EXPECT_EQ(r.v.frame_count(), 4u);
}

TEST(Video, FrameCallbackGetsFrameNumber) {
    Rig r;
    std::vector<uint64_t> frames;
    r.v.on_frame = [&](uint64_t f) { frames.push_back(f); };
    r.run_to(r.v.vblank_cycle(2));
    EXPECT_EQ(frames, (std::vector<uint64_t>{1, 2, 3}));
}

TEST(Video, FrameCallbackRunsAfterIrqRaise) {
    Rig r;
    uint32_t stat_seen = 0;
    r.v.on_frame = [&](uint64_t) { stat_seen = r.irq.stat(); };
    r.run_to(r.v.vblank_cycle(0));
    EXPECT_EQ(stat_seen, 1u);
}

TEST(Video, VblankStateWindow) {
    Rig r;
    int64_t vb = r.v.vblank_cycle(0);
    EXPECT_FALSE(r.v.in_vblank(vb - 1));
    EXPECT_TRUE(r.v.in_vblank(vb));
    int64_t end = r.v.vblank_cycle(1) - int64_t{240} * 3413 * 451584 / 715909;  // start of frame 1
    EXPECT_TRUE(r.v.in_vblank(end - 2));
    EXPECT_FALSE(r.v.in_vblank(end + 2));
}

TEST(Video, NextVblankEdgeAlternatesStartEnd) {
    Rig r;
    int64_t a = r.v.next_vblank_edge(0);
    EXPECT_EQ(a, r.v.vblank_cycle(0));
    int64_t b = r.v.next_vblank_edge(a);
    EXPECT_GT(b, a);
    EXPECT_FALSE(r.v.in_vblank(b));
    EXPECT_TRUE(r.v.in_vblank(b - 1));
    EXPECT_EQ(r.v.next_vblank_edge(b), r.v.vblank_cycle(1));
}

TEST(Video, EdgeIsStrictlyAfterQuery) {
    Rig r;
    int64_t e = r.v.next_hblank_edge(0);
    EXPECT_EQ(r.v.next_hblank_edge(e), r.v.next_hblank_edge(e));
    EXPECT_GT(r.v.next_hblank_edge(e), e);
    EXPECT_GT(r.v.next_vblank_edge(r.v.vblank_cycle(0)), r.v.vblank_cycle(0));
}

TEST(Video, HblankWindowFirstLine) {
    Rig r;
    int64_t start = ceil_cycle(2560), end = ceil_cycle(3413);
    EXPECT_FALSE(r.v.in_hblank(0));
    EXPECT_FALSE(r.v.in_hblank(start - 1));
    EXPECT_TRUE(r.v.in_hblank(start));
    EXPECT_TRUE(r.v.in_hblank(end - 1));
    EXPECT_FALSE(r.v.in_hblank(end));
    EXPECT_EQ(r.v.next_hblank_edge(0), start);
    EXPECT_EQ(r.v.next_hblank_edge(start), end);
}

TEST(Video, HblankCountMatchesLines) {
    Rig r;
    EXPECT_EQ(r.v.hblank_count(0), 0u);
    EXPECT_EQ(r.v.hblank_count(ceil_cycle(2560) - 1), 0u);
    EXPECT_EQ(r.v.hblank_count(ceil_cycle(2560)), 1u);
    EXPECT_EQ(r.v.hblank_count(ceil_cycle(3413 + 2560) - 1), 1u);
    EXPECT_EQ(r.v.hblank_count(ceil_cycle(3413 + 2560)), 2u);
}

TEST(Video, HblankCountPerFrameIs263) {
    Rig r;
    uint64_t a = r.v.hblank_count(r.v.vblank_cycle(0));
    uint64_t b = r.v.hblank_count(r.v.vblank_cycle(1));
    EXPECT_EQ(b - a, 263u);
}

TEST(Video, HblankCountMonotonic) {
    Rig r;
    uint64_t prev = 0;
    for (int64_t c = 0; c < 200000; c += 97) {
        uint64_t n = r.v.hblank_count(c);
        EXPECT_GE(n, prev);
        prev = n;
    }
}

TEST(Video, ScanlineNumbers) {
    Rig r;
    EXPECT_EQ(r.v.scanline(0), 0);
    EXPECT_EQ(r.v.scanline(ceil_cycle(3413)), 1);
    EXPECT_EQ(r.v.scanline(ceil_cycle(3413) - 1), 0);
    EXPECT_EQ(r.v.scanline(r.v.vblank_cycle(0)), 240);
    EXPECT_EQ(r.v.scanline(r.v.vblank_cycle(5)), 240);
}

TEST(Video, ScanlineWrapsAtFrameEnd) {
    Rig r;
    int64_t f1 = ceil_cycle(263 * 3413);
    EXPECT_EQ(r.v.scanline(f1 - 1), 262);
    EXPECT_EQ(r.v.scanline(f1), 0);
}

TEST(Video, DotDividerSetter) {
    Rig r;
    for (int d : {4, 5, 7, 10, 8}) {
        r.v.set_dot_divider(d);
        EXPECT_EQ(r.v.dot_divider(), d);
    }
}

TEST(Video, DotDividerIgnoresInvalid) {
    Rig r;
    r.v.set_dot_divider(0);
    r.v.set_dot_divider(-3);
    EXPECT_EQ(r.v.dot_divider(), 8);
}

TEST(Video, DotDividerDoesNotMoveFrames) {
    Rig r;
    r.v.set_dot_divider(4);
    EXPECT_EQ(r.v.vblank_cycle(0), ntsc_vblank(0));
}

TEST(Video, ReconfigureHooksFire) {
    Rig r;
    std::vector<int> seq;
    r.v.on_before_reconfigure = [&] { seq.push_back(1); };
    r.v.on_after_reconfigure = [&] { seq.push_back(2); };
    r.v.set_dot_divider(5);
    r.v.set_dot_divider(5);  // no change: no hooks
    r.v.set_interlaced(true);
    EXPECT_EQ(seq, (std::vector<int>{1, 2, 1, 2}));
}

TEST(Video, PalGeometry) {
    Rig r;
    r.v.set_standard(VideoStandard::Pal);
    EXPECT_EQ(r.v.line_clocks(), 3406);
    EXPECT_EQ(r.v.lines_per_frame(), 314);
    EXPECT_EQ(r.v.vblank_cycle(0), ceil_pal(288 * 3406));
    int64_t d = r.v.vblank_cycle(1) - r.v.vblank_cycle(0);
    EXPECT_TRUE(d == 680823 || d == 680824) << d;  // 314 * 3406 * 451584 / 709379 = 680823.5 (49.747 Hz)
}

TEST(Video, PalFramePeriodMatchesFormula) {
    Rig r;
    r.v.set_standard(VideoStandard::Pal);
    for (uint64_t k = 0; k < 20; ++k) {
        int64_t expect = ceil_pal((k + 1) * 314 * 3406 + 288 * 3406);
        EXPECT_EQ(r.v.vblank_cycle(k + 1), expect);
    }
}

TEST(Video, InterlacedGeometry) {
    Rig r;
    r.v.set_interlaced(true);
    EXPECT_EQ(r.v.lines_per_frame(), 525);
    // Two fields per 525 lines: field k starts at floor(k * 525 * 3413 / 2) video clocks.
    for (int64_t k = 0; k < 10; ++k)
        EXPECT_EQ(r.v.vblank_cycle(k), ceil_cycle(k * 525 * 3413 / 2 + 240 * 3413)) << k;
}

TEST(Video, InterlacedRaisesVblankEveryField) {
    Rig r;
    r.v.set_interlaced(true);
    r.run_to(r.v.vblank_cycle(3));
    EXPECT_EQ(r.v.frame_count(), 4u);
}

TEST(Video, InterlacedFieldIsAboutHalfAsLong) {
    Rig r;
    r.v.set_interlaced(true);
    int64_t span = r.v.vblank_cycle(20) - r.v.vblank_cycle(0);
    EXPECT_NEAR(static_cast<double>(span), 10.0 * 525 * 3413 * 451584 / 715909, 1.0);
}

TEST(Video, ReconfigureRestartsFramePosition) {
    Rig r;
    r.run_to(1000000);
    uint64_t frames = r.v.frame_count();
    r.v.set_interlaced(true);
    EXPECT_EQ(r.v.frame_count(), frames);
    EXPECT_EQ(r.v.vblank_cycle(0), 1000000 + ceil_cycle(240 * 3413));
    EXPECT_EQ(r.v.scanline(1000000), 0);
}

TEST(Video, ReconfigureKeepsHblankCountMonotonic) {
    Rig r;
    r.run_to(500000);
    uint64_t before = r.v.hblank_count(500000);
    r.v.set_standard(VideoStandard::Pal);
    EXPECT_GE(r.v.hblank_count(500000), before);
    EXPECT_GE(r.v.hblank_count(600000), r.v.hblank_count(500000));
}

TEST(Video, ReconfigureReschedulesVblankOnce) {
    Rig r;
    r.v.set_standard(VideoStandard::Pal);
    EXPECT_EQ(r.sched.pending_count(), 1u);
    r.run_to(r.v.vblank_cycle(0));
    EXPECT_EQ(r.v.frame_count(), 1u);
}

TEST(Video, StartsAtCurrentCycle) {
    dw3test::Clock c;
    c.ctx().cycles = 12345;
    IrqController irq(c.bus);
    VideoTiming v(c.sched, irq);
    EXPECT_EQ(v.vblank_cycle(0), 12345 + ceil_cycle(240 * 3413));
}
