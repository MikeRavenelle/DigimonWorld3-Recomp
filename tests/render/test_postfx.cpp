// Post-processing passes of the GPU renderer, run on a synthetic picture on every graphics API the build contains.
// Without a window the picture is drawn into an offscreen image (HwRenderer::render_to_image), which goes through the
// same passes as presentation.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#if DW3_HW_RENDERER
#include "dw3/hw_renderer.h"
#include "dw3/postfx.h"

namespace {

using namespace dw3;

struct GpuParam {
    GraphicsApi api;
    int scale;
};

const char* api_name(GraphicsApi api) {
    return api == GraphicsApi::D3D12 ? "D3D12" : api == GraphicsApi::Metal ? "Metal" : "Vulkan";
}

void PrintTo(const GpuParam& p, std::ostream* os) { *os << api_name(p.api) << " " << p.scale << "x"; }

std::vector<GpuParam> params() {
    std::vector<GpuParam> out;
    for (GraphicsApi api : {GraphicsApi::Vulkan, GraphicsApi::D3D12, GraphicsApi::Metal})
        if (HwRenderer::api_compiled_in(api))
            for (int scale : {1, 2}) out.push_back({api, scale});
    return out;
}

std::string param_name(const ::testing::TestParamInfo<GpuParam>& info) {
    return std::string(api_name(info.param.api)) + "_" + std::to_string(info.param.scale) + "x";
}

constexpr int kW = 320, kH = 240;

uint16_t word(int r, int g, int b) { return static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }

int channel(uint32_t px, int c) { return static_cast<int>((px >> (8 * c)) & 255); }

float luma(uint32_t px) { return 0.2126f * channel(px, 0) + 0.7152f * channel(px, 1) + 0.0722f * channel(px, 2); }

class PostFxTest : public ::testing::TestWithParam<GpuParam> {
protected:
    void SetUp() override {
        std::string error;
        HwOptions options;
        options.api = GetParam().api;
        hw = HwRenderer::create(nullptr, {}, error, GetParam().scale, options);
        if (!hw) GTEST_SKIP() << "no GPU device: " << error;
        display.enabled = true;
        display.width = kW;
        display.height = kH;
        display.vram_x = display.vram_y = 0;
    }

    // Fills the display area with `pixel(x, y)` and uploads it.
    template <typename F>
    void load(F pixel) {
        std::vector<uint16_t> vram(kVramWidth * kVramHeight, 0);
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) vram[y * kVramWidth + x] = pixel(x, y);
        hw->vram_written(vram, {});
    }

    std::vector<uint32_t> render(const PostFx* post, int w = kW, int h = kH, bool linear = false) {
        std::vector<uint32_t> img;
        EXPECT_TRUE(hw->render_to_image(display, ScaleMode::Stretch, linear, post, w, h, img));
        EXPECT_EQ(img.size(), static_cast<std::size_t>(w) * h);
        return img;
    }

    std::unique_ptr<HwRenderer> hw;
    DisplayState display;
};

// A black and white staircase edge: x + y/2 < 160 is white.
uint16_t staircase(int x, int y) { return (2 * x + y < 320) ? word(255, 255, 255) : word(0, 0, 0); }

uint16_t gradient(int x, int y) { return word(x * 255 / (kW - 1), y * 255 / (kH - 1), 128); }

int count_grey(const std::vector<uint32_t>& img) {
    int n = 0;
    for (uint32_t p : img) {
        const int v = channel(p, 0);
        n += v > 16 && v < 240;
    }
    return n;
}

TEST_P(PostFxTest, InactiveOptionsGiveTheUnprocessedPicture) {
    load(gradient);
    const std::vector<uint32_t> plain = render(nullptr);
    PostFx off;
    EXPECT_FALSE(off.active());
    EXPECT_EQ(plain, render(&off));
    PostFx neutral;
    neutral.grade = GradeParams();
    EXPECT_EQ(plain, render(&neutral));
}

TEST_P(PostFxTest, ScalingMatchesThePlainPathAtOneToOne) {
    load(gradient);
    // Colour grading with an identity-like look still runs the passes (gamma differs by a hair), so compare a pass that
    // is the identity: sharpening of a flat area.
    load([](int, int) { return word(120, 80, 200); });
    PostFx fx;
    fx.sharpness = 0.5f;
    const auto plain = render(nullptr);
    const auto sharp = render(&fx);
    for (std::size_t i = 0; i < plain.size(); ++i) ASSERT_EQ(plain[i] & 0xFFFFFF, sharp[i] & 0xFFFFFF) << i;
}

