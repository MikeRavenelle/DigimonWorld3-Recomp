#include <gtest/gtest.h>

#include "present/osd.h"

using namespace dw3app;

namespace {
uint32_t px(const OsdImage& img, int x, int y) { return img.rgba[static_cast<std::size_t>(y) * img.w + x]; }
}  // namespace

TEST(Osd, ScaleIsIntegerAndAtLeastTwo) {
    EXPECT_EQ(osd_scale(100), 2);
    EXPECT_EQ(osd_scale(480), 2);
    EXPECT_EQ(osd_scale(720), 2);
    EXPECT_EQ(osd_scale(1080), 3);
    EXPECT_EQ(osd_scale(2160), 6);
}

TEST(Osd, BoxSizeFollowsTextLength) {
    OsdImage img = osd_render_text("Saved slot 3", 2, 10000);
    int w = 0, h = 0;
    osd_box_size(12, 2, w, h);
    EXPECT_EQ(img.w, w);
    EXPECT_EQ(img.h, h);
    EXPECT_EQ(img.w, 12 * 8 * 2 + 2 * osd_padding(2));
    EXPECT_EQ(img.h, 8 * 2 + 2 * osd_padding(2));
    EXPECT_EQ(img.rgba.size(), static_cast<std::size_t>(img.w) * img.h);
}

TEST(Osd, GlyphPixelsAreScaledAndPlaced) {
    // '!' row 0 is 0x18: font pixels 3 and 4 are set, 0..2 and 5..7 are not.
    for (int scale : {2, 3}) {
        OsdImage img = osd_render_text("!", scale, 10000);
        const int pad = osd_padding(scale);
        EXPECT_EQ(px(img, 0, 0), kOsdBoxColor);
        for (int gx = 0; gx < 8; ++gx) {
            const bool on = gx == 3 || gx == 4;
            for (int s = 0; s < scale; ++s) {
                EXPECT_EQ(px(img, pad + gx * scale + s, pad + s), on ? kOsdTextColor : kOsdBoxColor) << gx;
            }
        }
        // Row 5 of '!' is blank, row 6 has the dot.
        EXPECT_EQ(px(img, pad + 3 * scale, pad + 5 * scale), kOsdBoxColor);
        EXPECT_EQ(px(img, pad + 3 * scale, pad + 6 * scale), kOsdTextColor);
    }
}

TEST(Osd, SecondGlyphIsOffsetByAdvance) {
    OsdImage img = osd_render_text("!!", 2, 10000);
    const int pad = osd_padding(2);
    EXPECT_EQ(px(img, pad + 3 * 2, pad), kOsdTextColor);
    EXPECT_EQ(px(img, pad + 8 * 2 + 3 * 2, pad), kOsdTextColor);
    EXPECT_EQ(px(img, pad + 8 * 2 + 1 * 2, pad), kOsdBoxColor);
}

TEST(Osd, UnknownCharactersDrawAsQuestionMark) {
    OsdImage q = osd_render_text("?", 2, 10000);
    EXPECT_EQ(osd_render_text("\x01", 2, 10000).rgba, q.rgba);
    EXPECT_EQ(osd_render_text("\xC3", 2, 10000).rgba, q.rgba);
    EXPECT_EQ(osd_render_text("\n", 2, 10000).rgba, q.rgba);
    EXPECT_NE(osd_render_text("A", 2, 10000).rgba, q.rgba);
}

TEST(Osd, SpaceDrawsNoGlyphPixels) {
    OsdImage img = osd_render_text(" ", 2, 10000);
    for (uint32_t p : img.rgba) EXPECT_EQ(p, kOsdBoxColor);
}

TEST(Osd, TextIsCutToTheAvailableWidth) {
    const int scale = 2;
    int w5 = 0, h = 0;
    osd_box_size(5, scale, w5, h);
    OsdImage img = osd_render_text("0123456789", scale, w5);
    EXPECT_EQ(img.w, w5);
    EXPECT_EQ(img.rgba, osd_render_text("01234", scale, 10000).rgba);
    // One pixel short of five characters leaves four.
    int w4 = 0;
    osd_box_size(4, scale, w4, h);
    EXPECT_EQ(osd_render_text("0123456789", scale, w5 - 1).w, w4);
}

TEST(Osd, NothingFitsGivesEmptyImage) {
    EXPECT_EQ(osd_render_text("abc", 2, 10).w, 0);
    EXPECT_TRUE(osd_render_text("abc", 2, 10).rgba.empty());
    EXPECT_EQ(osd_render_text("", 2, 10000).w, 0);
}

TEST(OsdLifetime, ShowsThenExpires) {
    Osd osd;
    float op = 0;
    EXPECT_EQ(osd.frame(1280, 960, 0, op), nullptr);
    osd.show("Saved slot 1", 1000);
    const OsdImage* img = osd.frame(1280, 960, 1100, op);
    ASSERT_NE(img, nullptr);
    EXPECT_GT(img->w, 0);
    EXPECT_FLOAT_EQ(op, 1.0f);
    img = osd.frame(1280, 960, 1000 + Osd::kShowMs - Osd::kFadeMs / 2, op);
    ASSERT_NE(img, nullptr);
    EXPECT_FLOAT_EQ(op, 0.5f);
    EXPECT_EQ(osd.frame(1280, 960, 1000 + Osd::kShowMs, op), nullptr);
    EXPECT_EQ(osd.frame(1280, 960, 1000 + Osd::kShowMs + 5, op), nullptr);
}

TEST(OsdLifetime, SerialChangesOnlyWhenPixelsDo) {
    Osd osd;
    float op = 0;
    osd.show("Slot 2", 0);
    osd.frame(1280, 960, 10, op);
    const uint64_t s = osd.serial();
    osd.frame(1280, 960, 20, op);
    EXPECT_EQ(osd.serial(), s);
    osd.frame(1280, 1440, 30, op);  // larger window, larger scale
    EXPECT_NE(osd.serial(), s);
    const uint64_t s2 = osd.serial();
    osd.show("Slot 3", 40);
    osd.frame(1280, 1440, 50, op);
    EXPECT_NE(osd.serial(), s2);
}

TEST(OsdLifetime, NewMessageRestartsTheTimer) {
    Osd osd;
    float op = 0;
    osd.show("one", 0);
    osd.show("two", 1500);
    EXPECT_NE(osd.frame(1280, 960, 2500, op), nullptr);
    EXPECT_EQ(osd.frame(1280, 960, 3600, op), nullptr);
}

TEST(OsdStatus, ShowsAtTheTopRightWhenNoMessageIsActive) {
    Osd osd;
    float op = 0;
    EXPECT_EQ(osd.frame(1280, 960, 0, op), nullptr);
    osd.set_status("60 FPS");
    const OsdImage* status = osd.frame(1280, 960, 0, op);
    ASSERT_NE(status, nullptr);
    EXPECT_LT(op, 1.0f);  // unobtrusive
    EXPECT_EQ(osd.x(1280), 1280 - status->w - osd.margin());
    osd.show("Slot 2", 100);
    const OsdImage* message = osd.frame(1280, 960, 150, op);
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(osd.x(1280), osd.margin());  // messages keep the top left
    EXPECT_EQ(op, 1.0f);
    EXPECT_NE(osd.frame(1280, 960, 100 + Osd::kShowMs + 10, op), nullptr);  // back to the status line
    EXPECT_NE(osd.x(1280), osd.margin());
    osd.set_status("");
    EXPECT_EQ(osd.frame(1280, 960, 100 + Osd::kShowMs + 20, op), nullptr);
}
