#include <gtest/gtest.h>

#include "dw3/vertex_builder.h"

using namespace dw3;

namespace {

DrawState area(int x1, int y1, int x2, int y2) {
    DrawState s;
    s.area_x1 = x1;
    s.area_y1 = y1;
    s.area_x2 = x2;
    s.area_y2 = y2;
    return s;
}

GpuVertex vert(int x, int y, int u = 0, int v = 0, uint8_t c = 0x80) {
    GpuVertex g;
    g.x = x;
    g.y = y;
    g.u = static_cast<uint8_t>(u);
    g.v = static_cast<uint8_t>(v);
    g.r = g.g = g.b = c;
    return g;
}

}  // namespace

TEST(VertexBuilder, StatePackingFollowsDrawState) {
    DrawState s = area(0, 0, 319, 239);
    s.semi_mode = 2;
    s.depth = TexDepth::Bit8;
    s.tex_base_x = 128;
    s.tex_base_y = 256;
    s.check_mask = true;
    s.win_mask_x = 3;
    s.win_off_y = 7;
    PrimitiveFlags f;
    f.textured = true;
    f.semi_transparent = true;
    f.clut = (480 << 6) | 5;
    const HwState st = pack_state(s, f, true);
    using namespace state_bits;
    EXPECT_EQ(st.s0 & (kTextured | kSemi | kDither | kCheckMask), kTextured | kSemi | kDither | kCheckMask);
    EXPECT_EQ(st.s0 & kRaw, 0u);
    EXPECT_EQ((st.s0 >> 6) & 3, 2u);
    EXPECT_EQ((st.s0 >> 8) & 3, 1u);
    EXPECT_EQ(st.s1 & 0xFFFF, f.clut);
    EXPECT_EQ((st.s1 >> 16) & 15, 2u);
    EXPECT_EQ((st.s1 >> 20) & 1, 1u);
    EXPECT_EQ(st.s2 & 31, 3u);
    EXPECT_EQ((st.s2 >> 15) & 31, 7u);
}

TEST(VertexBuilder, TextureDisableTurnsTexturingOff) {
    DrawState s = area(0, 0, 10, 10);
    s.texture_disable = true;
    PrimitiveFlags f;
    f.textured = true;
    EXPECT_FALSE(uses_texture(s, f));
    EXPECT_EQ(pack_state(s, f, false).s0 & state_bits::kTextured, 0u);
}

TEST(VertexBuilder, DitherRulesMatchTheSoftwareRasterizer) {
    DrawState s = area(0, 0, 10, 10);
    s.dither = true;
    PrimitiveFlags flat;
    EXPECT_FALSE(triangle_dithers(s, flat));
    PrimitiveFlags shaded;
    shaded.gouraud = true;
    EXPECT_TRUE(triangle_dithers(s, shaded));
    PrimitiveFlags tex;
    tex.textured = true;
    EXPECT_TRUE(triangle_dithers(s, tex));
    tex.raw_texture = true;
    EXPECT_FALSE(triangle_dithers(s, tex));
    EXPECT_TRUE(line_dithers(s, shaded));
    EXPECT_FALSE(line_dithers(s, flat));
}

TEST(VertexBuilder, ClipRegionIsHalfOpenAndLimitedToVram) {
    EXPECT_EQ(clip_region(area(10, 20, 29, 39)), (Region{10, 20, 30, 40}));
    EXPECT_EQ(clip_region(area(-5, 0, 5000, 5000)), (Region{0, 0, 1024, 512}));
    EXPECT_TRUE(clip_region(area(30, 0, 10, 10)).empty());
}

TEST(VertexBuilder, TriangleVerticesSitOnPixelCentres) {
    std::vector<HwVertex> out;
    GpuVertex v[3] = {vert(10, 20), vert(30, 20), vert(10, 40)};
    build_triangle(out, area(0, 0, 100, 100), {}, v);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_FLOAT_EQ(out[1].x, 30.5f);
    EXPECT_FLOAT_EQ(out[2].y, 40.5f);
}

TEST(VertexBuilder, FlatTrianglesUseTheFirstVertexColour) {
    std::vector<HwVertex> out;
    GpuVertex v[3] = {vert(0, 0, 0, 0, 10), vert(5, 0, 0, 0, 20), vert(0, 5, 0, 0, 30)};
    build_triangle(out, area(0, 0, 100, 100), {}, v);
    EXPECT_EQ(out[1].r, 10);
    EXPECT_EQ(out[2].b, 10);
    PrimitiveFlags g;
    g.gouraud = true;
    out.clear();
    build_triangle(out, area(0, 0, 100, 100), g, v);
    EXPECT_EQ(out[1].r, 20);
    EXPECT_EQ(out[2].b, 30);
}

