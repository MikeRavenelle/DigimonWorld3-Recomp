#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>
#include <vector>

#include "dw3/dma.h"
#include "dw3/gpu.h"
#include "dw3/irq.h"
#include "dw3/soft_rasterizer.h"
#include "dw3/video_timing.h"
#include "hw_test_util.h"

using namespace dw3;

namespace {

uint16_t C(int r, int g, int b) { return static_cast<uint16_t>(r | (g << 5) | (b << 10)); }

GpuVertex V(int x, int y, int r = 0, int g = 0, int b = 0, int u = 0, int v = 0) {
    GpuVertex o;
    o.x = x;
    o.y = y;
    o.r = static_cast<uint8_t>(r);
    o.g = static_cast<uint8_t>(g);
    o.b = static_cast<uint8_t>(b);
    o.u = static_cast<uint8_t>(u);
    o.v = static_cast<uint8_t>(v);
    return o;
}

struct Base {
    std::vector<uint16_t> vram = std::vector<uint16_t>(kVramWidth * kVramHeight, 0);
    DrawState s;
    PrimitiveFlags f;
    SoftRasterizer rz;

    Base() {
        s.area_x2 = kVramWidth - 1;
        s.area_y2 = kVramHeight - 1;
    }
    void tri(const GpuVertex& a, const GpuVertex& b, const GpuVertex& c) {
        GpuVertex t[3] = {a, b, c};
        rz.triangle(vram, s, f, t);
    }
    void quad(const GpuVertex& a, const GpuVertex& b, const GpuVertex& c, const GpuVertex& d) {
        tri(a, b, c);
        tri(b, c, d);
    }
    void rect(int x, int y, int w, int h, const GpuVertex& proto = V(0, 0)) {
        GpuVertex v = proto;
        v.x = x;
        v.y = y;
        rz.rect(vram, s, f, v, w, h);
    }
    void line(const GpuVertex& a, const GpuVertex& b) { rz.line(vram, s, f, a, b); }
    uint16_t& at(int x, int y) { return vram[static_cast<size_t>(y * kVramWidth + x)]; }
    int count_all() const { return static_cast<int>(std::count_if(vram.begin(), vram.end(), [](uint16_t p) { return p != 0; })); }
    int count(int x0, int y0, int x1, int y1) {
        int n = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) n += at(x, y) != 0;
        return n;
    }
    uint16_t max_in(int x0, int y0, int x1, int y1) {
        uint16_t m = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) m = std::max(m, at(x, y));
        return m;
    }
    // Texture helpers.
    void page(int bx, int by, TexDepth d) {
        s.tex_base_x = bx;
        s.tex_base_y = by;
        s.depth = d;
    }
    void textured(bool raw) {
        f.textured = true;
        f.raw_texture = raw;
    }
    // 4-bit texture (index == u) at VRAM (0,0), 16 texels; CLUT (0,480): entry i = (i+1)*0x421.
    void setup_4bit() {
        for (int k = 0; k < 4; ++k)
            at(k, 0) = static_cast<uint16_t>((4 * k) | ((4 * k + 1) << 4) | ((4 * k + 2) << 8) | ((4 * k + 3) << 12));
        for (int i = 0; i < 16; ++i) at(i, 480) = static_cast<uint16_t>((i + 1) * 0x421);
        page(0, 0, TexDepth::Bit4);
        f.clut = static_cast<uint16_t>(480 << 6);
        textured(true);
    }
    // 8-bit texture (index == u) at (0,0), 256 texels; CLUT (0,500): entry i = i+1.
    void setup_8bit() {
        for (int k = 0; k < 128; ++k) at(k, 0) = static_cast<uint16_t>((2 * k) | ((2 * k + 1) << 8));
        for (int i = 0; i < 256; ++i) at(i, 500) = static_cast<uint16_t>(i + 1);
        page(0, 0, TexDepth::Bit8);
        f.clut = static_cast<uint16_t>(500 << 6);
        textured(true);
    }
    // 15-bit row texture at page (64,0) row 0: texel(u) = u+1; and column texture at page (64,256): texel(v) = v+1.
    void setup_15bit() {
        for (int u = 0; u < 256; ++u) {
            at(64 + u, 0) = static_cast<uint16_t>(u + 1);
            at(64 + u, 256 + u) = static_cast<uint16_t>(u + 1);
        }
        page(64, 0, TexDepth::Bit15);
        textured(true);
    }
};

struct SoftF : Base, ::testing::Test {};
template <class P>
struct SoftPT : Base, ::testing::TestWithParam<P> {};