TEST_P(PostFxTest, SharpeningIncreasesContrastAtASoftEdge) {
    load([](int x, int) { return x < 158 ? word(96, 96, 96) : x < 162 ? word(128, 128, 128) : word(160, 160, 160); });
    PostFx fx;
    fx.sharpness = 0.8f;
    const auto plain = render(nullptr);
    const auto sharp = render(&fx);
    auto spread = [](const std::vector<uint32_t>& img) {
        float lo = 255, hi = 0;
        for (int x = 150; x < 170; ++x) {
            lo = std::min(lo, luma(img[100 * kW + x]));
            hi = std::max(hi, luma(img[100 * kW + x]));
        }
        return hi - lo;
    };
    EXPECT_GT(spread(sharp), spread(plain));
}

TEST_P(PostFxTest, WeakestSharpeningStaysClose) {
    load(gradient);
    PostFx fx;
    fx.sharpness = 0.01f;
    const auto plain = render(nullptr);
    const auto sharp = render(&fx);
    int worst = 0;
    for (std::size_t i = 0; i < plain.size(); ++i)
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(channel(plain[i], c) - channel(sharp[i], c)));
    EXPECT_LE(worst, 3);
}

TEST_P(PostFxTest, VividGradingRaisesSaturation) {
    load([](int, int) { return word(180, 120, 100); });
    PostFx fx;
    fx.grade.saturation = 1.2f;
    fx.grade.vibrance = 0.3f;
    const auto plain = render(nullptr);
    const auto vivid = render(&fx);
    auto chroma = [](uint32_t p) { return std::max({channel(p, 0), channel(p, 1), channel(p, 2)}) - std::min({channel(p, 0), channel(p, 1), channel(p, 2)}); };
    EXPECT_GT(chroma(vivid[100 * kW + 100]), chroma(plain[100 * kW + 100]));
}

TEST_P(PostFxTest, GradingWithIdentityValuesIsTheIdentity) {
    load(gradient);
    PostFx fx;
    fx.grade.tint[0] = 1.0001f;  // enough to run the pass, not enough to show
    const auto plain = render(nullptr);
    const auto graded = render(&fx);
    int worst = 0;
    for (std::size_t i = 0; i < plain.size(); ++i)
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(channel(plain[i], c) - channel(graded[i], c)));
    EXPECT_LE(worst, 1);
}

TEST_P(PostFxTest, GammaFollowsTheCurveOnEveryChannel) {
    load([](int, int) { return word(120, 80, 200); });
    const auto plain = render(nullptr);
    for (const float gamma : {0.8f, 1.4f}) {
        PostFx fx;
        fx.grade.gamma = gamma;
        const auto graded = render(&fx);
        for (int c = 0; c < 3; ++c) {
            const float in = static_cast<float>(channel(plain[100 * kW + 100], c)) / 255.0f;
            EXPECT_NEAR(channel(graded[100 * kW + 100], c), 255.0f * std::pow(in, 1.0f / gamma), 2.0f) << "gamma " << gamma << " channel " << c;
        }
    }
}

TEST_P(PostFxTest, BrightnessIsAddedAndClamped) {
    load([](int, int) { return word(120, 80, 248); });
    const auto plain = render(nullptr);
    PostFx fx;
    fx.grade.brightness = 0.1f;
    const auto brighter = render(&fx);
    EXPECT_NEAR(channel(brighter[100 * kW + 100], 0), channel(plain[100 * kW + 100], 0) + 25.5f, 2.0f);
    EXPECT_NEAR(channel(brighter[100 * kW + 100], 1), channel(plain[100 * kW + 100], 1) + 25.5f, 2.0f);
    EXPECT_EQ(channel(brighter[100 * kW + 100], 2), 255);
    fx.grade.brightness = -0.1f;
    const auto darker = render(&fx);
    EXPECT_NEAR(channel(darker[100 * kW + 100], 0), channel(plain[100 * kW + 100], 0) - 25.5f, 2.0f);
}