TEST(VertexBuilder, RectTexelCoordinatesHitTheStartCoordinateAtTheFirstPixelCentre) {
    std::vector<HwVertex> out;
    PrimitiveFlags f;
    f.textured = true;
    build_rect(out, area(0, 0, 100, 100), f, vert(4, 6, 10, 20), 8, 4);
    ASSERT_EQ(out.size(), 6u);
    // Left edge value u0 - 0.5, right edge u0 - 0.5 + width: the centre of pixel 0 samples exactly u0.
    EXPECT_FLOAT_EQ(out[0].u, 9.5f);
    EXPECT_FLOAT_EQ(out[1].u, 17.5f);
    EXPECT_FLOAT_EQ(out[0].v, 19.5f);
    EXPECT_FLOAT_EQ(out[2].v, 23.5f);
    EXPECT_FLOAT_EQ(out[1].x, 12.0f);
    EXPECT_FLOAT_EQ(out[2].y, 10.0f);
}

TEST(VertexBuilder, FlippedRectRunsTexelCoordinatesBackwards) {
    std::vector<HwVertex> out;
    DrawState s = area(0, 0, 100, 100);
    s.rect_flip_x = true;
    PrimitiveFlags f;
    f.textured = true;
    build_rect(out, s, f, vert(0, 0, 10, 0), 4, 1);
    EXPECT_FLOAT_EQ(out[0].u, 10.5f);
    EXPECT_FLOAT_EQ(out[1].u, 6.5f);
}

TEST(VertexBuilder, LineEmitsOneQuadPerPixelInsideTheClip) {
    std::vector<HwVertex> out;
    const int n = build_line(out, area(0, 0, 100, 100), {}, vert(2, 3), vert(12, 3));
    EXPECT_EQ(n, 11);
    EXPECT_EQ(out.size(), 66u);
    out.clear();
    EXPECT_EQ(build_line(out, area(5, 0, 9, 100), {}, vert(2, 3), vert(12, 3)), 5);  // x = 5..9
}

TEST(VertexBuilder, GouraudLineInterpolatesColour) {
    std::vector<HwVertex> out;
    PrimitiveFlags f;
    f.gouraud = true;
    GpuVertex a = vert(0, 0, 0, 0, 0), b = vert(10, 0, 0, 0, 200);
    build_line(out, area(0, 0, 100, 100), f, a, b);
    EXPECT_EQ(out.front().r, 0);
    EXPECT_EQ(out[out.size() - 1].r, 200);
    EXPECT_EQ(out[5 * 6].r, 100);
}

TEST(VertexBuilder, BoundsAreClippedToTheDrawingArea) {
    GpuVertex v[3] = {vert(-20, -20), vert(60, 10), vert(10, 90)};
    EXPECT_EQ(triangle_bounds(area(0, 0, 49, 49), v), (Region{0, 0, 50, 50}));
    EXPECT_TRUE(rect_bounds(area(0, 0, 9, 9), vert(20, 20), 4, 4).empty());
    EXPECT_EQ(line_bounds(area(0, 0, 99, 99), vert(5, 9), vert(2, 3)), (Region{2, 3, 6, 10}));
}

TEST(VertexBuilder, TextureRegionDependsOnDepth) {
    DrawState s = area(0, 0, 10, 10);
    s.tex_base_x = 320;
    s.tex_base_y = 256;
    s.depth = TexDepth::Bit4;
    EXPECT_EQ(texture_region(s, 0, 63, 0, 31), (Region{320, 256, 336, 288}));
    s.depth = TexDepth::Bit8;
    EXPECT_EQ(texture_region(s, 0, 63, 0, 31), (Region{320, 256, 352, 288}));
    s.depth = TexDepth::Bit15;
    EXPECT_EQ(texture_region(s, 0, 63, 0, 31), (Region{320, 256, 384, 288}));
}

TEST(VertexBuilder, TextureWindowWidensToTheWholePage) {
    DrawState s = area(0, 0, 10, 10);
    s.depth = TexDepth::Bit15;
    s.win_mask_x = 1;
    EXPECT_EQ(texture_region(s, 4, 5, 4, 5), (Region{0, 0, 256, 256}));
}

TEST(VertexBuilder, ClutRegion) {
    DrawState s = area(0, 0, 10, 10);
    PrimitiveFlags f;
    f.clut = static_cast<uint16_t>((497 << 6) | 3);
    s.depth = TexDepth::Bit4;
    EXPECT_EQ(clut_region(s, f), (Region{48, 497, 64, 498}));
    s.depth = TexDepth::Bit8;
    EXPECT_EQ(clut_region(s, f), (Region{48, 497, 304, 498}));
    s.depth = TexDepth::Bit15;
    EXPECT_TRUE(clut_region(s, f).empty());
}

TEST(VertexBuilder, RectTextureRegionWrapsToWholePage) {
    DrawState s = area(0, 0, 10, 10);
    s.depth = TexDepth::Bit15;
    EXPECT_EQ(rect_texture_region(s, vert(0, 0, 10, 10), 8, 8), (Region{10, 10, 18, 18}));
    EXPECT_EQ(rect_texture_region(s, vert(0, 0, 250, 10), 16, 8), (Region{0, 10, 256, 18}));
}
