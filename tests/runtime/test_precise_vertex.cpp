// Sub-pixel vertex tracking: the cache itself, the GTE projections that feed it and the GPU front end that
// consumes it.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "dw3/dma.h"
#include "dw3/gpu.h"
#include "dw3/gte.h"
#include "dw3/irq.h"
#include "dw3/precise_vertex.h"
#include "dw3/video_timing.h"
#include "hw_test_util.h"

using namespace dw3;

namespace {

constexpr uint32_t pk(int x, int y) { return (static_cast<uint32_t>(x) & 0xFFFFu) | (static_cast<uint32_t>(y) << 16); }

}  // namespace

// ---------------------------------------------------------------- cache

TEST(PreciseVertexCache, HitReturnsTheRecordedPosition) {
    PreciseVertexCache c;
    c.record(10, -20, 10.25, -19.5, 300.0);
    PreciseVertex v;
    ASSERT_TRUE(c.find(pk(10, -20), v));
    EXPECT_FLOAT_EQ(v.x, 10.25f);
    EXPECT_FLOAT_EQ(v.y, -19.5f);
    EXPECT_FLOAT_EQ(v.w, 300.0f);
    EXPECT_EQ(c.hits(), 1u);
}

TEST(PreciseVertexCache, MissOnAnUnknownWordOrAChangedWord) {
    PreciseVertexCache c;
    c.record(10, 20, 10.5, 20.5, 100.0);
    PreciseVertex v;
    EXPECT_FALSE(c.find(pk(11, 20), v));  // the game moved the vertex
    EXPECT_FALSE(c.find(pk(10, 21), v));
    EXPECT_FALSE(c.find(0, v));
    EXPECT_EQ(c.misses(), 3u);
}

TEST(PreciseVertexCache, OnlyTheElevenBitFieldsTheGpuReadsAreTheKey) {
    PreciseVertexCache c;
    c.record(-5, 7, -4.75, 7.25, 50.0);
    PreciseVertex v;
    // Bits the GPU ignores (sign extension, flags in the unused bits) do not matter.
    EXPECT_TRUE(c.find(pk(-5, 7), v));
    EXPECT_TRUE(c.find(pk(-5, 7) ^ 0x0000F800u ^ 0xF8000000u, v));
}

TEST(PreciseVertexCache, NewerRecordReplacesTheSameWord) {
    PreciseVertexCache c;
    c.record(3, 4, 3.1, 4.1, 10.0);
    c.record(3, 4, 3.9, 4.9, 11.0);
    PreciseVertex v;
    ASSERT_TRUE(c.find(pk(3, 4), v));
    EXPECT_FLOAT_EQ(v.x, 3.9f);
    EXPECT_FLOAT_EQ(v.w, 11.0f);
}

TEST(PreciseVertexCache, RecordIsRefusedWhenTheIntegerIsNotTheTruncatedPosition) {
    PreciseVertexCache c;
    PreciseVertex v;
    c.record(10, 10, 11.0, 10.5, 5.0);   // x is not within [10, 11)
    c.record(10, 10, 10.5, 9.99, 5.0);   // y below the integer
    c.record(10, 10, 10.5, 10.5, 0.0);   // no depth
    c.record(10, 10, 10.5, 10.5, -1.0);
    EXPECT_FALSE(c.find(pk(10, 10), v));
    c.record(-1, -1, -0.5, -0.25, 5.0);  // negative positions truncate toward minus infinity
    EXPECT_TRUE(c.find(pk(-1, -1), v));
}

TEST(PreciseVertexCache, EntriesExpireAfterTheirFrames) {
    uint64_t frame = 100;
    PreciseVertexCache c;
    c.set_clock([&] { return frame; });
    c.record(1, 2, 1.5, 2.5, 9.0);
    PreciseVertex v;
    EXPECT_TRUE(c.find(pk(1, 2), v));
    frame = 103;
    EXPECT_TRUE(c.find(pk(1, 2), v));  // the game draws up to three video frames after the projection
    frame = 104;
    EXPECT_FALSE(c.find(pk(1, 2), v));  // stale
    c.record(1, 2, 1.25, 2.25, 9.0);    // refreshed
    EXPECT_TRUE(c.find(pk(1, 2), v));
    EXPECT_FLOAT_EQ(v.x, 1.25f);
}

