#include <gtest/gtest.h>

#include "dw3/hdr.h"

using namespace dw3;

TEST(HdrPq, EncodesTheReferenceLevels) {
    EXPECT_NEAR(pq_encode(0.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(pq_encode(10000.0f), 1.0f, 1e-6f);
    // Published ST 2084 code values: 100 nits 0.5081, 203 nits 0.5807 (reference white), 1000 nits 0.7518.
    EXPECT_NEAR(pq_encode(100.0f), 0.5081f, 5e-4f);
    EXPECT_NEAR(pq_encode(203.0f), 0.5807f, 5e-4f);
    EXPECT_NEAR(pq_encode(1000.0f), 0.7518f, 5e-4f);
    EXPECT_NEAR(pq_encode(0.1f), 0.0623f, 5e-4f);
}

TEST(HdrPq, ClampsOutsideTheRange) {
    EXPECT_NEAR(pq_encode(-5.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(pq_encode(50000.0f), 1.0f, 1e-6f);
}

TEST(HdrPq, DecodeInvertsEncode) {
    for (float nits : {0.01f, 1.0f, 80.0f, 203.0f, 600.0f, 1500.0f, 4000.0f, 10000.0f})
        EXPECT_NEAR(pq_decode(pq_encode(nits)), nits, nits * 2e-3f + 1e-3f) << nits;
    EXPECT_NEAR(pq_decode(0.0f), 0.0f, 1e-4f);
}

TEST(HdrPq, IsMonotonic) {
    float last = -1.0f;
    for (float nits = 0.0f; nits <= 10000.0f; nits += 25.0f) {
        const float v = pq_encode(nits);
        EXPECT_GT(v, last);
        last = v;
    }
}

TEST(HdrSrgb, LinearisesWithTheSrgbCurve) {
    EXPECT_FLOAT_EQ(srgb_to_linear(0.0f), 0.0f);
    EXPECT_NEAR(srgb_to_linear(1.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(srgb_to_linear(0.5f), 0.2140f, 5e-4f);
    EXPECT_NEAR(srgb_to_linear(0.04f), 0.04f / 12.92f, 1e-6f);  // the linear segment
    EXPECT_FLOAT_EQ(srgb_to_linear(-1.0f), 0.0f);
    EXPECT_FLOAT_EQ(srgb_to_linear(2.0f), 1.0f);
}

TEST(HdrGain, LeavesEverythingBelowTheKneeAlone) {
    for (float y : {0.0f, 0.1f, 0.5f, kHdrKnee}) EXPECT_FLOAT_EQ(hdr_gain(y, 1000.0f, 203.0f), 1.0f) << y;
}

TEST(HdrGain, LiftsWhiteToThePeak) {
    EXPECT_NEAR(hdr_gain(1.0f, 1000.0f, 203.0f), 1000.0f / 203.0f, 1e-4f);
    EXPECT_NEAR(hdr_gain(1.0f, 400.0f, 100.0f), 4.0f, 1e-5f);
}

TEST(HdrGain, RisesSmoothlyAndNeverBelowOne) {
    float last = 1.0f;
    for (float y = 0.0f; y <= 1.0f; y += 0.01f) {
        const float g = hdr_gain(y, 1000.0f, 203.0f);
        EXPECT_GE(g, last - 1e-6f);
        last = g;
    }
}

TEST(HdrGain, NeverShrinksWhenThePeakIsBelowPaperWhite) {
    EXPECT_FLOAT_EQ(hdr_gain(1.0f, 100.0f, 203.0f), 1.0f);
}

TEST(HdrEncode, PaperWhiteScalingInScRgb) {
    HdrParams p{HdrMode::ScRgb, 203.0f, 203.0f};  // peak at paper white: no expansion
    const auto white = hdr_encode_pixel(1, 1, 1, p);
    for (float c : white) EXPECT_NEAR(c, 203.0f / 80.0f, 1e-5f);
    const auto grey = hdr_encode_pixel(0.5f, 0.5f, 0.5f, p);  // SDR mid grey 0.2140 of white
    EXPECT_NEAR(grey[0], 0.2140f * 203.0f / 80.0f, 1e-3f);
    const auto black = hdr_encode_pixel(0, 0, 0, p);
    for (float c : black) EXPECT_FLOAT_EQ(c, 0.0f);
    p.paper_white_nits = 100.0f;
    p.peak_nits = 100.0f;
    EXPECT_NEAR(hdr_encode_pixel(1, 1, 1, p)[1], 100.0f / 80.0f, 1e-5f);
}

TEST(HdrEncode, WhiteReachesThePeakAndMidtonesStayPut) {
    const HdrParams p{HdrMode::ScRgb, 1000.0f, 200.0f};
    EXPECT_NEAR(hdr_encode_pixel(1, 1, 1, p)[0] * 80.0f, 1000.0f, 0.5f);
    // Below the knee (relative luminance 0.75 is sRGB 0.88) the picture is exactly the SDR one at paper white.
    const auto mid = hdr_encode_pixel(0.6f, 0.6f, 0.6f, p);
    EXPECT_NEAR(mid[0] * 80.0f, srgb_to_linear(0.6f) * 200.0f, 1e-3f);
}

TEST(HdrEncode, ColoursKeepTheirHue) {
    const HdrParams p{HdrMode::ScRgb, 1000.0f, 203.0f};
    for (auto c : {std::array<float, 3>{1.0f, 0.9f, 0.7f}, std::array<float, 3>{0.95f, 1.0f, 0.95f}, std::array<float, 3>{0.2f, 0.4f, 0.9f}}) {
        const auto out = hdr_encode_pixel(c[0], c[1], c[2], p);
        const float a = srgb_to_linear(c[0]), b = srgb_to_linear(c[1]), d = srgb_to_linear(c[2]);
        // Equal gain on all three channels: ratios are those of the SDR colour.
        EXPECT_NEAR(out[1] / out[0], b / a, 1e-4f);
        EXPECT_NEAR(out[2] / out[0], d / a, 1e-4f);
    }
}

TEST(HdrEncode, NoChannelPassesThePeak) {
    const HdrParams p{HdrMode::ScRgb, 600.0f, 300.0f};
    for (float r : {0.0f, 0.5f, 1.0f})
        for (float g : {0.0f, 0.5f, 1.0f})
            for (float b : {0.0f, 0.5f, 1.0f}) {
                const auto out = hdr_encode_pixel(r, g, b, p);
                for (float c : out) EXPECT_LE(c * 80.0f, 600.0f + 0.01f);
            }
}

TEST(HdrEncode, PqPutsPaperWhiteAtItsCodeValue) {
    HdrParams p{HdrMode::Pq, 203.0f, 203.0f};
    for (float c : hdr_encode_pixel(1, 1, 1, p)) EXPECT_NEAR(c, pq_encode(203.0f), 2e-3f);  // Rec. 709 white is Rec. 2020 white
    p.peak_nits = 1000.0f;
    for (float c : hdr_encode_pixel(1, 1, 1, p)) EXPECT_NEAR(c, pq_encode(1000.0f), 2e-3f);
}

TEST(HdrEncode, EdrIsRelativeToPaperWhite) {
    const HdrParams p{HdrMode::Edr, 800.0f, 200.0f};
    EXPECT_NEAR(hdr_encode_pixel(0.6f, 0.6f, 0.6f, p)[0], srgb_to_linear(0.6f), 1e-5f);
    EXPECT_NEAR(hdr_encode_pixel(1, 1, 1, p)[0], 4.0f, 1e-4f);
}

TEST(HdrEdr, SwapChainModeFollowsTheLayerKind) {
    EXPECT_EQ(swap_chain_hdr_mode(true), HdrMode::Edr);
    EXPECT_EQ(swap_chain_hdr_mode(false), HdrMode::ScRgb);
}

TEST(HdrEdr, OutputScaleOfFlatContent) {
    EXPECT_FLOAT_EQ(hdr_output_scale({HdrMode::ScRgb, 1000.0f, 200.0f}), 2.5f);
    EXPECT_FLOAT_EQ(hdr_output_scale({HdrMode::Edr, 1000.0f, 200.0f}), 1.0f);
    EXPECT_FLOAT_EQ(hdr_output_scale({HdrMode::Off, 1000.0f, 200.0f}), 0.0f);
}

TEST(HdrEdr, PeakIsCutToTheDisplayHeadroom) {
    const HdrParams p{HdrMode::Edr, 1000.0f, 200.0f};
    EXPECT_FLOAT_EQ(edr_limited(p, 2.0f).peak_nits, 400.0f);
    EXPECT_FLOAT_EQ(edr_limited(p, 16.0f).peak_nits, 1000.0f);
    EXPECT_FLOAT_EQ(edr_limited(p, 0.5f).peak_nits, 200.0f);  // never below SDR white
    EXPECT_EQ(edr_limited(p, 2.0f).paper_white_nits, 200.0f);
}

TEST(HdrEdr, LimitedPeakKeepsWhiteInsideTheHeadroom) {
    const HdrParams p = edr_limited({HdrMode::Edr, 1000.0f, 200.0f}, 2.0f);
    EXPECT_NEAR(hdr_encode_pixel(1, 1, 1, p)[0], 2.0f, 1e-4f);
}

TEST(HdrEncode, OffPassesTheSdrValuesThrough) {
    const auto out = hdr_encode_pixel(0.25f, 0.5f, 0.75f, HdrParams{});
    EXPECT_FLOAT_EQ(out[0], 0.25f);
    EXPECT_FLOAT_EQ(out[2], 0.75f);
}

TEST(HdrGamut, Rec709WhiteStaysWhiteInRec2020) {
    const auto w = rec709_to_rec2020(1, 1, 1);
    for (float c : w) EXPECT_NEAR(c, 1.0f, 1e-3f);
    const auto red = rec709_to_rec2020(1, 0, 0);  // a Rec. 709 primary is inside the wider gamut
    EXPECT_GT(red[0], 0.6f);
    EXPECT_GT(red[1], 0.0f);
}