// Additive blend setup for double-draw detection: each draw adds 8 (5-bit) to red.
void additive(Base& b) {
    b.f.semi_transparent = true;
    b.s.semi_mode = 1;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Triangle coverage

using SoftTriCount = SoftPT<int>;
TEST_P(SoftTriCount, RightTriangleHasTriangularNumberOfPixels) {
    int n = GetParam();
    tri(V(0, 0, 255), V(n, 0, 255), V(0, n, 255));
    EXPECT_EQ(count_all(), n * (n + 1) / 2);
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftTriCount, ::testing::Range(1, 17));

TEST_F(SoftF, TopLeftRuleRightTriangleEdges) {
    tri(V(0, 0, 255), V(4, 0, 255), V(0, 4, 255));
    EXPECT_NE(at(0, 0), 0);  // top-left vertex
    EXPECT_NE(at(3, 0), 0);  // last pixel of the top row
    EXPECT_EQ(at(4, 0), 0);  // right vertex excluded
    EXPECT_NE(at(0, 3), 0);
    EXPECT_EQ(at(0, 4), 0);  // bottom vertex excluded
    EXPECT_EQ(at(3, 1), 0);  // hypotenuse (right/bottom edge) excluded
    EXPECT_NE(at(2, 1), 0);
}

TEST_F(SoftF, IsoscelesTriangleHasKnownCoverage) {
    tri(V(0, 0, 255), V(8, 0, 255), V(4, 8, 255));
    EXPECT_EQ(count_all(), 36);
    EXPECT_EQ(count(0, 0, 9, 0), 8);
    EXPECT_EQ(count(0, 7, 9, 7), 1);
    EXPECT_EQ(count(0, 8, 9, 8), 0);
}

TEST_F(SoftF, SinglePixelTriangleVertexGrid) {
    tri(V(5, 5, 255), V(6, 5, 255), V(5, 6, 255));
    EXPECT_EQ(count_all(), 1);
    EXPECT_NE(at(5, 5), 0);
}

TEST_F(SoftF, TopEdgeIncludedBottomEdgeExcluded) {
    tri(V(0, 0, 255), V(10, 0, 255), V(10, 10, 255));
    EXPECT_NE(at(9, 0), 0);
    EXPECT_EQ(count(0, 10, 10, 10), 0);
    EXPECT_EQ(at(10, 0), 0);
}

TEST_F(SoftF, LeftEdgeIncludedRightEdgeExcluded) {
    tri(V(0, 0, 255), V(10, 10, 255), V(0, 10, 255));
    EXPECT_NE(at(0, 5), 0);
    EXPECT_EQ(at(5, 5), 0);
    EXPECT_NE(at(4, 5), 0);
}

TEST_F(SoftF, DegenerateCollinearDrawsNothing) {
    tri(V(0, 0, 255), V(5, 5, 255), V(10, 10, 255));
    EXPECT_EQ(count_all(), 0);
}
TEST_F(SoftF, DegenerateHorizontalDrawsNothing) {
    tri(V(0, 3, 255), V(7, 3, 255), V(20, 3, 255));
    EXPECT_EQ(count_all(), 0);
}
TEST_F(SoftF, DegenerateVerticalDrawsNothing) {
    tri(V(3, 0, 255), V(3, 7, 255), V(3, 20, 255));
    EXPECT_EQ(count_all(), 0);
}
TEST_F(SoftF, DegenerateIdenticalPointsDrawNothing) {
    tri(V(3, 3, 255), V(3, 3, 255), V(3, 3, 255));
    EXPECT_EQ(count_all(), 0);
}

using SoftWinding = SoftPT<std::array<int, 6>>;
TEST_P(SoftWinding, ReversedWindingProducesSameImage) {
    auto p = GetParam();
    tri(V(p[0], p[1], 255), V(p[2], p[3], 255), V(p[4], p[5], 255));
    std::vector<uint16_t> a = vram;
    std::fill(vram.begin(), vram.end(), 0);
    tri(V(p[0], p[1], 255), V(p[4], p[5], 255), V(p[2], p[3], 255));
    EXPECT_EQ(a, vram);
    EXPECT_GT(count_all(), 0);
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftWinding,
                         ::testing::Values(std::array<int, 6>{0, 0, 10, 3, 4, 12}, std::array<int, 6>{5, 5, 30, 8, 12, 40},
                                           std::array<int, 6>{50, 2, 3, 3, 20, 20}, std::array<int, 6>{7, 9, 8, 30, 33, 10},
                                           std::array<int, 6>{100, 100, 101, 140, 160, 120},
                                           std::array<int, 6>{0, 0, 1, 10, 2, 0}));

using SoftQuadSize = SoftPT<std::pair<int, int>>;
TEST_P(SoftQuadSize, AxisAlignedQuadFillsExactlyOnce) {
    auto [w, h] = GetParam();
    additive(*this);
    quad(V(10, 20, 64), V(10 + w, 20, 64), V(10, 20 + h, 64), V(10 + w, 20 + h, 64));
    EXPECT_EQ(count_all(), w * h);
    EXPECT_EQ(max_in(0, 0, 80, 80), C(8, 0, 0));  // 64>>3 once; double drawing would give 16
    EXPECT_EQ(at(10, 20), C(8, 0, 0));
    EXPECT_EQ(at(10 + w - 1, 20 + h - 1), C(8, 0, 0));
    EXPECT_EQ(at(10 + w, 20), 0);
    EXPECT_EQ(at(10, 20 + h), 0);
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftQuadSize,
                         ::testing::Values(std::pair{1, 1}, std::pair{2, 3}, std::pair{5, 5}, std::pair{16, 16},
                                           std::pair{17, 9}, std::pair{31, 2}, std::pair{3, 40}, std::pair{50, 50}));

using SoftSkewQuad = SoftPT<std::array<int, 8>>;
TEST_P(SoftSkewQuad, SharedDiagonalNeverDoubleDrawn) {
    auto p = GetParam();
    additive(*this);
    // v0 v1 v2 / v1 v2 v3 split like the GPU front end.
    GpuVertex a = V(p[0], p[1], 64), b = V(p[2], p[3], 64), c = V(p[4], p[5], 64), d = V(p[6], p[7], 64);
    tri(a, b, c);
    int n1 = count_all();
    std::vector<uint16_t> first = vram;
    tri(b, c, d);
    EXPECT_EQ(max_in(0, 0, 200, 200), C(8, 0, 0));
    // Union: the second triangle contributes only new pixels, so the union is the sum of both counts.
    std::fill(vram.begin(), vram.end(), 0);
    tri(b, c, d);
    int n2 = count_all();
    std::fill(vram.begin(), vram.end(), 0);
    f.semi_transparent = false;
    quad(a, b, c, d);
    EXPECT_EQ(count_all(), n1 + n2);
    EXPECT_GT(n1, 0);
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftSkewQuad,
                         ::testing::Values(std::array<int, 8>{10, 10, 60, 14, 12, 70, 66, 77},
                                           std::array<int, 8>{20, 5, 80, 30, 5, 40, 60, 90},
                                           std::array<int, 8>{0, 0, 33, 1, 1, 33, 40, 40},
                                           std::array<int, 8>{50, 50, 51, 90, 90, 51, 91, 91}));

TEST_F(SoftF, FanAroundCenterHasNoSeams) {
    additive(*this);
    GpuVertex c = V(20, 20, 64);
    tri(V(0, 0, 64), V(40, 0, 64), c);
    tri(V(40, 0, 64), V(40, 40, 64), c);
    tri(V(40, 40, 64), V(0, 40, 64), c);
    tri(V(0, 40, 64), V(0, 0, 64), c);
    EXPECT_EQ(count_all(), 1600);
    EXPECT_EQ(max_in(0, 0, 60, 60), C(8, 0, 0));
}

// ------------------------------------------------------------------------------------------------
// Clipping

TEST_F(SoftF, DrawAreaIsInclusive) {
    s.area_x1 = 10; s.area_y1 = 10; s.area_x2 = 20; s.area_y2 = 20;
    tri(V(0, 0, 255), V(100, 0, 255), V(0, 100, 255));
    EXPECT_EQ(count_all(), 121);
    EXPECT_NE(at(10, 10), 0);
    EXPECT_NE(at(20, 20), 0);
    EXPECT_EQ(at(21, 20), 0);
    EXPECT_EQ(at(9, 10), 0);
}
TEST_F(SoftF, DrawAreaSinglePixel) {
    s.area_x1 = 5; s.area_y1 = 5; s.area_x2 = 5; s.area_y2 = 5;
    tri(V(0, 0, 255), V(100, 0, 255), V(0, 100, 255));
    EXPECT_EQ(count_all(), 1);
}
TEST_F(SoftF, InvertedDrawAreaDrawsNothing) {
    s.area_x1 = 20; s.area_y1 = 20; s.area_x2 = 10; s.area_y2 = 10;
    tri(V(0, 0, 255), V(100, 0, 255), V(0, 100, 255));
    rect(0, 0, 50, 50, V(0, 0, 255));
    line(V(0, 0, 255), V(50, 50, 255));
    EXPECT_EQ(count_all(), 0);
}
TEST_F(SoftF, NegativeCoordinatesAreClipped) {
    tri(V(-50, -50, 255), V(10, -50, 255), V(-50, 10, 255));
    // Triangle with legs 60 spans x,y in [-50,9]; hypotenuse x+y<-40.
    int expected = 0;
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) expected += (x + y < -40 + 0) ? 1 : 0;
    EXPECT_EQ(count_all(), expected);
}
TEST_F(SoftF, NegativeVertexLargeTriangleCoversPositiveQuadrant) {
    s.area_x2 = 15; s.area_y2 = 15;
    tri(V(-100, -100, 255), V(300, -100, 255), V(-100, 300, 255));
    EXPECT_EQ(count_all(), 256);
}
TEST_F(SoftF, DrawAreaLimitedToVramBounds) {
    s.area_x2 = 5000; s.area_y2 = 5000;
    rect(1020, 508, 10, 10, V(0, 0, 255));
    EXPECT_EQ(count_all(), 4 * 4);
}
TEST_F(SoftF, TriangleClippedLeftOfAreaKeepsInteriorValues) {
    s.area_x1 = 8;
    f.gouraud = true;
    tri(V(0, 0, 0), V(16, 0, 240), V(0, 16, 0));
    EXPECT_EQ(at(8, 0), C(15, 0, 0));
    EXPECT_EQ(at(7, 0), 0);
}
TEST_F(SoftF, RectClipLeftTop) {
    s.area_x1 = 12; s.area_y1 = 12;
    rect(10, 10, 8, 8, V(0, 0, 255));
    EXPECT_EQ(count_all(), 6 * 6);
}
TEST_F(SoftF, RectClipRightBottom) {
    s.area_x2 = 12; s.area_y2 = 12;
    rect(10, 10, 8, 8, V(0, 0, 255));
    EXPECT_EQ(count_all(), 3 * 3);
}
TEST_F(SoftF, RectFullyOutsideAreaDrawsNothing) {
    s.area_x1 = 100; s.area_y1 = 100;
    rect(10, 10, 8, 8, V(0, 0, 255));
    EXPECT_EQ(count_all(), 0);
}
TEST_F(SoftF, LineClippedToArea) {
    s.area_x1 = 5; s.area_x2 = 9;
    line(V(0, 3, 255), V(20, 3, 255));
    EXPECT_EQ(count_all(), 5);
}

// ------------------------------------------------------------------------------------------------
// Flat and gouraud colours

using SoftFlat = SoftPT<std::pair<int, int>>;  // 8-bit value, expected 5-bit
TEST_P(SoftFlat, EightBitTo5BitConversion) {
    auto [in, out] = GetParam();
    rect(3, 3, 1, 1, V(0, 0, in, in, in));
    EXPECT_EQ(at(3, 3), in < 8 ? 0 : C(out, out, out));
    tri(V(10, 10, in, in, in), V(20, 10, in, in, in), V(10, 20, in, in, in));
    if (in >= 8) EXPECT_EQ(at(11, 11), C(out, out, out));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftFlat,
                         ::testing::Values(std::pair{255, 31}, std::pair{128, 16}, std::pair{8, 1}, std::pair{7, 0},
                                           std::pair{248, 31}, std::pair{247, 30}, std::pair{64, 8}, std::pair{100, 12}));