TEST(PreciseVertexCache, ASlowlyChangingDepthKeepsTheEntry) {
    uint64_t frame = 100;
    PreciseVertexCache c;
    c.set_clock([&] { return frame; });
    c.record(1, 2, 1.5, 2.5, 1000.0);
    frame = 101;
    c.record(1, 2, 1.6, 2.5, 1100.0);
    PreciseVertex v;
    EXPECT_TRUE(c.find(pk(1, 2), v));
    EXPECT_FLOAT_EQ(v.x, 1.6f);
}

TEST(PreciseVertexCache, PixelsSharedByVerticesOfVeryDifferentDepthAreNotReturned) {
    uint64_t frame = 100;
    PreciseVertexCache c;
    c.set_clock([&] { return frame; });
    c.record(1, 2, 1.5, 2.5, 1000.0);
    c.record(1, 2, 1.9, 2.1, 9000.0);  // another vertex, much farther away
    PreciseVertex v;
    EXPECT_FALSE(c.find(pk(1, 2), v));
    frame = 110;  // the contest is over once the entries are old
    c.record(1, 2, 1.7, 2.3, 9000.0);
    EXPECT_TRUE(c.find(pk(1, 2), v));
}

TEST(PreciseVertexCache, ClearInvalidatesEverything) {
    PreciseVertexCache c;
    for (int i = 0; i < 50; ++i) c.record(i, i, i + 0.5, i + 0.5, 1.0);
    c.clear();
    PreciseVertex v;
    for (int i = 0; i < 50; ++i) EXPECT_FALSE(c.find(pk(i, i), v));
}

TEST(PreciseVertexCache, CollidingWordsNeverReturnAnotherWordsPosition) {
    PreciseVertexCache c;
    std::mt19937 rng(7);
    std::vector<std::pair<uint32_t, float>> all;
    for (int i = 0; i < 40000; ++i) {
        int x = static_cast<int>(rng() % 2048) - 1024, y = static_cast<int>(rng() % 2048) - 1024;
        c.record(x, y, x + 0.5, y + 0.5, 1.0);
        all.push_back({pk(x, y), static_cast<float>(x) + 0.5f});
    }
    int found = 0;
    for (const auto& [word, px] : all) {
        PreciseVertex v;
        if (!c.find(word, v)) continue;  // evicted
        ++found;
        EXPECT_FLOAT_EQ(v.x, px);
    }
    EXPECT_GT(found, 1000);
}

// ---------------------------------------------------------------- GTE

namespace {

struct GteRig {
    Gte g;
    PreciseVertexCache cache;
    GteRig() {
        g.set_precise_sink(&cache);
        // Identity rotation, H = 0x1000, no offsets.
        g.write_ctrl(0, pk(0x1000, 0));
        g.write_ctrl(1, pk(0, 0));
        g.write_ctrl(2, pk(0x1000, 0));
        g.write_ctrl(3, pk(0, 0));
        g.write_ctrl(4, 0x1000);
        g.write_ctrl(26, 0x1000);
    }
    void vec(int n, int x, int y, int z) {
        g.write_data(static_cast<unsigned>(n) * 2, pk(x, y));
        g.write_data(static_cast<unsigned>(n) * 2 + 1, static_cast<uint32_t>(z));
    }
    static constexpr uint32_t kRtps = 0x4A080001u, kRtpt = 0x4A080030u;
    static int sx(uint32_t w) { return static_cast<int16_t>(w & 0xFFFF); }
    static int sy(uint32_t w) { return static_cast<int16_t>(w >> 16); }
};

}  // namespace

