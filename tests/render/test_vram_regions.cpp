#include <gtest/gtest.h>

#include "dw3/vram_regions.h"

using namespace dw3;

TEST(RegionFrom, PlainRectangle) {
    EXPECT_EQ(region_from({10, 20, 30, 40}), (Region{10, 20, 40, 60}));
}

TEST(RegionFrom, WrapWidensTheAxis) {
    EXPECT_EQ(region_from({1000, 5, 50, 4}), (Region{0, 5, 1024, 9}));
    EXPECT_EQ(region_from({3, 500, 8, 40}), (Region{3, 0, 11, 512}));
}

TEST(RegionFrom, NegativeAndLargeOrigins) {
    EXPECT_EQ(region_from({1024 + 16, 512 + 2, 4, 4}), (Region{16, 2, 20, 6}));
    EXPECT_EQ(region_from({-8, 0, 4, 4}), (Region{1016, 0, 1020, 4}));
}

TEST(RegionFrom, FullSizeCoversEverything) { EXPECT_EQ(region_from({}), kWholeVram); }

TEST(Region, IntersectionAndUnion) {
    const Region a{0, 0, 10, 10}, b{5, 5, 20, 20}, c{10, 0, 12, 10};
    EXPECT_TRUE(a.intersects(b));
    EXPECT_FALSE(a.intersects(c));  // half-open: touching edges do not overlap
    EXPECT_EQ(a.intersection(b), (Region{5, 5, 10, 10}));
    EXPECT_EQ(a.united(b), (Region{0, 0, 20, 20}));
    EXPECT_TRUE(Region{}.empty());
}

TEST(RegionSet, DisjointRegionsStaySeparate) {
    RegionSet s;
    s.add({0, 0, 10, 10});
    s.add({100, 100, 110, 110});
    EXPECT_EQ(s.regions().size(), 2u);
    EXPECT_TRUE(s.intersects({5, 5, 6, 6}));
    EXPECT_FALSE(s.intersects({50, 50, 60, 60}));
}

TEST(RegionSet, OverlappingRegionsMergeTransitively) {
    RegionSet s;
    s.add({0, 0, 10, 10});
    s.add({20, 0, 30, 10});
    s.add({8, 0, 22, 10});  // bridges both
    ASSERT_EQ(s.regions().size(), 1u);
    EXPECT_EQ(s.regions()[0], (Region{0, 0, 30, 10}));
}

TEST(RegionSet, TakeIntersectingRemovesOnlyThose) {
    RegionSet s;
    s.add({0, 0, 10, 10});
    s.add({100, 0, 110, 10});
    const auto taken = s.take_intersecting({5, 0, 6, 1});
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0], (Region{0, 0, 10, 10}));
    EXPECT_EQ(s.regions().size(), 1u);
    EXPECT_TRUE(s.take_intersecting({5, 0, 6, 1}).empty());
}

TEST(RegionSet, CollapsesToBoundingBoxPastCapacity) {
    RegionSet s;
    for (int i = 0; i < static_cast<int>(RegionSet::kMaxRegions) + 1; ++i) s.add({i * 20, i * 5, i * 20 + 4, i * 5 + 4});
    ASSERT_EQ(s.regions().size(), 1u);
    // Conservative: everything that was added is still covered.
    EXPECT_TRUE(s.intersects({0, 0, 4, 4}));
    EXPECT_TRUE(s.intersects({48 * 20, 48 * 5, 48 * 20 + 4, 48 * 5 + 4}));
}

TEST(RegionSet, EmptyRegionsAreIgnored) {
    RegionSet s;
    s.add({5, 5, 5, 9});
    EXPECT_TRUE(s.empty());
}
