#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "display.h"

using namespace dw3app;

namespace {

bool contains(const std::vector<ScreenSize>& list, ScreenSize s) { return std::find(list.begin(), list.end(), s) != list.end(); }

}  // namespace

TEST(WindowedSizes, OnlySizesThatFitTheDesktopAreCommon) {
    const auto sizes = windowed_sizes({1920, 1080}, {1280, 960});
    EXPECT_TRUE(contains(sizes, {960, 720}));
    EXPECT_TRUE(contains(sizes, {1280, 960}));
    EXPECT_TRUE(contains(sizes, {1920, 1080}));
    EXPECT_TRUE(contains(sizes, {1280, 720}));
    EXPECT_FALSE(contains(sizes, {1600, 1200}));  // taller than the desktop
    EXPECT_FALSE(contains(sizes, {2560, 1440}));
    for (const ScreenSize s : sizes) {
        EXPECT_LE(s.w, 1920);
        EXPECT_LE(s.h, 1080);
    }
}

TEST(WindowedSizes, BigDesktopOffersFourThreeSizesUpToTheLimit) {
    const auto sizes = windowed_sizes({3840, 2160}, {1280, 960});
    EXPECT_TRUE(contains(sizes, {1600, 1200}));
    EXPECT_TRUE(contains(sizes, {1920, 1440}));
    EXPECT_TRUE(contains(sizes, {3840, 2160}));
    EXPECT_FALSE(contains(sizes, {3200, 2400}));  // 2400 > 2160
}

TEST(WindowedSizes, SortedSmallestFirstWithoutDuplicates) {
    const auto sizes = windowed_sizes({2560, 1600}, {1280, 960});
    for (std::size_t i = 1; i < sizes.size(); ++i) EXPECT_LE(sizes[i - 1].w * sizes[i - 1].h, sizes[i].w * sizes[i].h);
    EXPECT_EQ(std::count(sizes.begin(), sizes.end(), ScreenSize{1280, 960}), 1);
}

TEST(WindowedSizes, AnUncommonSavedSizeStaysSelectable) {
    const auto sizes = windowed_sizes({1920, 1080}, {1111, 777});
    EXPECT_TRUE(contains(sizes, {1111, 777}));
    EXPECT_EQ(sizes[nearest_size(sizes, {1111, 777})], (ScreenSize{1111, 777}));
}

TEST(WindowedSizes, TinyDesktopStillOffersTheCurrentSize) {
    const auto sizes = windowed_sizes({400, 300}, {1280, 960});
    ASSERT_EQ(sizes.size(), 1u);
    EXPECT_EQ(sizes[0], (ScreenSize{1280, 960}));
}

TEST(ExclusiveModes, DropsTinyAndDuplicateModesAndSorts) {
    const ScreenMode reported[] = {
        {1920, 1080, 14400}, {1920, 1080, 6000}, {1920, 1080, 5994}, {800, 600, 6000}, {320, 240, 6000},
        {1280, 720, 6000},   {1920, 1080, 14400}, {1920, 1080, 6000},
    };
    const auto modes = exclusive_modes(reported);
    // 320x240 is dropped, 59.94 and 60 Hz are one mode, the repeated modes appear once.
    ASSERT_EQ(modes.size(), 4u);
    EXPECT_EQ(modes[0], (ScreenMode{800, 600, 6000}));
    EXPECT_EQ(modes[1], (ScreenMode{1280, 720, 6000}));
    EXPECT_EQ(modes[2], (ScreenMode{1920, 1080, 6000}));
    EXPECT_EQ(modes[3], (ScreenMode{1920, 1080, 14400}));
}

TEST(ExclusiveModes, EmptyInputGivesEmptyList) {
    EXPECT_TRUE(exclusive_modes({}).empty());
    EXPECT_EQ(nearest_mode({}, {1920, 1080, 6000}), 0u);
}

TEST(ExclusiveModes, NearestPrefersTheSameSizeThenTheClosestRefresh) {
    const ScreenMode list[] = {{1280, 720, 6000}, {1920, 1080, 6000}, {1920, 1080, 14400}, {2560, 1440, 14400}};
    EXPECT_EQ(nearest_mode(list, {1920, 1080, 14400}), 2u);
    EXPECT_EQ(nearest_mode(list, {1920, 1080, 12000}), 2u);
    EXPECT_EQ(nearest_mode(list, {1920, 1080, 7000}), 1u);
    EXPECT_EQ(nearest_mode(list, {1900, 1000, 6000}), 1u);
}