TEST(PreciseGte, RtpsRecordsTheFractionBehindTheTruncatedWord) {
    GteRig r;
    r.vec(0, 0x100, 0x200, 0x3000);  // x * H / SZ = 256 / 3 = 85.33, 512 / 3 = 170.67
    r.g.execute(GteRig::kRtps);
    const uint32_t word = r.g.read_data(14);
    EXPECT_EQ(GteRig::sx(word), 85);
    EXPECT_EQ(GteRig::sy(word), 170);
    PreciseVertex v;
    ASSERT_TRUE(r.cache.find(word, v));
    EXPECT_NEAR(v.x, 85.333f, 0.01f);
    EXPECT_NEAR(v.y, 170.667f, 0.01f);
    EXPECT_FLOAT_EQ(v.w, static_cast<float>(r.g.read_data(19)));
    EXPECT_EQ(std::floor(v.x), GteRig::sx(word));
    EXPECT_EQ(std::floor(v.y), GteRig::sy(word));
}

TEST(PreciseGte, ScreenOffsetIsPartOfThePosition) {
    GteRig r;
    r.g.write_ctrl(24, (10u << 16) | 0x8000u);  // OFX = 10.5
    r.g.write_ctrl(25, static_cast<uint32_t>(-20) << 16);
    r.vec(0, 0, 0, 0x1000);
    r.g.execute(GteRig::kRtps);
    PreciseVertex v;
    ASSERT_TRUE(r.cache.find(r.g.read_data(14), v));
    EXPECT_FLOAT_EQ(v.x, 10.5f);
    EXPECT_FLOAT_EQ(v.y, -20.0f);
}

TEST(PreciseGte, RtptRecordsAllThreeVertices) {
    GteRig r;
    r.vec(0, 100, 50, 0x3000);
    r.vec(1, -200, 70, 0x2800);
    r.vec(2, 30, -90, 0x5000);
    r.g.execute(GteRig::kRtpt);
    for (unsigned reg : {12u, 13u, 14u}) {
        const uint32_t word = r.g.read_data(reg);
        PreciseVertex v;
        ASSERT_TRUE(r.cache.find(word, v)) << "SXY" << reg - 12;
        EXPECT_EQ(std::floor(v.x), GteRig::sx(word));
        EXPECT_EQ(std::floor(v.y), GteRig::sy(word));
        EXPECT_FLOAT_EQ(v.w, static_cast<float>(r.g.read_data(reg + 5)));  // SZ1..SZ3
    }
}

TEST(PreciseGte, TruncatedValuesMatchAcrossRandomProjections) {
    GteRig r;
    std::mt19937 rng(99);
    int recorded = 0;
    for (int i = 0; i < 2000; ++i) {
        r.g.write_ctrl(24, static_cast<uint32_t>((static_cast<int>(rng() % 600) - 300) * 65536 + static_cast<int>(rng() & 0xFFFF)));
        r.g.write_ctrl(25, static_cast<uint32_t>((static_cast<int>(rng() % 400) - 200) * 65536 + static_cast<int>(rng() & 0xFFFF)));
        r.g.write_ctrl(26, 0x200 + rng() % 0x2000);
        r.vec(0, static_cast<int>(rng() % 4000) - 2000, static_cast<int>(rng() % 4000) - 2000, 0x100 + rng() % 0x7000);
        r.g.execute(GteRig::kRtps);
        const uint32_t word = r.g.read_data(14);
        PreciseVertex v;
        if (!r.cache.find(word, v)) continue;  // saturated or divide overflow: stays integer
        ++recorded;
        EXPECT_EQ(std::floor(v.x), GteRig::sx(word));
        EXPECT_EQ(std::floor(v.y), GteRig::sy(word));
    }
    EXPECT_GT(recorded, 500);
}

TEST(PreciseGte, SaturatedAndOverflowedProjectionsAreNotRecorded) {
    GteRig r;
    r.vec(0, 0x7000, 0, 0x100);  // far outside the screen: SX saturates
    r.g.execute(GteRig::kRtps);
    PreciseVertex v;
    EXPECT_FALSE(r.cache.find(r.g.read_data(14), v));
    r.g.write_ctrl(26, 0x1000);
    r.vec(0, 10, 10, 0x10);  // H >= 2 * SZ3: divide overflow
    r.g.execute(GteRig::kRtps);
    EXPECT_FALSE(r.cache.find(r.g.read_data(14), v));
}

