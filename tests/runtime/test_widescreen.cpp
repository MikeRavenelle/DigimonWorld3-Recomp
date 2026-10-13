#include <gtest/gtest.h>

#include "dw3/widescreen.h"

using namespace dw3;

TEST(WideMargin, ReachesTheAspectInWholeStepsOfFour) {
    EXPECT_EQ(wide_margin(WideAspect::Off, 320, 240), 0);
    // 240 * 16 / 9 = 426 columns: (426 - 320) / 2 = 53, rounded down to 52.
    EXPECT_EQ(wide_margin(WideAspect::Ratio16x9, 320, 240), 52);
    EXPECT_EQ(wide_margin(WideAspect::Ratio16x10, 320, 240), 32);
    EXPECT_EQ(wide_margin(WideAspect::Ratio21x9, 320, 240), 120);
    for (WideAspect a : {WideAspect::Ratio16x9, WideAspect::Ratio16x10, WideAspect::Ratio21x9}) {
        const int m = wide_margin(a, 320, 240);
        EXPECT_EQ(m % 4, 0);
        const AspectRatio r = aspect_ratio(a);
        const double wide = (320 + 2 * m) / 240.0, want = static_cast<double>(r.num) / r.den;
        EXPECT_NEAR(wide, want, 0.015);
    }
}

TEST(WideMargin, NeverShrinksTheFrame) {
    EXPECT_EQ(wide_margin(WideAspect::Ratio16x9, 640, 240), 0);  // already wider than 16:9
    EXPECT_EQ(wide_margin(WideAspect::Ratio16x10, 384, 240), 0);
}

TEST(WideMargin, FitsTheRenderTargetColumns) {
    EXPECT_LE(make_wide_frame(WideAspect::Ratio21x9).width(), WideFrame::kMaxColumns);
}

TEST(WideFrameMapping, CentresTheFrameInTheCopy) {
    const WideFrame w = make_wide_frame(WideAspect::Ratio16x9);
    EXPECT_TRUE(w.enabled());
    EXPECT_EQ(w.width(), 424);
    EXPECT_EQ(w.map_x(0), WideFrame::kOriginX + 52);
    EXPECT_EQ(w.map_x(319), WideFrame::kOriginX + 52 + 319);
    // The world beyond the 4:3 edges lands inside the copy's margins.
    EXPECT_EQ(w.map_x(-52), WideFrame::kOriginX);
    EXPECT_EQ(w.map_x(319 + 52), WideFrame::kOriginX + w.width() - 1);
}

TEST(WideFrameMapping, SnapsEdgeTouchingShapesToTheCopyEdges) {
    const WideFrame w = make_wide_frame(WideAspect::Ratio16x9);
    EXPECT_EQ(w.snap_x(0), WideFrame::kOriginX);
    EXPECT_EQ(w.snap_x(-5), WideFrame::kOriginX);
    EXPECT_EQ(w.snap_x(320), WideFrame::kOriginX + w.width());
    EXPECT_EQ(w.snap_x(400), WideFrame::kOriginX + w.width());
    EXPECT_EQ(w.snap_x(100), w.map_x(100));
}

TEST(WideFrameMapping, RecognisesFrameBufferAreas) {
    const WideFrame w = make_wide_frame(WideAspect::Ratio16x9);
    EXPECT_TRUE(w.in_frame(0, 0, 319, 239));
    EXPECT_TRUE(w.in_frame(0, 256, 319, 495));
    EXPECT_TRUE(w.in_frame(208, 76, 307, 135));  // the target portrait box
    EXPECT_FALSE(w.in_frame(0, 0, 511, 255));    // a texture-sized area
    EXPECT_FALSE(w.in_frame(0, 240, 319, 479));  // straddles the gap between the two buffers
    EXPECT_FALSE(w.in_frame(384, 0, 447, 63));
    EXPECT_TRUE(w.covers_width(0, 319));
    EXPECT_FALSE(w.covers_width(208, 307));
}