TEST_F(SoftF, FlatTriangleChannelsIndependent) {
    tri(V(0, 0, 248, 80, 16), V(20, 0, 248, 80, 16), V(0, 20, 248, 80, 16));
    EXPECT_EQ(at(2, 2), C(31, 10, 2));
}
TEST_F(SoftF, FlatShadingUsesFirstVertexColour) {
    tri(V(0, 0, 248, 0, 0), V(20, 0, 0, 248, 0), V(0, 20, 0, 0, 248));
    EXPECT_EQ(at(5, 5), C(31, 0, 0));
}
TEST_F(SoftF, GouraudVertexPixelsKeepVertexColour) {
    f.gouraud = true;
    tri(V(0, 0, 200, 100, 50), V(32, 0, 40, 240, 8), V(0, 32, 8, 16, 248));
    EXPECT_EQ(at(0, 0), C(25, 12, 6));
}
TEST_F(SoftF, GouraudRedAlongTopEdge) {
    f.gouraud = true;
    tri(V(0, 0, 0), V(16, 0, 240), V(0, 16, 0));
    const int expect[] = {0, 1, 3, 5, 7, 9, 11, 13, 15};  // (15*x)>>3
    for (int x = 0; x < 9; ++x) EXPECT_EQ(at(x, 0), C(expect[x] , 0, 0)) << x;
}
TEST_F(SoftF, GouraudRedConstantAlongColumn) {
    f.gouraud = true;
    tri(V(0, 0, 0), V(16, 0, 240), V(0, 16, 0));
    EXPECT_EQ(at(8, 4), C(15, 0, 0));
    EXPECT_EQ(at(8, 1), C(15, 0, 0));
}
TEST_F(SoftF, GouraudVerticalGradient) {
    f.gouraud = true;
    tri(V(0, 0, 0, 0, 0), V(16, 0, 0, 0, 0), V(0, 16, 0, 240, 0));
    for (int y = 0; y < 15; ++y) EXPECT_EQ(at(0, y), C(0, (15 * y) >> 3, 0)) << y;
}
TEST_F(SoftF, GouraudBlueChannel) {
    f.gouraud = true;
    tri(V(0, 0, 0, 0, 240), V(16, 0, 0, 0, 0), V(0, 16, 0, 0, 240));
    EXPECT_EQ(at(0, 0), C(0, 0, 30));
    EXPECT_EQ(at(8, 0), C(0, 0, 15));
}
TEST_F(SoftF, GouraudMonotonicOnLargeTriangle) {
    f.gouraud = true;
    tri(V(0, 0, 0), V(300, 0, 255), V(0, 200, 0));
    int prev = -1;
    for (int x = 0; x < 299; ++x) {
        int r = at(x, 0) & 31;
        EXPECT_GE(r, prev);
        prev = r;
    }
    EXPECT_EQ(prev, 31);
}
TEST_F(SoftF, GouraudNeverOverflowsAtExtremes) {
    f.gouraud = true;
    tri(V(0, 0, 255, 255, 255), V(63, 0, 255, 255, 255), V(0, 63, 0, 0, 0));
    EXPECT_EQ(at(0, 0), C(31, 31, 31));
    EXPECT_EQ(at(31, 0), C(31, 31, 31));
}

// ------------------------------------------------------------------------------------------------
// Texturing

using SoftTex4 = SoftPT<int>;
TEST_P(SoftTex4, FourBitClutLookup) {
    int u = GetParam();
    setup_4bit();
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, u, 0));
    EXPECT_EQ(at(100, 100), static_cast<uint16_t>((u + 1) * 0x421));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftTex4, ::testing::Range(0, 16));

using SoftTex8 = SoftPT<int>;
TEST_P(SoftTex8, EightBitClutLookup) {
    int u = GetParam();
    setup_8bit();
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, u, 0));
    EXPECT_EQ(at(100, 100), static_cast<uint16_t>(u + 1));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftTex8, ::testing::Values(0, 1, 2, 3, 5, 100, 127, 128, 254, 255));

using SoftTex15 = SoftPT<int>;
TEST_P(SoftTex15, DirectTexel) {
    int u = GetParam();
    setup_15bit();
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, u, 0));
    EXPECT_EQ(at(100, 100), static_cast<uint16_t>(u + 1));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftTex15, ::testing::Values(0, 1, 31, 32, 200, 255));

TEST_F(SoftF, ClutPositionDecoding) {
    // clut x = (clut & 0x3F)*16 = 48, y = 481.
    at(48, 481) = C(5, 6, 7);
    at(0, 0) = 0x0000;  // index 0 -> clut[0]
    page(0, 0, TexDepth::Bit4);
    f.clut = static_cast<uint16_t>(3 | (481 << 6));
    textured(true);
    rect(10, 10, 1, 1);
    EXPECT_EQ(at(10, 10), C(5, 6, 7));
}
TEST_F(SoftF, TexturePageOffsetsApply) {
    setup_15bit();
    page(128, 256, TexDepth::Bit15);
    at(128 + 3, 256 + 2) = C(1, 2, 3);
    rect(50, 50, 1, 1, V(0, 0, 0, 0, 0, 3, 2));
    EXPECT_EQ(at(50, 50), C(1, 2, 3));
}
TEST_F(SoftF, FourBitTextureReadsFromPageBase) {
    page(320, 0, TexDepth::Bit4);
    at(320, 7) = 0x0002;  // u=0 -> index 2
    at(5, 10) = C(9, 9, 9);
    f.clut = static_cast<uint16_t>(10 << 6);  // clut at (0,10)
    at(2, 10) = C(4, 5, 6);
    textured(true);
    rect(30, 30, 1, 1, V(0, 0, 0, 0, 0, 0, 7));
    EXPECT_EQ(at(30, 30), C(4, 5, 6));
}

struct WinCase { int mask, off, u, expect; };
using SoftWinU = SoftPT<WinCase>;
TEST_P(SoftWinU, HorizontalWindow) {
    auto c = GetParam();
    setup_15bit();
    s.win_mask_x = static_cast<uint8_t>(c.mask);
    s.win_off_x = static_cast<uint8_t>(c.off);
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, c.u, 0));
    EXPECT_EQ(at(100, 100), static_cast<uint16_t>(c.expect + 1));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftWinU,
                         ::testing::Values(WinCase{1, 0, 0x0F, 0x07}, WinCase{1, 1, 0x00, 0x08},
                                           WinCase{0x1F, 0, 0xAB, 0x03}, WinCase{0x1F, 0x1F, 0x03, 0xFB},
                                           WinCase{2, 0, 0x37, 0x27}, WinCase{3, 1, 0xFF, 0xEF},
                                           WinCase{0, 5, 0x42, 0x42}, WinCase{4, 7, 0x00, 0x20}));

using SoftWinV = SoftPT<WinCase>;
TEST_P(SoftWinV, VerticalWindow) {
    auto c = GetParam();
    setup_15bit();
    page(64, 256, TexDepth::Bit15);  // column texture: texel(v) = v+1 at x=64+v... use u = v
    s.win_mask_y = static_cast<uint8_t>(c.mask);
    s.win_off_y = static_cast<uint8_t>(c.off);
    // Texel at (64+u, 256+v) is only set for u==v, so sample with u = expected v'.
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, c.expect, c.u));
    EXPECT_EQ(at(100, 100), static_cast<uint16_t>(c.expect + 1));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftWinV,
                         ::testing::Values(WinCase{1, 0, 0x0F, 0x07}, WinCase{1, 1, 0x00, 0x08},
                                           WinCase{0x1F, 0x1F, 0x03, 0xFB}, WinCase{3, 1, 0xFF, 0xEF}));

