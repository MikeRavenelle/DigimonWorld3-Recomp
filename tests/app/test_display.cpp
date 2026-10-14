#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "display.h"
#include "fastforward.h"

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

namespace {

constexpr int64_t kFrameNs = 16683000;  // one 59.94 Hz frame

// Plays frames against a display that completes a present every `gap_for_frame(f)` ns while it reports
// `reported_hz`. Emulation falls behind real time when a frame's presents last longer than a real-time frame.
struct CadenceRun {
    PresentCadence cadence;
    std::vector<int> repeats;
    int64_t lag_ns = 0;

    void play(int frames, double reported_hz, const std::function<int64_t(int)>& gap_for_frame) {
        cadence.set_enabled(true);
        cadence.set_reported_hz(reported_hz);
        for (int f = 0; f < frames; ++f) {
            const int presents = cadence.plan_frame();
            const int64_t gap = gap_for_frame(f);
            for (int i = 1; i < presents; ++i) cadence.on_present_gap(gap);
            lag_ns = std::max<int64_t>(0, lag_ns + presents * gap - kFrameNs);
            if (cadence.end_frame(lag_ns)) lag_ns = 0;
            repeats.push_back(presents);
        }
    }
};

}  // namespace

TEST(PresentCadence, RepeatsForMeasuredGaps) {
    EXPECT_EQ(PresentCadence::repeats_for_gap(8333000), 2);
    EXPECT_EQ(PresentCadence::repeats_for_gap(5555000), 3);
    EXPECT_EQ(PresentCadence::repeats_for_gap(4167000), 4);
    EXPECT_EQ(PresentCadence::repeats_for_gap(16667000), 1);
    EXPECT_EQ(PresentCadence::repeats_for_gap(6944000), 1);  // 144 Hz
    EXPECT_EQ(PresentCadence::repeats_for_gap(0), 1);
}

TEST(PresentCadence, RealOneTwentyDoublesEveryFrame) {
    CadenceRun run;
    run.play(300, 120.0, [](int) { return 8333000LL; });
    EXPECT_EQ(run.repeats.back(), 2);
    EXPECT_EQ(run.cadence.repeats(), 2);
}

TEST(PresentCadence, ReportedOneTwentyDeliveredSixtyStaysSingle) {
    CadenceRun run;
    run.play(2000, 119.93, [](int) { return 16683000LL; });
    EXPECT_EQ(run.cadence.repeats(), 1);
    EXPECT_LE(std::count(run.repeats.begin(), run.repeats.end(), 2), 8);  // probes back off
    EXPECT_EQ(run.repeats.back(), 1);
}

TEST(PresentCadence, ReportedSixtyNeverRepeats) {
    CadenceRun run;
    run.play(500, 59.94, [](int) { return 8333000LL; });
    EXPECT_EQ(std::count(run.repeats.begin(), run.repeats.end(), 1), 500);
}

TEST(PresentCadence, VariableRefreshJitterKeepsTheRepeats) {
    CadenceRun run;
    run.play(400, 120.0, [](int f) { return (7600 + (f * 37) % 1100) * 1000LL; });
    EXPECT_EQ(run.cadence.repeats(), 2);
}

TEST(PresentCadence, RefreshChangeMidRunFallsBackAndComesBack) {
    CadenceRun run;
    run.play(300, 120.0, [](int) { return 8333000LL; });
    ASSERT_EQ(run.cadence.repeats(), 2);
    // A frame limiter turns on: presents take 16.7 ms although 120 Hz is still reported.
    run.play(40, 120.0, [](int) { return 16683000LL; });
    EXPECT_EQ(run.cadence.repeats(), 1);
    // The limiter turns off; a later probe finds 120 Hz again.
    run.play(1000, 120.0, [](int) { return 8333000LL; });
    EXPECT_EQ(run.cadence.repeats(), 2);
}

TEST(PresentCadence, FallsBackAtOnceWhenEmulationFallsBehind) {
    PresentCadence cadence;
    cadence.set_enabled(true);
    cadence.set_reported_hz(120.0);
    for (int i = 0; i < PresentCadence::kFirstWaitFrames; ++i) cadence.plan_frame();
    cadence.on_present_gap(8333000);
    cadence.end_frame(0);
    ASSERT_EQ(cadence.repeats(), 2);
    EXPECT_TRUE(cadence.end_frame(PresentCadence::kMaxLagNs + 1));
    EXPECT_EQ(cadence.repeats(), 1);
    EXPECT_EQ(cadence.plan_frame(), 1);
}

TEST(PresentCadence, DisabledPacingNeverRepeats) {
    PresentCadence cadence;
    cadence.set_reported_hz(120.0);
    for (int i = 0; i < 500; ++i) EXPECT_EQ(cadence.plan_frame(), 1);
    cadence.set_enabled(true);
    for (int i = 0; i < 200; ++i) cadence.plan_frame();
    cadence.set_enabled(false);
    EXPECT_EQ(cadence.plan_frame(), 1);
    EXPECT_EQ(cadence.repeats(), 1);
}

TEST(Pacer, KeepsRealTimeWhenTheDisplayIsAtLeastAsFast) {
    Pacer pacer;
    pacer.reset(0, 0);
    uint64_t now = 0;
    int64_t cycles = 0;
    constexpr int64_t kFrameCycles = 564480;  // one 59.94 Hz frame
    for (int f = 0; f < 3600; ++f) {
        cycles += kFrameCycles;
        now += 16667000;  // a 60.00 Hz display with vsync on delivers the present
        now += pacer.delay_ns(now, cycles);
    }
    EXPECT_NEAR(static_cast<double>(cycles) / Pacer::kCpuHz, static_cast<double>(now) / 1e9, 0.01);
}
