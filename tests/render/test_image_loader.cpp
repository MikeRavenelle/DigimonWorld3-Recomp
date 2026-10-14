// Background decoding of replacement images and the planning of which map cells to decode ahead of time.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "dw3/texrep/field_backdrop.h"
#include "dw3/texrep/image_loader.h"

using namespace dw3::texrep;

namespace {

ImageLoader::Job solid_job(uint32_t colour, int size = 4) {
    return [=](Image& out, std::string&) {
        out.w = out.h = size;
        out.rgba.assign(static_cast<std::size_t>(size) * size, colour);
        return true;
    };
}

}  // namespace

TEST(ImageLoader, DeliversEveryJobOnceWithItsTicket) {
    ImageLoader loader(2);
    std::set<uint64_t> tickets;
    for (uint32_t i = 0; i < 20; ++i) tickets.insert(loader.submit(solid_job(i)));
    EXPECT_EQ(tickets.size(), 20u);
    loader.wait_idle();
    std::set<uint64_t> got;
    for (const ImageLoader::Result& r : loader.take(std::size_t(1) << 30)) {
        EXPECT_TRUE(r.ok);
        EXPECT_TRUE(got.insert(r.ticket).second);
    }
    EXPECT_EQ(got, tickets);
    EXPECT_EQ(loader.outstanding(), 0u);
}

TEST(ImageLoader, TakeStopsAtTheByteBudgetButAlwaysReturnsOne) {
    ImageLoader loader(1);
    for (int i = 0; i < 5; ++i) loader.submit(solid_job(1, 8));  // 256 bytes each
    loader.wait_idle();
    EXPECT_EQ(loader.take(600).size(), 2u);
    EXPECT_EQ(loader.take(1).size(), 1u) << "an image over the budget still comes out alone";
    EXPECT_EQ(loader.take(std::size_t(1) << 20).size(), 2u);
    EXPECT_TRUE(loader.take(std::size_t(1) << 20).empty());
}

TEST(ImageLoader, FailedJobsReportTheirError) {
    ImageLoader loader(1);
    const uint64_t ticket = loader.submit([](Image&, std::string& error) {
        error = "broken";
        return false;
    });
    loader.wait_idle();
    const auto results = loader.take(1 << 20);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].ticket, ticket);
    EXPECT_FALSE(results[0].ok);
    EXPECT_EQ(results[0].error, "broken");
}

TEST(ImageLoader, SubmitDoesNotWaitForTheJob) {
    ImageLoader loader(1);
    std::atomic<bool> release{false};
    loader.submit([&](Image&, std::string&) {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    });
    const auto before = std::chrono::steady_clock::now();
    loader.submit(solid_job(1));
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(500));
    EXPECT_TRUE(loader.take(1 << 20).empty());
    EXPECT_EQ(loader.outstanding(), 2u);
    release = true;
    loader.wait_idle();
    EXPECT_EQ(loader.take(1 << 20).size(), 2u);
}

TEST(ImageLoader, DestructionWithQueuedJobsDoesNotHang) {
    std::atomic<int> ran{0};
    {
        ImageLoader loader(1);
        for (int i = 0; i < 50; ++i)
            loader.submit([&](Image&, std::string&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                ++ran;
                return true;
            });
    }
    EXPECT_LT(ran.load(), 50);
}

namespace {

StageLibrary::Map grid_map(int cols, int rows) {
    StageLibrary::Map map;
    map.cols = cols;
    map.rows = rows;
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < cols; ++col) {
            StageLibrary::Tile tile;
            tile.col = col;
            tile.row = row;
            map.tiles.push_back(tile);
        }
    return map;
}

}  // namespace

TEST(TilesToPrefetch, ListsCellsWithinOneCellOfTheViewNearestFirst) {
    const StageLibrary::Map map = grid_map(8, 4);
    const BackdropRect view{0, 0, 320, 240};
    const std::vector<uint32_t> wanted = tiles_to_prefetch(map, 0, 0, view);
    EXPECT_EQ(wanted.size(), 12u) << "columns 0..3 and rows 0..2 reach into the view plus one cell";
    EXPECT_EQ(std::set<uint32_t>(wanted.begin(), wanted.end()).size(), wanted.size());
    EXPECT_EQ(map.tiles[wanted.front()].col, 1) << "the cell around the middle of the view comes first";
    EXPECT_EQ(map.tiles[wanted.front()].row, 0);
}

TEST(TilesToPrefetch, FollowsTheScroll) {
    const StageLibrary::Map map = grid_map(8, 4);
    const std::vector<uint32_t> wanted = tiles_to_prefetch(map, 5 * kStageCell, 0, {0, 0, 320, 240});
    ASSERT_FALSE(wanted.empty());
    for (const uint32_t t : wanted) EXPECT_GE(map.tiles[t].col, 4);
    EXPECT_TRUE(tiles_to_prefetch(map, 100 * kStageCell, 0, {0, 0, 320, 240}).empty());
}

TEST(TilesToPrefetch, WiderViewsReachFurther) {
    const StageLibrary::Map map = grid_map(8, 4);
    const auto narrow = tiles_to_prefetch(map, 0, 0, {0, 0, 320, 240});
    const auto wide = tiles_to_prefetch(map, 0, 0, {-128, 0, 448, 240});
    EXPECT_GT(wide.size(), narrow.size());
}

TEST(FloorDiv, RoundsTowardsMinusInfinity) {
    EXPECT_EQ(floor_div(-1, 128), -1);
    EXPECT_EQ(floor_div(-128, 128), -1);
    EXPECT_EQ(floor_div(-129, 128), -2);
    EXPECT_EQ(floor_div(127, 128), 0);
    EXPECT_EQ(floor_div(128, 128), 1);
}