TEST_F(SoftF, WindowAppliesToTriangles) {
    setup_15bit();
    s.win_mask_x = 1;  // clears bit 3 of u
    tri(V(0, 0, 0, 0, 0, 8, 0), V(16, 0, 0, 0, 0, 8, 0), V(0, 16, 0, 0, 0, 8, 0));
    EXPECT_EQ(at(3, 0), 1);  // u' = 0 -> texel 1
}

TEST_F(SoftF, TransparentTexelIsSkipped) {
    setup_15bit();
    at(64 + 5, 0) = 0;
    at(100, 100) = C(1, 1, 1);
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, 5, 0));
    EXPECT_EQ(at(100, 100), C(1, 1, 1));
}
TEST_F(SoftF, BlackWithBit15IsOpaque) {
    setup_15bit();
    at(64 + 5, 0) = 0x8000;
    at(100, 100) = C(1, 1, 1);
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, 5, 0));
    EXPECT_EQ(at(100, 100), 0x8000);
}
TEST_F(SoftF, ClutIndexZeroTransparentWhenEntryZero) {
    setup_4bit();
    at(0, 480) = 0;
    at(100, 100) = C(2, 2, 2);
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, 0, 0));
    EXPECT_EQ(at(100, 100), C(2, 2, 2));
}
TEST_F(SoftF, TransparentTexelsInTriangleLeaveHolesOnly) {
    setup_15bit();
    for (int u = 0; u < 256; ++u) at(64 + u, 0) = (u % 2) ? C(10, 10, 10) : 0;
    tri(V(0, 0, 0, 0, 0, 0, 0), V(32, 0, 0, 0, 0, 32, 0), V(0, 32, 0, 0, 0, 0, 0));
    EXPECT_EQ(at(1, 0), C(10, 10, 10));
    EXPECT_EQ(at(2, 0), 0);
}
TEST_F(SoftF, TextureDisableDrawsUntextured) {
    setup_15bit();
    s.texture_disable = true;
    rect(10, 10, 2, 2, V(0, 0, 248, 0, 0, 3, 0));
    EXPECT_EQ(at(10, 10), C(31, 0, 0));
}

using SoftMod = SoftPT<int>;
TEST_P(SoftMod, ModulationHalfIntensityTexel) {
    int col = GetParam();
    setup_15bit();
    f.raw_texture = false;
    at(64, 0) = C(16, 16, 16);  // 8-bit 128
    rect(100, 100, 1, 1, V(0, 0, col, col, col, 0, 0));
    int e = std::min(255, (128 * col) >> 7) >> 3;
    EXPECT_EQ(at(100, 100), C(e, e, e));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftMod, ::testing::Values(0, 0x40, 0x80, 0xC0, 0xFF, 0x20, 0xA0));

TEST_F(SoftF, ModulationNeutralIs0x80) {
    setup_15bit();
    f.raw_texture = false;
    at(64, 0) = C(31, 17, 3);
    rect(100, 100, 1, 1, V(0, 0, 0x80, 0x80, 0x80, 0, 0));
    EXPECT_EQ(at(100, 100), C(31, 17, 3));
}
TEST_F(SoftF, ModulationSaturates) {
    setup_15bit();
    f.raw_texture = false;
    at(64, 0) = C(31, 31, 31);
    rect(100, 100, 1, 1, V(0, 0, 0xFF, 0xFF, 0xFF, 0, 0));
    EXPECT_EQ(at(100, 100), C(31, 31, 31));
}
TEST_F(SoftF, ModulationPerChannel) {
    setup_15bit();
    f.raw_texture = false;
    at(64, 0) = C(16, 16, 16);
    rect(100, 100, 1, 1, V(0, 0, 0x00, 0x80, 0xFF, 0, 0));
    EXPECT_EQ(at(100, 100), C(0, 16, 31));
}
TEST_F(SoftF, RawTextureIgnoresVertexColour) {
    setup_15bit();
    at(64, 0) = C(16, 7, 3);
    rect(100, 100, 1, 1, V(0, 0, 0x10, 0x20, 0x30, 0, 0));
    EXPECT_EQ(at(100, 100), C(16, 7, 3));
}
TEST_F(SoftF, ModulationKeepsTexelBit15) {
    setup_15bit();
    f.raw_texture = false;
    at(64, 0) = static_cast<uint16_t>(0x8000 | C(16, 16, 16));
    rect(100, 100, 1, 1, V(0, 0, 0x80, 0x80, 0x80, 0, 0));
    EXPECT_EQ(at(100, 100), 0x8000 | C(16, 16, 16));
}
TEST_F(SoftF, TexelWithoutBit15KeepsBit15Clear) {
    setup_15bit();
    at(64, 0) = C(16, 16, 16);
    rect(100, 100, 1, 1, V(0, 0, 0, 0, 0, 0, 0));
    EXPECT_EQ(at(100, 100) & 0x8000, 0);
}
TEST_F(SoftF, TexturedGouraudTriangleModulatesPerPixel) {
    setup_15bit();
    f.raw_texture = false;
    f.gouraud = true;
    for (int u = 0; u < 256; ++u) at(64 + u, 0) = C(31, 31, 31);
    tri(V(0, 0, 0, 0, 0, 0, 0), V(16, 0, 240, 240, 240, 0, 0), V(0, 16, 0, 0, 0, 0, 0));
    EXPECT_EQ(at(0, 0), 0);  // modulated to black but texel was opaque: black pixel written as 0
    EXPECT_EQ(at(8, 0), C(29, 29, 29));  // (248*120)>>7 = 232
}
TEST_F(SoftF, TexturedTriangleUvInterpolatesAtOneToOne) {
    setup_15bit();
    tri(V(10, 10, 0, 0, 0, 0, 0), V(110, 10, 0, 0, 0, 100, 0), V(10, 20, 0, 0, 0, 0, 0));
    for (int x = 0; x < 100; x += 7) EXPECT_EQ(at(10 + x, 10), static_cast<uint16_t>(x + 1)) << x;
}
TEST_F(SoftF, TexturedTriangleScaledUv) {
    setup_15bit();
    // u runs 0..128 over 64 pixels: 2 texels per pixel.
    tri(V(0, 0, 0, 0, 0, 0, 0), V(64, 0, 0, 0, 0, 128, 0), V(0, 8, 0, 0, 0, 0, 0));
    for (int x = 0; x < 60; x += 5) EXPECT_EQ(at(x, 0), static_cast<uint16_t>(2 * x + 1)) << x;
}
TEST_F(SoftF, TexturedVerticalUv) {
    setup_15bit();
    page(64, 256, TexDepth::Bit15);
    // column texture: texel(u=v) = v+1, so sample along the diagonal with u == v.
    tri(V(0, 0, 0, 0, 0, 0, 0), V(40, 0, 0, 0, 0, 40, 0), V(0, 40, 0, 0, 0, 0, 40));
    EXPECT_EQ(at(0, 0), 1);
    EXPECT_EQ(at(5, 0), 0);  // texel (5,0) is not set in the column texture
}
TEST_F(SoftF, TexturedQuadSolidTextureNoSeamNoOverdraw) {
    setup_15bit();
    for (int u = 0; u < 256; ++u) for (int v = 0; v < 32; ++v) at(64 + u, v) = C(4, 4, 4);
    additive(*this);
    quad(V(10, 10, 0, 0, 0, 0, 0), V(26, 10, 0, 0, 0, 16, 0), V(10, 26, 0, 0, 0, 0, 16), V(26, 26, 0, 0, 0, 16, 16));
    // Texels have bit 15 clear so blending is skipped; check coverage by value written.
    int n = 0;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x)
            if (x >= 10 && x < 26 && y >= 10 && y < 26) {
                EXPECT_EQ(at(x, y), C(4, 4, 4)) << x << "," << y;
                ++n;
            } else {
                EXPECT_EQ(at(x, y), 0);
            }
    EXPECT_EQ(n, 256);
}

// ------------------------------------------------------------------------------------------------
// Semi-transparency

struct SemiCase { int mode; int r, g, b; };
// Background (20,10,4), foreground 8-bit (96,200,40) = 5-bit (12,25,5).
const SemiCase kSemi[] = {{0, 16, 17, 4}, {1, 31, 31, 9}, {2, 8, 0, 0}, {3, 23, 16, 5}};