TEST_P(PostFxTest, FsrProducesTheWindowSizedPicture) {
    load(staircase);
    PostFx fx;
    fx.scaler = OutputScaler::Fsr;
    fx.sharpness = 0.5f;
    const int w = 1000, h = 750;
    const auto img = render(&fx, w, h);
    ASSERT_EQ(img.size(), static_cast<std::size_t>(w) * h);
    // Both sides of the edge keep their level and the edge itself stays one transition wide.
    EXPECT_GT(luma(img[100 * w + 100]), 240.0f);
    EXPECT_LT(luma(img[(h - 100) * w + (w - 100)]), 16.0f);
    const auto linear = render(nullptr, w, h, true);
    EXPECT_NE(img, linear);
}

TEST_P(PostFxTest, FsrFallsBackToLinearWhenNotEnlarging) {
    load(gradient);
    PostFx fx;
    fx.scaler = OutputScaler::Fsr;
    const auto img = render(&fx, kW / 2, kH / 2);
    const auto linear = render(nullptr, kW / 2, kH / 2, true);
    int worst = 0;
    for (std::size_t i = 0; i < img.size(); ++i)
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(channel(img[i], c) - channel(linear[i], c)));
    EXPECT_LE(worst, 1);
}

TEST_P(PostFxTest, SmaaSoftensAStaircaseEdge) {
    load(staircase);
    const int scale = GetParam().scale;
    PostFx fx;
    fx.smaa = true;
    // At the internal resolution of the picture (no scaling) the edge has hard steps; SMAA adds intermediate levels.
    const int w = kW * scale, h = kH * scale;
    const auto plain = render(nullptr, w, h);
    const auto smoothed = render(&fx, w, h);
    EXPECT_EQ(count_grey(plain), 0);
    EXPECT_GT(count_grey(smoothed), 100);
    // Flat areas are untouched.
    EXPECT_EQ(smoothed[10] & 0xFFFFFF, plain[10] & 0xFFFFFF);
    EXPECT_EQ(smoothed[(h - 10) * w + w - 10] & 0xFFFFFF, plain[(h - 10) * w + w - 10] & 0xFFFFFF);
}

TEST_P(PostFxTest, SmaaLeavesAFlatPictureAlone) {
    load([](int, int) { return word(10, 200, 90); });
    PostFx fx;
    fx.smaa = true;
    EXPECT_EQ(render(nullptr), render(&fx));
}

TEST_P(PostFxTest, CrtScanlinesAlternateBrightness) {
    load([](int, int) { return word(200, 200, 200); });
    PostFx fx;
    fx.crt.scanlines = 1.0f;
    const int w = 640, h = 960;  // four window rows per console line
    const auto img = render(&fx, w, h);
    float lo = 255, hi = 0;
    for (int y = 200; y < 208; ++y) {
        lo = std::min(lo, luma(img[y * w + 300]));
        hi = std::max(hi, luma(img[y * w + 300]));
    }
    EXPECT_GT(hi - lo, 15.0f);
    // Without scanlines the same flat picture is flat.
    PostFx mask_only;
    mask_only.crt.vignette = 0.01f;
    const auto flat = render(&mask_only, w, h);
    EXPECT_LT(std::abs(luma(flat[200 * w + 300]) - luma(flat[204 * w + 300])), 2.0f);
}

TEST_P(PostFxTest, CrtCurvatureBlanksTheCorners) {
    load([](int, int) { return word(255, 255, 255); });
    PostFx fx;
    fx.crt.curvature = 1.0f;
    const auto img = render(&fx, 640, 480);
    EXPECT_LT(luma(img[0]), 8.0f);
    EXPECT_GT(luma(img[240 * 640 + 320]), 200.0f);
}

TEST_P(PostFxTest, AllPassesTogetherRun) {
    load(gradient);
    PostFx fx;
    fx.smaa = true;
    fx.sharpness = 0.5f;
    fx.grade.saturation = 1.1f;
    fx.scaler = OutputScaler::Fsr;
    fx.crt.scanlines = 0.5f;
    fx.crt.mask = 0.3f;
    const auto img = render(&fx, 800, 600);
    EXPECT_GT(luma(img[300 * 800 + 400]), 20.0f);
}

INSTANTIATE_TEST_SUITE_P(Api, PostFxTest, ::testing::ValuesIn(params()), param_name);

}  // namespace
#endif