TEST(DisplayLabels, SizesAndModesReadAsText) {
    EXPECT_EQ(size_label({1280, 960}), "1280x960");
    EXPECT_EQ(mode_label({1920, 1080, 14400}), "1920x1080 144");
    EXPECT_EQ(mode_label({1920, 1080, 5994}), "1920x1080 60");
}

TEST(MonitorListTest, AutoMeansTheWindowsMonitorAndBadNumbersFallBackToIt) {
    MonitorList list;
    EXPECT_EQ(list.effective(0), nullptr);
    list.monitors = {{"A", {1920, 1080}, {1920, 1080, 6000}, {}}, {"B", {2560, 1440}, {2560, 1440, 14400}, {}}};
    list.window_monitor = 1;
    EXPECT_EQ(list.effective(0)->name, "B");
    EXPECT_EQ(list.effective(1)->name, "A");
    EXPECT_EQ(list.effective(2)->name, "B");
    EXPECT_EQ(list.effective(7)->name, "B");  // unplugged: auto
}

TEST(FramePacing, WholeMultiplesOfSixtyAreEven) {
    EXPECT_EQ(refreshes_per_frame(60.0), 1);
    EXPECT_EQ(refreshes_per_frame(59.94), 1);
    EXPECT_EQ(refreshes_per_frame(120.0), 2);
    EXPECT_EQ(refreshes_per_frame(119.88), 2);
    EXPECT_EQ(refreshes_per_frame(180.0), 3);
    EXPECT_EQ(refreshes_per_frame(240.0), 4);
    EXPECT_EQ(refreshes_per_frame(239.76), 4);
}

TEST(FramePacing, OtherRatesCannotBePacedEvenly) {
    EXPECT_EQ(refreshes_per_frame(144.0), 0);
    EXPECT_EQ(refreshes_per_frame(75.0), 0);
    EXPECT_EQ(refreshes_per_frame(100.0), 0);
    EXPECT_EQ(refreshes_per_frame(165.0), 0);
    EXPECT_EQ(refreshes_per_frame(30.0), 0);
    EXPECT_EQ(refreshes_per_frame(0.0), 0);
    EXPECT_EQ(refreshes_per_frame(600.0), 0);  // too many refreshes per frame to be worth it
}

TEST(GammaMaths, NeutralValuesChangeNothing) {
    for (float c : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) EXPECT_NEAR(gamma_brightness_curve(c, 1.0f, 0.0f), c, 1e-6f);
    EXPECT_FLOAT_EQ(gamma_from_percent(100), 1.0f);
    EXPECT_FLOAT_EQ(brightness_from_percent(0), 0.0f);
}

TEST(GammaMaths, HigherGammaBrightensTheMidtonesButKeepsTheEnds) {
    const float mid = 0.5f;
    EXPECT_GT(gamma_brightness_curve(mid, 1.4f, 0.0f), mid);
    EXPECT_LT(gamma_brightness_curve(mid, 0.8f, 0.0f), mid);
    EXPECT_NEAR(gamma_brightness_curve(mid, 1.4f, 0.0f), std::pow(0.5f, 1.0f / 1.4f), 1e-6f);
    EXPECT_FLOAT_EQ(gamma_brightness_curve(0.0f, 1.4f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(gamma_brightness_curve(1.0f, 0.8f, 0.0f), 1.0f);
}

TEST(GammaMaths, BrightnessIsAddedAndClamped) {
    EXPECT_NEAR(gamma_brightness_curve(0.5f, 1.0f, 0.1f), 0.6f, 1e-6f);
    EXPECT_NEAR(gamma_brightness_curve(0.5f, 1.0f, -0.1f), 0.4f, 1e-6f);
    EXPECT_FLOAT_EQ(gamma_brightness_curve(0.95f, 1.0f, 0.25f), 1.0f);
    EXPECT_FLOAT_EQ(gamma_brightness_curve(0.05f, 1.0f, -0.25f), 0.0f);
}

TEST(GammaMaths, SettingsPercentConvertToTheShaderValues) {
    EXPECT_FLOAT_EQ(gamma_from_percent(80), 0.8f);
    EXPECT_FLOAT_EQ(gamma_from_percent(140), 1.4f);
    EXPECT_FLOAT_EQ(brightness_from_percent(50), 0.25f);
    EXPECT_FLOAT_EQ(brightness_from_percent(-50), -0.25f);
}