using SoftSemiTri = SoftPT<SemiCase>;
TEST_P(SoftSemiTri, UntexturedTriangleBlends) {
    auto c = GetParam();
    f.semi_transparent = true;
    s.semi_mode = static_cast<uint8_t>(c.mode);
    at(5, 5) = C(20, 10, 4);
    tri(V(0, 0, 96, 200, 40), V(40, 0, 96, 200, 40), V(0, 40, 96, 200, 40));
    EXPECT_EQ(at(5, 5), C(c.r, c.g, c.b));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftSemiTri, ::testing::ValuesIn(kSemi));

using SoftSemiRect = SoftPT<SemiCase>;
TEST_P(SoftSemiRect, UntexturedRectBlends) {
    auto c = GetParam();
    f.semi_transparent = true;
    s.semi_mode = static_cast<uint8_t>(c.mode);
    at(5, 5) = C(20, 10, 4);
    rect(5, 5, 1, 1, V(0, 0, 96, 200, 40));
    EXPECT_EQ(at(5, 5), C(c.r, c.g, c.b));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftSemiRect, ::testing::ValuesIn(kSemi));

using SoftSemiLine = SoftPT<SemiCase>;
TEST_P(SoftSemiLine, LineBlends) {
    auto c = GetParam();
    f.semi_transparent = true;
    s.semi_mode = static_cast<uint8_t>(c.mode);
    at(5, 5) = C(20, 10, 4);
    at(6, 5) = C(20, 10, 4);
    line(V(5, 5, 96, 200, 40), V(6, 5, 96, 200, 40));
    EXPECT_EQ(at(5, 5), C(c.r, c.g, c.b));
    EXPECT_EQ(at(6, 5), C(c.r, c.g, c.b));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftSemiLine, ::testing::ValuesIn(kSemi));

using SoftSemiTex = SoftPT<SemiCase>;
TEST_P(SoftSemiTex, TexelBit15SetBlendsAndKeepsBit) {
    auto c = GetParam();
    setup_15bit();
    f.semi_transparent = true;
    s.semi_mode = static_cast<uint8_t>(c.mode);
    at(64, 0) = static_cast<uint16_t>(0x8000 | C(12, 25, 5));
    at(5, 5) = C(20, 10, 4);
    rect(5, 5, 1, 1, V(0, 0, 0, 0, 0, 0, 0));
    EXPECT_EQ(at(5, 5), 0x8000 | C(c.r, c.g, c.b));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftSemiTex, ::testing::ValuesIn(kSemi));

using SoftSemiTexOpaque = SoftPT<int>;
TEST_P(SoftSemiTexOpaque, TexelBit15ClearIsOpaque) {
    setup_15bit();
    f.semi_transparent = true;
    s.semi_mode = static_cast<uint8_t>(GetParam());
    at(64, 0) = C(12, 25, 5);
    at(5, 5) = C(20, 10, 4);
    rect(5, 5, 1, 1, V(0, 0, 0, 0, 0, 0, 0));
    EXPECT_EQ(at(5, 5), C(12, 25, 5));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftSemiTexOpaque, ::testing::Range(0, 4));

TEST_F(SoftF, SemiModeIgnoredWhenPrimitiveNotSemiTransparent) {
    s.semi_mode = 1;
    at(5, 5) = C(20, 10, 4);
    rect(5, 5, 1, 1, V(0, 0, 96, 200, 40));
    EXPECT_EQ(at(5, 5), C(12, 25, 5));
}
TEST_F(SoftF, SemiAddSaturatesAt31) {
    f.semi_transparent = true;
    s.semi_mode = 1;
    at(1, 1) = C(31, 31, 31);
    rect(1, 1, 1, 1, V(0, 0, 255, 255, 255));
    EXPECT_EQ(at(1, 1), C(31, 31, 31));
}
TEST_F(SoftF, SemiSubtractSaturatesAtZero) {
    f.semi_transparent = true;
    s.semi_mode = 2;
    at(1, 1) = C(3, 3, 3);
    rect(1, 1, 1, 1, V(0, 0, 255, 255, 255));
    EXPECT_EQ(at(1, 1), 0);
}
TEST_F(SoftF, SemiQuarterAddSaturates) {
    f.semi_transparent = true;
    s.semi_mode = 3;
    at(1, 1) = C(28, 28, 28);
    rect(1, 1, 1, 1, V(0, 0, 255, 255, 255));
    EXPECT_EQ(at(1, 1), C(31, 31, 31));
}
TEST_F(SoftF, SemiHalfOfWhiteAndBlack) {
    f.semi_transparent = true;
    s.semi_mode = 0;
    at(1, 1) = C(0, 0, 0);
    at(2, 1) = C(31, 31, 31);
    rect(1, 1, 1, 1, V(0, 0, 248, 248, 248));
    rect(2, 1, 1, 1, V(0, 0, 0, 0, 0));
    EXPECT_EQ(at(1, 1), C(15, 15, 15));
    EXPECT_EQ(at(2, 1), C(15, 15, 15));
}
TEST_F(SoftF, UntexturedSemiClearsBit15UnlessSetMask) {
    f.semi_transparent = true;
    s.semi_mode = 1;
    at(1, 1) = static_cast<uint16_t>(0x8000 | C(1, 1, 1));
    rect(1, 1, 1, 1, V(0, 0, 8, 8, 8));
    EXPECT_EQ(at(1, 1), C(2, 2, 2));
}
TEST_F(SoftF, GouraudSemiTriangleBlendsPerPixel) {
    f.gouraud = true;
    f.semi_transparent = true;
    s.semi_mode = 1;
    for (int x = 0; x < 16; ++x) at(x, 0) = C(5, 5, 5);
    tri(V(0, 0, 0, 0, 0), V(16, 0, 240, 0, 0), V(0, 16, 0, 0, 0));
    EXPECT_EQ(at(8, 0), C(20, 5, 5));
}

// ------------------------------------------------------------------------------------------------
// Mask bit

TEST_F(SoftF, CheckMaskSkipsMaskedTriangle) {
    s.check_mask = true;
    at(5, 5) = 0x8000;
    tri(V(0, 0, 248, 0, 0), V(40, 0, 248, 0, 0), V(0, 40, 248, 0, 0));
    EXPECT_EQ(at(5, 5), 0x8000);
    EXPECT_EQ(at(6, 5), C(31, 0, 0));
}
TEST_F(SoftF, CheckMaskSkipsMaskedRect) {
    s.check_mask = true;
    at(5, 5) = static_cast<uint16_t>(0x8000 | C(1, 2, 3));
    rect(5, 5, 2, 1, V(0, 0, 248, 0, 0));
    EXPECT_EQ(at(5, 5), 0x8000 | C(1, 2, 3));
    EXPECT_EQ(at(6, 5), C(31, 0, 0));
}
TEST_F(SoftF, CheckMaskSkipsMaskedLinePixel) {
    s.check_mask = true;
    at(2, 0) = 0x8001;
    line(V(0, 0, 248, 0, 0), V(4, 0, 248, 0, 0));
    EXPECT_EQ(at(2, 0), 0x8001);
    EXPECT_EQ(at(1, 0), C(31, 0, 0));
    EXPECT_EQ(at(3, 0), C(31, 0, 0));
}
TEST_F(SoftF, CheckMaskOffOverwritesMaskedPixel) {
    at(5, 5) = 0x8000;
    rect(5, 5, 1, 1, V(0, 0, 248, 0, 0));
    EXPECT_EQ(at(5, 5), C(31, 0, 0));
}
TEST_F(SoftF, SetMaskForcesBit15OnTriangle) {
    s.set_mask = true;
    tri(V(0, 0, 248, 0, 0), V(40, 0, 248, 0, 0), V(0, 40, 248, 0, 0));
    EXPECT_EQ(at(5, 5), 0x8000 | C(31, 0, 0));
}
TEST_F(SoftF, SetMaskForcesBit15OnRect) {
    s.set_mask = true;
    rect(5, 5, 1, 1, V(0, 0, 0, 0, 0));
    EXPECT_EQ(at(5, 5), 0x8000);
}
TEST_F(SoftF, SetMaskForcesBit15OnLine) {
    s.set_mask = true;
    line(V(0, 0, 8, 0, 0), V(3, 0, 8, 0, 0));
    for (int x = 0; x < 4; ++x) EXPECT_EQ(at(x, 0), 0x8000 | C(1, 0, 0));
}
TEST_F(SoftF, SetMaskOnTexturedOutput) {
    setup_15bit();
    s.set_mask = true;
    at(64, 0) = C(3, 3, 3);
    rect(5, 5, 1, 1);
    EXPECT_EQ(at(5, 5), 0x8000 | C(3, 3, 3));
}
TEST_F(SoftF, SetAndCheckMaskTogether) {
    s.set_mask = true;
    s.check_mask = true;
    at(1, 0) = 0x8000;
    rect(0, 0, 3, 1, V(0, 0, 248, 0, 0));
    EXPECT_EQ(at(0, 0), 0x8000 | C(31, 0, 0));
    EXPECT_EQ(at(1, 0), 0x8000);
    EXPECT_EQ(at(2, 0), 0x8000 | C(31, 0, 0));
}
TEST_F(SoftF, CheckMaskAppliesBeforeBlend) {
    s.check_mask = true;
    f.semi_transparent = true;
    s.semi_mode = 1;
    at(1, 1) = static_cast<uint16_t>(0x8000 | C(1, 1, 1));
    rect(1, 1, 1, 1, V(0, 0, 80, 80, 80));
    EXPECT_EQ(at(1, 1), 0x8000 | C(1, 1, 1));
}
TEST_F(SoftF, TexturedSkipsTransparentBeforeMaskCheck) {
    setup_15bit();
    s.set_mask = true;
    at(64, 0) = 0;
    rect(5, 5, 1, 1);
    EXPECT_EQ(at(5, 5), 0);
}