TEST(PreciseGte, NothingIsRecordedWithoutASink) {
    Gte g;
    g.write_ctrl(0, pk(0x1000, 0));
    g.write_ctrl(2, pk(0x1000, 0));
    g.write_ctrl(4, 0x1000);
    g.write_ctrl(26, 0x1000);
    g.write_data(0, pk(0x100, 0x200));
    g.write_data(1, 0x3000);
    g.execute(GteRig::kRtps);  // must not touch a null sink
    EXPECT_EQ(g.read_data(19), 0x3000u);
}

// ---------------------------------------------------------------- GPU front end

namespace {

struct Recorder : Rasterizer {
    std::vector<GpuVertex> tris;
    void triangle(std::span<uint16_t>, const DrawState&, const PrimitiveFlags&, const GpuVertex v[3]) override {
        tris.insert(tris.end(), v, v + 3);
    }
    void rect(std::span<uint16_t>, const DrawState&, const PrimitiveFlags&, const GpuVertex&, int, int) override {}
    void line(std::span<uint16_t>, const DrawState&, const PrimitiveFlags&, const GpuVertex&, const GpuVertex&) override {}
};

struct GpuRig : dw3test::Clock {
    IrqController irq{bus};
    VideoTiming video{sched, irq};
    Dma dma{bus, sched, irq};
    Gpu gpu{bus, irq, video, dma};
    PreciseVertexCache cache;
    Recorder rec;
    GpuRig() {
        gpu.set_rasterizer(&rec);
        gpu.set_precise_source(&cache);
        gpu.write_gp0(0xE3000000u);                       // area top-left (0, 0)
        gpu.write_gp0(0xE4000000u | 319u | (239u << 10));  // area bottom-right
        gpu.write_gp0(0xE5000000u | 3u | (5u << 11));      // draw offset (3, 5)
    }
    void flat_triangle(uint32_t a, uint32_t b, uint32_t c) {
        gpu.write_gp0(0x20FFFFFFu);
        gpu.write_gp0(a);
        gpu.write_gp0(b);
        gpu.write_gp0(c);
    }
};

}  // namespace

TEST(PreciseGpu, VerticesWithARecordedWordCarryTheirPositionPlusTheDrawOffset) {
    GpuRig r;
    r.cache.record(10, 20, 10.25, 20.75, 400.0);
    r.flat_triangle(pk(10, 20), pk(50, 20), pk(10, 60));
    ASSERT_EQ(r.rec.tris.size(), 3u);
    const GpuVertex& a = r.rec.tris[0];
    EXPECT_TRUE(a.precise);
    EXPECT_EQ(a.x, 13);
    EXPECT_EQ(a.y, 25);
    EXPECT_FLOAT_EQ(a.fx, 13.25f);
    EXPECT_FLOAT_EQ(a.fy, 25.75f);
    EXPECT_FLOAT_EQ(a.w, 400.0f);
    EXPECT_FALSE(r.rec.tris[1].precise);  // never recorded: integer vertex
    EXPECT_FALSE(r.rec.tris[2].precise);
}

TEST(PreciseGpu, AWordTheGameChangedFallsBackToTheInteger) {
    GpuRig r;
    r.cache.record(10, 20, 10.25, 20.75, 400.0);
    r.flat_triangle(pk(11, 20), pk(50, 20), pk(10, 61));
    for (const GpuVertex& v : r.rec.tris) EXPECT_FALSE(v.precise);
}

TEST(PreciseGpu, WithoutASourceEveryVertexIsInteger) {
    GpuRig r;
    r.gpu.set_precise_source(nullptr);
    r.cache.record(10, 20, 10.25, 20.75, 400.0);
    r.flat_triangle(pk(10, 20), pk(50, 20), pk(10, 60));
    for (const GpuVertex& v : r.rec.tris) EXPECT_FALSE(v.precise);
}
