#include <gtest/gtest.h>

#include <vector>

#include "dw3/dma.h"
#include "dw3/gpu.h"
#include "dw3/irq.h"
#include "dw3/video_timing.h"
#include "hw_test_util.h"

using namespace dw3;

namespace {

struct DisplayRig : dw3test::Clock {
    IrqController irq{bus};
    VideoTiming video{sched, irq};
    Dma dma{bus, sched, irq};
    Gpu gpu{bus, irq, video, dma};

    void set(int x, int y, uint16_t v) { gpu.vram()[static_cast<size_t>(y * kVramWidth + x)] = v; }
    // GP1: display on, start address, mode (hres 320 or 640, vres 240, 15 or 24 bit).
    void configure(int vx, int vy, bool wide, bool rgb24, bool enabled = true) {
        gpu.write_gp1(0x03000000u | (enabled ? 0u : 1u));
        gpu.write_gp1(0x05000000u | static_cast<uint32_t>(vx) | (static_cast<uint32_t>(vy) << 10));
        gpu.write_gp1(0x08000000u | (wide ? 3u : 1u) | (rgb24 ? 0x10u : 0u));
    }
};

}  // namespace

TEST(DisplaySnapshot, FifteenBitExpandsToRgba) {
    DisplayRig r;
    r.configure(0, 0, false, false);
    r.set(0, 0, 0x7FFF);                       // white
    r.set(1, 0, 31);                           // red
    r.set(2, 0, static_cast<uint16_t>(31 << 5));  // green
    r.set(0, 1, static_cast<uint16_t>(31 << 10)); // blue
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    ASSERT_TRUE(r.gpu.snapshot_display(px, w, h));
    EXPECT_EQ(w, 320);
    EXPECT_EQ(h, 240);
    ASSERT_EQ(px.size(), 320u * 240u);
    EXPECT_EQ(px[0], 0xFFFFFFFFu);
    EXPECT_EQ(px[1], 0xFF0000FFu);  // R in the low byte
    EXPECT_EQ(px[2], 0xFF00FF00u);
    EXPECT_EQ(px[320], 0xFFFF0000u);
}

TEST(DisplaySnapshot, DisplayOriginOffsetsIntoVram) {
    DisplayRig r;
    r.configure(100, 50, false, false);
    r.set(100, 50, 31);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    ASSERT_TRUE(r.gpu.snapshot_display(px, w, h));
    EXPECT_EQ(px[0], 0xFF0000FFu);
    EXPECT_EQ(px[1], 0xFF000000u);
}

TEST(DisplaySnapshot, TwentyFourBitPacksThreeBytesPerPixel) {
    DisplayRig r;
    r.configure(0, 0, false, true);
    // Pixels 0 and 1: bytes 11 22 33 | 44 55 66 -> words 0x2211, 0x4433, 0x6655.
    r.set(0, 0, 0x2211);
    r.set(1, 0, 0x4433);
    r.set(2, 0, 0x6655);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    ASSERT_TRUE(r.gpu.snapshot_display(px, w, h));
    EXPECT_EQ(px[0], 0xFF332211u);
    EXPECT_EQ(px[1], 0xFF665544u);
}

TEST(DisplaySnapshot, WideModeAndWrapAroundVram) {
    DisplayRig r;
    r.configure(0, 500, true, false);
    r.set(0, 4, 31);  // line 12 of the display wraps past VRAM row 511
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    ASSERT_TRUE(r.gpu.snapshot_display(px, w, h));
    EXPECT_EQ(w, 640);
    EXPECT_EQ(px[static_cast<size_t>(16 * w)], 0xFF0000FFu);
}

TEST(DisplaySnapshot, DisabledDisplayReportsFalseAndBlack) {
    DisplayRig r;
    r.configure(0, 0, false, false, false);
    r.set(0, 0, 0x7FFF);
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    EXPECT_FALSE(r.gpu.snapshot_display(px, w, h));
    ASSERT_FALSE(px.empty());
    EXPECT_EQ(px[0], 0xFF000000u);
}