// ------------------------------------------------------------------------------------------------
// Dithering

constexpr int kDith[4][4] = {{12, 12, 12, 13}, {13, 12, 13, 12}, {12, 13, 12, 12}, {13, 12, 13, 12}};

TEST_F(SoftF, DitherPatternOnConstantGouraud) {
    f.gouraud = true;
    s.dither = true;
    tri(V(0, 0, 103, 103, 103), V(64, 0, 103, 103, 103), V(0, 64, 103, 103, 103));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) EXPECT_EQ(at(x, y), C(kDith[y][x], kDith[y][x], kDith[y][x])) << x << "," << y;
}
TEST_F(SoftF, DitherPatternRepeatsEveryFourPixels) {
    f.gouraud = true;
    s.dither = true;
    tri(V(0, 0, 103, 103, 103), V(64, 0, 103, 103, 103), V(0, 64, 103, 103, 103));
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) EXPECT_EQ(at(x + 8, y + 8), at(x & 3, y & 3)) << x << "," << y;
}
TEST_F(SoftF, DitherUsesAbsoluteCoordinates) {
    f.gouraud = true;
    s.dither = true;
    tri(V(1, 1, 103, 103, 103), V(65, 1, 103, 103, 103), V(1, 65, 103, 103, 103));
    EXPECT_EQ(at(3, 0 + 1), C(kDith[1][3], kDith[1][3], kDith[1][3]));
    EXPECT_EQ(at(4, 2), C(kDith[2][0], kDith[2][0], kDith[2][0]));
}
TEST_F(SoftF, DitherOffGivesFlatResult) {
    f.gouraud = true;
    tri(V(0, 0, 103, 103, 103), V(64, 0, 103, 103, 103), V(0, 64, 103, 103, 103));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) EXPECT_EQ(at(x, y), C(12, 12, 12));
}
TEST_F(SoftF, DitherNotAppliedToFlatTriangle) {
    s.dither = true;
    tri(V(0, 0, 103, 103, 103), V(64, 0, 103, 103, 103), V(0, 64, 103, 103, 103));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) EXPECT_EQ(at(x, y), C(12, 12, 12));
}
TEST_F(SoftF, DitherNeverAppliedToRects) {
    s.dither = true;
    f.gouraud = true;
    rect(0, 0, 8, 8, V(0, 0, 103, 103, 103));
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) EXPECT_EQ(at(x, y), C(12, 12, 12));
}
TEST_F(SoftF, DitherAppliedToModulatedTexture) {
    setup_15bit();
    f.raw_texture = false;
    s.dither = true;
    for (int u = 0; u < 256; ++u) at(64 + u, 0) = C(12, 12, 12);  // 8-bit 96
    // 96 * 0x80 >> 7 = 96; +1 -> 97 (12), +... need 8 boundary: 96*0x8A>>7 = 103.
    tri(V(0, 0, 0x8A, 0x8A, 0x8A, 0, 0), V(64, 0, 0x8A, 0x8A, 0x8A, 0, 0), V(0, 64, 0x8A, 0x8A, 0x8A, 0, 0));
    bool differs = false;
    for (int x = 0; x < 4; ++x) differs |= at(x, 0) != at(0, 0);
    EXPECT_TRUE(differs);
}
TEST_F(SoftF, DitherNotAppliedToRawTexture) {
    setup_15bit();
    s.dither = true;
    for (int u = 0; u < 256; ++u) at(64 + u, 0) = C(12, 12, 12);
    tri(V(0, 0, 0x8A, 0x8A, 0x8A, 0, 0), V(64, 0, 0x8A, 0x8A, 0x8A, 0, 0), V(0, 64, 0x8A, 0x8A, 0x8A, 0, 0));
    for (int x = 0; x < 8; ++x) EXPECT_EQ(at(x, 0), C(12, 12, 12));
}
TEST_F(SoftF, DitherAppliedToGouraudLines) {
    f.gouraud = true;
    s.dither = true;
    line(V(0, 0, 103, 103, 103), V(7, 0, 103, 103, 103));
    for (int x = 0; x < 4; ++x) EXPECT_EQ(at(x, 0), C(kDith[0][x], kDith[0][x], kDith[0][x]));
}
TEST_F(SoftF, DitherNotAppliedToFlatLines) {
    s.dither = true;
    line(V(0, 0, 103, 103, 103), V(7, 0, 103, 103, 103));
    for (int x = 0; x < 8; ++x) EXPECT_EQ(at(x, 0), C(12, 12, 12));
}
using SoftDitherClamp = SoftPT<std::pair<int, int>>;
TEST_P(SoftDitherClamp, ClampsAtRangeEnds) {
    auto [in, out] = GetParam();
    f.gouraud = true;
    s.dither = true;
    tri(V(0, 0, in, in, in), V(64, 0, in, in, in), V(0, 64, in, in, in));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) EXPECT_EQ(at(x, y), C(out, out, out));
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftDitherClamp,
                         ::testing::Values(std::pair{255, 31}, std::pair{252, 31}, std::pair{0, 0}, std::pair{4, 0}));

// ------------------------------------------------------------------------------------------------
// Rectangles and sprites

using SoftRectSize = SoftPT<std::pair<int, int>>;
TEST_P(SoftRectSize, CoversExactlyWidthByHeightAtTopLeft) {
    auto [w, h] = GetParam();
    rect(10, 20, w, h, V(0, 0, 255, 255, 255));
    EXPECT_EQ(count_all(), w * h);
    EXPECT_NE(at(10, 20), 0);
    EXPECT_NE(at(10 + w - 1, 20 + h - 1), 0);
    EXPECT_EQ(at(9, 20), 0);
    EXPECT_EQ(at(10, 19), 0);
    EXPECT_EQ(at(10 + w, 20), 0);
    EXPECT_EQ(at(10, 20 + h), 0);
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftRectSize,
                         ::testing::Values(std::pair{1, 1}, std::pair{8, 8}, std::pair{16, 16}, std::pair{3, 7},
                                           std::pair{64, 1}, std::pair{1, 64}, std::pair{100, 30}, std::pair{255, 3}));

