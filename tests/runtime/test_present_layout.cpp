#include <gtest/gtest.h>

#include "dw3/present_layout.h"

using namespace dw3;

TEST(PresentLayout, FitExact43) {
    EXPECT_EQ(compute_present_rect(320, 240, 1280, 960, ScaleMode::Fit), (PresentRect{0, 0, 1280, 960}));
}

TEST(PresentLayout, FitPillarboxesWideWindow) {
    PresentRect r = compute_present_rect(320, 240, 1920, 1080, ScaleMode::Fit);
    EXPECT_EQ(r.h, 1080);
    EXPECT_EQ(r.w, 1440);
    EXPECT_EQ(r.x, 240);
    EXPECT_EQ(r.y, 0);
}

TEST(PresentLayout, FitLetterboxesTallWindow) {
    PresentRect r = compute_present_rect(640, 480, 800, 1000, ScaleMode::Fit);
    EXPECT_EQ(r.w, 800);
    EXPECT_EQ(r.h, 600);
    EXPECT_EQ(r.x, 0);
    EXPECT_EQ(r.y, 200);
}

TEST(PresentLayout, AspectIgnoresSourceWidth) {
    // 256, 320, 512 and 640 wide modes all fill the same 4:3 area.
    for (int w : {256, 320, 368, 512, 640})
        EXPECT_EQ(compute_present_rect(w, 240, 1024, 768, ScaleMode::Fit), (PresentRect{0, 0, 1024, 768}));
}

TEST(PresentLayout, StretchFillsWindow) {
    EXPECT_EQ(compute_present_rect(320, 240, 1000, 300, ScaleMode::Stretch), (PresentRect{0, 0, 1000, 300}));
}

TEST(PresentLayout, IntegerUsesWholeMultiples) {
    // 320x240 on a 1920x1080 window: 4x gives 1280x960.
    PresentRect r = compute_present_rect(320, 240, 1920, 1080, ScaleMode::Integer);
    EXPECT_EQ(r.w, 1280);
    EXPECT_EQ(r.h, 960);
    EXPECT_EQ(r.x, 320);
    EXPECT_EQ(r.y, 60);
}

TEST(PresentLayout, IntegerFallsBackToFitWhenWindowIsSmall) {
    EXPECT_EQ(compute_present_rect(320, 240, 200, 150, ScaleMode::Integer),
              compute_present_rect(320, 240, 200, 150, ScaleMode::Fit));
}

TEST(PresentLayout, DegenerateSizesGiveEmptyRect) {
    EXPECT_EQ(compute_present_rect(0, 240, 100, 100, ScaleMode::Fit), PresentRect{});
    EXPECT_EQ(compute_present_rect(320, 240, 0, 100, ScaleMode::Stretch), PresentRect{});
}

TEST(PresentLayout, RectStaysInsideWindow) {
    for (int ww = 1; ww < 200; ww += 7)
        for (int wh = 1; wh < 200; wh += 11)
            for (ScaleMode m : {ScaleMode::Fit, ScaleMode::Integer, ScaleMode::Stretch}) {
                PresentRect r = compute_present_rect(320, 240, ww, wh, m);
                EXPECT_GE(r.x, 0);
                EXPECT_GE(r.y, 0);
                EXPECT_LE(r.x + r.w, ww);
                EXPECT_LE(r.y + r.h, wh);
            }
}

TEST(PresentLayout, ParseScaleMode) {
    ScaleMode m = ScaleMode::Fit;
    EXPECT_TRUE(parse_scale_mode("integer", m));
    EXPECT_EQ(m, ScaleMode::Integer);
    EXPECT_TRUE(parse_scale_mode("stretch", m));
    EXPECT_EQ(m, ScaleMode::Stretch);
    EXPECT_FALSE(parse_scale_mode("zoom", m));
    EXPECT_FALSE(parse_scale_mode(nullptr, m));
}

TEST(PresentLayout, UploadRowAlignment) {
    EXPECT_EQ(upload_row_pixels(1), 64u);
    EXPECT_EQ(upload_row_pixels(64), 64u);
    EXPECT_EQ(upload_row_pixels(320), 320u);
    EXPECT_EQ(upload_row_pixels(368), 384u);
}

TEST(PresentRectAspect, WideFrameFitsA16x9WindowExactly) {
    EXPECT_EQ(compute_present_rect(424, 240, 1920, 1080, ScaleMode::Fit, 16, 9), (PresentRect{0, 0, 1920, 1080}));
}

TEST(PresentRectAspect, FourByThreeInAWideWindowIsPillarboxed) {
    EXPECT_EQ(compute_present_rect(320, 240, 1920, 1080, ScaleMode::Fit), (PresentRect{240, 0, 1440, 1080}));
}

TEST(PresentRectAspect, WideFrameInAFourByThreeWindowIsLetterboxed) {
    EXPECT_EQ(compute_present_rect(424, 240, 1024, 768, ScaleMode::Fit, 16, 9), (PresentRect{0, 96, 1024, 576}));
}

TEST(PresentRectAspect, IntegerModeUsesTheWideWidth) {
    const PresentRect r = compute_present_rect(424, 240, 1920, 1080, ScaleMode::Integer, 16, 9);
    EXPECT_EQ(r.h % 240, 0);
    EXPECT_EQ(r.w, 426 * (r.h / 240));  // 426 = 240 * 16 / 9
}
