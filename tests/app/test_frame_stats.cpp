#include <gtest/gtest.h>

#include "frame_stats.h"

using namespace dw3app;

TEST(FrameStats, PercentilesUseNearestRank) {
    FrameStatSeries s;
    for (int i = 1; i <= 100; ++i) s.add(i * 1'000'000);
    EXPECT_EQ(s.count(), 100u);
    EXPECT_EQ(s.percentile(0.50), 50'000'000);
    EXPECT_EQ(s.percentile(0.95), 95'000'000);
    EXPECT_EQ(s.percentile(0.99), 99'000'000);
    EXPECT_EQ(s.percentile(1.0), 100'000'000);
    EXPECT_EQ(s.max(), 100'000'000);
    EXPECT_EQ(s.max_index(), 99u);
}

TEST(FrameStats, CountsFramesOverTheBudgets) {
    FrameStatSeries s;
    for (int64_t ms : {1, 10, 16, 17, 20, 33, 34, 100}) s.add(ms * 1'000'000);
    EXPECT_EQ(s.count_over(kFrameBudget60Ns), 5u);
    EXPECT_EQ(s.count_over(kFrameBudget30Ns), 2u);
}

TEST(FrameStats, EmptySeriesReadsAsZero) {
    FrameStatSeries s;
    EXPECT_EQ(s.percentile(0.99), 0);
    EXPECT_EQ(s.max(), 0);
    EXPECT_EQ(s.count_over(0), 0u);
}

TEST(FrameStats, ReportListsEachSeriesOnce) {
    FrameStats stats;
    stats.series("cpu").add(2'000'000);
    stats.series("cpu").add(40'000'000);
    stats.series("present").add(1'000'000);
    const std::string text = stats.report();
    EXPECT_NE(text.find("cpu"), std::string::npos);
    EXPECT_NE(text.find("n=2"), std::string::npos);
    EXPECT_NE(text.find("over 33 ms: 1"), std::string::npos);
    EXPECT_EQ(text.find("cpu", text.find("cpu") + 1), std::string::npos);
    EXPECT_NE(text.find("present"), std::string::npos);
}