TEST_F(SoftF, RectTexcoordsIncrementPerPixel) {
    setup_15bit();
    rect(10, 10, 8, 1, V(0, 0, 0, 0, 0, 20, 0));
    for (int i = 0; i < 8; ++i) EXPECT_EQ(at(10 + i, 10), static_cast<uint16_t>(21 + i));
}
TEST_F(SoftF, RectFlipXReversesDirection) {
    setup_15bit();
    s.rect_flip_x = true;
    rect(10, 10, 8, 1, V(0, 0, 0, 0, 0, 20, 0));
    for (int i = 0; i < 8; ++i) EXPECT_EQ(at(10 + i, 10), static_cast<uint16_t>(21 - i));
}
TEST_F(SoftF, RectFlipXWrapsBelowZero) {
    setup_15bit();
    s.rect_flip_x = true;
    rect(10, 10, 3, 1, V(0, 0, 0, 0, 0, 1, 0));
    EXPECT_EQ(at(10, 10), 2);
    EXPECT_EQ(at(11, 10), 1);
    EXPECT_EQ(at(12, 10), 256);
}
TEST_F(SoftF, RectTexcoordWrapsAt256) {
    setup_15bit();
    rect(10, 10, 3, 1, V(0, 0, 0, 0, 0, 254, 0));
    EXPECT_EQ(at(10, 10), 255);
    EXPECT_EQ(at(11, 10), 256);
    EXPECT_EQ(at(12, 10), 1);
}
TEST_F(SoftF, RectFlipYReversesDirection) {
    setup_15bit();
    page(64, 256, TexDepth::Bit15);
    // texel(u=v) = v+1: with u fixed at 5 only row 5 has a texel.
    s.rect_flip_y = true;
    rect(10, 10, 1, 3, V(0, 0, 0, 0, 0, 5, 6));
    EXPECT_EQ(at(10, 10), 0);   // v=6: no texel at (5,6)
    EXPECT_EQ(at(10, 11), 6);   // v=5
    EXPECT_EQ(at(10, 12), 0);   // v=4
}
TEST_F(SoftF, RectNoFlipYIncreases) {
    setup_15bit();
    page(64, 256, TexDepth::Bit15);
    rect(10, 10, 1, 3, V(0, 0, 0, 0, 0, 5, 4));
    EXPECT_EQ(at(10, 10), 0);
    EXPECT_EQ(at(10, 11), 6);
}
TEST_F(SoftF, RectFlipBothAxes) {
    setup_15bit();
    page(64, 256, TexDepth::Bit15);
    s.rect_flip_x = s.rect_flip_y = true;
    rect(10, 10, 3, 3, V(0, 0, 0, 0, 0, 2, 2));
    EXPECT_EQ(at(10, 10), 3);  // (2,2)
    EXPECT_EQ(at(11, 11), 2);  // (1,1)
    EXPECT_EQ(at(12, 12), 1);  // (0,0)
}
TEST_F(SoftF, TexturedRectFourBitMultiPixel) {
    setup_4bit();
    rect(10, 10, 16, 1, V(0, 0, 0, 0, 0, 0, 0));
    for (int i = 0; i < 16; ++i) EXPECT_EQ(at(10 + i, 10), static_cast<uint16_t>((i + 1) * 0x421));
}
TEST_F(SoftF, ModulatedTexturedRect) {
    setup_15bit();
    f.raw_texture = false;
    at(64, 0) = C(16, 16, 16);
    rect(20, 20, 1, 1, V(0, 0, 0x40, 0x40, 0x40, 0, 0));
    EXPECT_EQ(at(20, 20), C(8, 8, 8));
}
TEST_F(SoftF, SemiTransparentRect16x16) {
    f.semi_transparent = true;
    s.semi_mode = 1;
    rect(0, 0, 16, 16, V(0, 0, 64, 0, 0));
    rect(0, 0, 16, 16, V(0, 0, 64, 0, 0));
    EXPECT_EQ(at(15, 15), C(16, 0, 0));
    EXPECT_EQ(at(0, 0), C(16, 0, 0));
}

// ------------------------------------------------------------------------------------------------
// Lines

using SoftLineLen = SoftPT<int>;
TEST_P(SoftLineLen, HorizontalLineIncludesBothEndpoints) {
    int n = GetParam();
    line(V(10, 5, 255, 255, 255), V(10 + n, 5, 255, 255, 255));
    EXPECT_EQ(count_all(), n + 1);
    EXPECT_NE(at(10, 5), 0);
    EXPECT_NE(at(10 + n, 5), 0);
}
INSTANTIATE_TEST_SUITE_P(Soft, SoftLineLen, ::testing::Values(0, 1, 2, 7, 31, 100));

TEST_F(SoftF, VerticalLineIncludesBothEndpoints) {
    line(V(5, 10, 255, 255, 255), V(5, 25, 255, 255, 255));
    EXPECT_EQ(count_all(), 16);
    EXPECT_NE(at(5, 10), 0);
    EXPECT_NE(at(5, 25), 0);
}
TEST_F(SoftF, DiagonalLinePixels) {
    line(V(0, 0, 255, 255, 255), V(9, 9, 255, 255, 255));
    EXPECT_EQ(count_all(), 10);
    for (int i = 0; i < 10; ++i) EXPECT_NE(at(i, i), 0);
}
TEST_F(SoftF, AntiDiagonalLinePixels) {
    line(V(9, 0, 255, 255, 255), V(0, 9, 255, 255, 255));
    EXPECT_EQ(count_all(), 10);
    for (int i = 0; i < 10; ++i) EXPECT_NE(at(9 - i, i), 0);
}
TEST_F(SoftF, ShallowLineOnePixelPerColumn) {
    line(V(0, 0, 255, 255, 255), V(10, 5, 255, 255, 255));
    EXPECT_EQ(count_all(), 11);
    int last = 0;
    for (int x = 0; x <= 10; ++x) {
        int n = 0, yy = -1;
        for (int y = 0; y <= 5; ++y)
            if (at(x, y)) { ++n; yy = y; }
        EXPECT_EQ(n, 1) << x;
        EXPECT_GE(yy, last);
        last = yy;
    }
    EXPECT_NE(at(0, 0), 0);
    EXPECT_NE(at(10, 5), 0);
}
TEST_F(SoftF, SteepLineOnePixelPerRow) {
    line(V(0, 0, 255, 255, 255), V(5, 10, 255, 255, 255));
    EXPECT_EQ(count_all(), 11);
    for (int y = 0; y <= 10; ++y) EXPECT_EQ(count(0, y, 5, y), 1) << y;
}
TEST_F(SoftF, ReversedLineCoversSameBoundingEndpoints) {
    line(V(10, 10, 255, 255, 255), V(0, 0, 255, 255, 255));
    EXPECT_EQ(count_all(), 11);
    EXPECT_NE(at(0, 0), 0);
    EXPECT_NE(at(10, 10), 0);
}
TEST_F(SoftF, SinglePointLineDrawsOnePixel) {
    line(V(7, 7, 248, 0, 0), V(7, 7, 248, 0, 0));
    EXPECT_EQ(count_all(), 1);
    EXPECT_EQ(at(7, 7), C(31, 0, 0));
}
TEST_F(SoftF, FlatLineUsesFirstColour) {
    line(V(0, 0, 248, 0, 0), V(5, 0, 0, 248, 0));
    for (int x = 0; x <= 5; ++x) EXPECT_EQ(at(x, 0), C(31, 0, 0));
}
TEST_F(SoftF, GouraudLineHorizontalRamp) {
    f.gouraud = true;
    line(V(0, 0, 0, 0, 0), V(8, 0, 248, 0, 0));
    const int expect[] = {0, 3, 7, 11, 15, 19, 23, 27, 31};
    for (int x = 0; x <= 8; ++x) EXPECT_EQ(at(x, 0), C(expect[x], 0, 0)) << x;
}
TEST_F(SoftF, GouraudLineEndpointsExact) {
    f.gouraud = true;
    line(V(2, 3, 248, 8, 0), V(30, 20, 0, 248, 128));
    EXPECT_EQ(at(2, 3), C(31, 1, 0));
    EXPECT_EQ(at(30, 20), C(0, 31, 16));
}
TEST_F(SoftF, GouraudLineBlueRampVertical) {
    f.gouraud = true;
    line(V(4, 0, 0, 0, 0), V(4, 8, 0, 0, 248));
    for (int y = 0; y <= 8; ++y) EXPECT_EQ(at(4, y), C(0, 0, (31 * y) >> 3)) << y;
}
TEST_F(SoftF, LineIsNeverTextured) {
    setup_15bit();
    line(V(0, 0, 248, 0, 0), V(3, 0, 248, 0, 0));
    EXPECT_EQ(at(1, 0), C(31, 0, 0));
}
TEST_F(SoftF, LongLine) {
    line(V(0, 0, 255, 255, 255), V(1000, 400, 255, 255, 255));
    EXPECT_EQ(count_all(), 1001);
}

// ------------------------------------------------------------------------------------------------
// Quads

TEST_F(SoftF, GouraudQuadHasNoSeam) {
    f.gouraud = true;
    quad(V(0, 0, 255, 255, 255), V(32, 0, 255, 255, 255), V(0, 32, 255, 255, 255), V(32, 32, 255, 255, 255));
    EXPECT_EQ(count(0, 0, 40, 40), 1024);
    EXPECT_EQ(at(15, 16), C(31, 31, 31));
}
TEST_F(SoftF, QuadSharedDiagonalPixelsDrawnExactlyOnce) {
    additive(*this);
    quad(V(0, 0, 64), V(7, 0, 64), V(0, 7, 64), V(7, 7, 64));
    for (int i = 0; i < 7; ++i) EXPECT_EQ(at(6 - i, i), C(8, 0, 0)) << i;
}
TEST_F(SoftF, TexturedQuadUvGradient) {
    setup_15bit();
    quad(V(0, 0, 0, 0, 0, 0, 0), V(16, 0, 0, 0, 0, 16, 0), V(0, 16, 0, 0, 0, 0, 16), V(16, 16, 0, 0, 0, 16, 16));
    for (int x = 0; x < 16; ++x) EXPECT_EQ(at(x, 0), static_cast<uint16_t>(x + 1));
}
TEST_F(SoftF, TriangleNeverTouchesOutsideItsBounds) {
    tri(V(100, 100, 255), V(120, 105, 255), V(103, 130, 255));
    int outside = 0;
    for (int y = 0; y < kVramHeight; ++y)
        for (int x = 0; x < kVramWidth; ++x)
            if (at(x, y) && (x < 100 || x > 120 || y < 100 || y > 130)) ++outside;
    EXPECT_EQ(outside, 0);
    EXPECT_GT(count_all(), 100);
}
TEST_F(SoftF, FullFrameFillIsFast) {
    // 320x240 frame worth of overlapping gouraud triangles, repeated; a smoke test for throughput.
    f.gouraud = true;
    s.dither = true;
    for (int i = 0; i < 100; ++i) {
        quad(V(0, 0, 255, 0, 0), V(320, 0, 0, 255, 0), V(0, 240, 0, 0, 255), V(320, 240, 255, 255, 255));
    }
    EXPECT_EQ(count(0, 0, 319, 239) > 76000, true);
    EXPECT_EQ(at(320, 0), 0);
}

// ------------------------------------------------------------------------------------------------
// Integration through dw3::Gpu

namespace {
struct GpuRig : dw3test::Clock {
    IrqController irq{bus};
    VideoTiming video{sched, irq};
    Dma dma{bus, sched, irq};
    Gpu gpu{bus, irq, video, dma};
    SoftRasterizer rz;
    GpuRig() { gpu.set_rasterizer(&rz); }
    void w(uint32_t v) { gpu.write_gp0(v); }
    uint16_t px(int x, int y) { return gpu.vram()[static_cast<size_t>(y * kVramWidth + x)]; }
    static uint32_t xy(int x, int y) { return static_cast<uint32_t>((y << 16) | (x & 0xFFFF)); }
    void setup_area() {
        w(0xE3000000);
        w(0xE4000000 | (239u << 10) | 319u);
        w(0xE5000000);
    }
};
}  // namespace

TEST(SoftGpuIntegration, TexturedQuadFromUploadedTexture) {
    GpuRig r;
    r.setup_area();
    // Upload a 4x4 15-bit texture at VRAM (320,0).
    r.w(0xA0000000);
    r.w(GpuRig::xy(320, 0));
    r.w(GpuRig::xy(4, 4));
    uint16_t tex[16];
    for (int i = 0; i < 16; ++i) tex[i] = static_cast<uint16_t>(0x100 + i * 37);
    for (int i = 0; i < 16; i += 2) r.w(tex[i] | (static_cast<uint32_t>(tex[i + 1]) << 16));
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) EXPECT_EQ(r.px(320 + i, j), tex[j * 4 + i]);
    const uint32_t page = 5 | (2 << 7);  // base x 320, y 0, 15-bit
    r.w(0x2D808080);
    r.w(GpuRig::xy(10, 10));
    r.w(0x0000);
    r.w(GpuRig::xy(14, 10));
    r.w(4 | (page << 16));
    r.w(GpuRig::xy(10, 14));
    r.w(4 << 8);
    r.w(GpuRig::xy(14, 14));
    r.w(4 | (4 << 8));
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) EXPECT_EQ(r.px(10 + i, 10 + j), tex[j * 4 + i]) << i << "," << j;
    EXPECT_EQ(r.px(14, 10), 0);
    EXPECT_EQ(r.px(10, 14), 0);
    EXPECT_EQ(r.px(9, 9), 0);
}

TEST(SoftGpuIntegration, FlatTriangleHonoursOffsetAndDrawArea) {
    GpuRig r;
    r.w(0xE3000000 | (10u << 10) | 10u);  // area (10,10)-(20,20)
    r.w(0xE4000000 | (20u << 10) | 20u);
    r.w(0xE5000000 | (5u << 11) | 5u);  // offset (5,5)
    r.w(0x20F80000 | 0);                // flat triangle, colour 0x00_00_F8 -> blue? bytes: r=0,g=0,b=F8
    r.w(GpuRig::xy(0, 0));
    r.w(GpuRig::xy(100, 0));
    r.w(GpuRig::xy(0, 100));
    EXPECT_EQ(r.px(10, 10), C(0, 0, 31));
    EXPECT_EQ(r.px(20, 20), C(0, 0, 31));
    EXPECT_EQ(r.px(21, 20), 0);
    EXPECT_EQ(r.px(9, 10), 0);
    EXPECT_EQ(r.gpu.primitives(), 1u);
}

TEST(SoftGpuIntegration, FourBitSpriteWithClut) {
    GpuRig r;
    r.setup_area();
    // CLUT at (0,480): 16 entries, entry i = (i+1)*0x421.
    r.w(0xA0000000);
    r.w(GpuRig::xy(0, 480));
    r.w(GpuRig::xy(16, 1));
    for (int i = 0; i < 16; i += 2)
        r.w(static_cast<uint32_t>((i + 1) * 0x421) | (static_cast<uint32_t>((i + 2) * 0x421) << 16));
    // 4-bit texture at (0,0): indices 0..7 in the first two halfwords.
    r.w(0xA0000000);
    r.w(GpuRig::xy(0, 0));
    r.w(GpuRig::xy(2, 1));
    r.w(0x7654 << 16 | 0x3210);
    r.w(0xE1000000);  // texpage 0, 4-bit
    r.w(0x65808080);  // raw variable-size textured rect
    r.w(GpuRig::xy(50, 60));
    r.w(0 | ((480u << 6) << 16));
    r.w(GpuRig::xy(8, 1));
    for (int i = 0; i < 8; ++i) EXPECT_EQ(r.px(50 + i, 60), static_cast<uint16_t>((i + 1) * 0x421)) << i;
    EXPECT_EQ(r.px(58, 60), 0);
}

TEST(SoftGpuIntegration, GouraudQuadAndSemiTransparencyThroughGp0) {
    GpuRig r;
    r.setup_area();
    r.w(0xA0000000);  // background pixel (5,5)
    r.w(GpuRig::xy(5, 5));
    r.w(GpuRig::xy(2, 1));
    r.w(C(20, 10, 4) | (static_cast<uint32_t>(C(20, 10, 4)) << 16));
    r.w(0x2A000000 | 0x00C86000);  // flat semi-transparent quad, colour r=0x00,g=0xC8,b=0x60?
    // Colour bytes: word = 0x00BBGGRR -> 0x00C86000 -> r=0x00, g=0x60, b=0xC8.
    r.w(GpuRig::xy(0, 0));
    r.w(GpuRig::xy(20, 0));
    r.w(GpuRig::xy(0, 20));
    r.w(GpuRig::xy(20, 20));
    // Mode 0 (default texpage): (B+F)/2 -> r=(20+0)/2, g=(10+12)/2, b=(4+25)/2.
    EXPECT_EQ(r.px(5, 5), C(10, 11, 14));
    EXPECT_NE(r.px(19, 19), 0);
}
