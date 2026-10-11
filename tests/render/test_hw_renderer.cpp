// Tests for the shader blobs and, when a Vulkan device is available, a comparison of the GPU renderer against the
// software rasterizer on the same primitives.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "dw3/soft_rasterizer.h"
#include "dw3/vertex_builder.h"

#if DW3_HW_RENDERER
#include "dw3/hw_renderer.h"
#include "shaders/present.frag.h"
#include "shaders/present.vert.h"
#include "shaders/psx_draw.frag.h"
#include "shaders/psx_draw.vert.h"

namespace {

bool is_spirv(const uint8_t* data, std::size_t size) {
    if (size < 20 || size % 4 != 0) return false;
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    return magic == 0x07230203u;
}

}  // namespace

TEST(Shaders, CompiledToSpirv) {
    using namespace dw3::shaders;
    EXPECT_TRUE(is_spirv(psx_draw_vert_spv, psx_draw_vert_spv_size));
    EXPECT_TRUE(is_spirv(psx_draw_frag_spv, psx_draw_frag_spv_size));
    EXPECT_TRUE(is_spirv(present_vert_spv, present_vert_spv_size));
    EXPECT_TRUE(is_spirv(present_frag_spv, present_frag_spv_size));
}

namespace {

using namespace dw3;

constexpr int kPixels = kVramWidth * kVramHeight;

struct Scene {
    std::vector<uint16_t> vram;
    DrawState s;
    Scene() : vram(kPixels, 0) {
        std::mt19937 rng(1234);
        // Texture data in the right half of the upper page, CLUTs in the bottom rows.
        for (int y = 0; y < 256; ++y)
            for (int x = 512; x < 768; ++x) vram[y * kVramWidth + x] = static_cast<uint16_t>(rng());
        for (int y = 480; y < 484; ++y)
            for (int x = 0; x < 256; ++x) vram[y * kVramWidth + x] = static_cast<uint16_t>(rng() & 0xFFFF);
        // Some zero (transparent) CLUT entries.
        for (int x = 0; x < 256; x += 7) vram[480 * kVramWidth + x] = 0;
        s.area_x1 = 0;
        s.area_y1 = 0;
        s.area_x2 = 319;
        s.area_y2 = 239;
        s.tex_base_x = 512;
        s.tex_base_y = 0;
    }
};

GpuVertex gv(int x, int y, int r = 128, int g = 128, int b = 128, int u = 0, int v = 0) {
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

struct Diff {
    int count = 0, max_channel = 0;
};

Diff compare(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    Diff d;
    for (int i = 0; i < kPixels; ++i) {
        if (a[i] == b[i]) continue;
        ++d.count;
        for (int sh : {0, 5, 10})
            d.max_channel = std::max(d.max_channel, std::abs(((a[i] >> sh) & 31) - ((b[i] >> sh) & 31)));
        if ((a[i] ^ b[i]) & 0x8000) d.max_channel = std::max(d.max_channel, 32);
    }
    return d;
}

class HwCompare : public ::testing::Test {
protected:
    void SetUp() override {
        std::string error;
        hw = HwRenderer::create(nullptr, {}, error);
        if (!hw) GTEST_SKIP() << "no Vulkan device: " << error;
    }

    // Runs `draw` against both rasterizers starting from the same VRAM and returns the difference.
    template <typename F>
    Diff run(F draw) {
        Scene a, b;
        hw->vram_written(b.vram, {});  // start from a defined GPU state
        SoftRasterizer soft;
        draw(soft, a.vram, a.s);
        draw(*hw, b.vram, b.s);
        std::vector<uint16_t> got(kPixels);
        hw->read_vram(got);
        return compare(a.vram, got);
    }

    std::unique_ptr<HwRenderer> hw;
};

TEST_F(HwCompare, FlatRectanglesAreExact) {
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        PrimitiveFlags f;
        r.rect(v, s, f, gv(10, 10, 255, 0, 0), 40, 30);
        r.rect(v, s, f, gv(30, 20, 0, 255, 0), 1, 1);
        r.rect(v, s, f, gv(300, 220, 10, 20, 30), 50, 50);  // clipped by the drawing area
        r.rect(v, s, f, gv(-10, -10, 9, 99, 199), 20, 20);
    });
    EXPECT_EQ(d.count, 0);
}

TEST_F(HwCompare, TexturedSpritesAreExact) {
    for (TexDepth depth : {TexDepth::Bit4, TexDepth::Bit8, TexDepth::Bit15}) {
        Diff d = run([depth](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
            s.depth = depth;
            PrimitiveFlags f;
            f.textured = true;
            f.clut = static_cast<uint16_t>(480 << 6);
            r.rect(v, s, f, gv(20, 20, 128, 128, 128, 0, 0), 64, 64);
            f.raw_texture = true;
            r.rect(v, s, f, gv(100, 20, 128, 128, 128, 37, 11), 100, 40);
            f.raw_texture = false;
            s.rect_flip_x = true;
            s.rect_flip_y = true;
            r.rect(v, s, f, gv(20, 100, 200, 100, 50, 200, 200), 60, 60);
        });
        EXPECT_EQ(d.count, 0) << "depth " << static_cast<int>(depth);
    }
}

TEST_F(HwCompare, TextureWindow) {
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        s.depth = TexDepth::Bit15;
        s.win_mask_x = 3;
        s.win_mask_y = 2;
        s.win_off_x = 5;
        s.win_off_y = 1;
        PrimitiveFlags f;
        f.textured = true;
        f.raw_texture = true;
        r.rect(v, s, f, gv(20, 20, 128, 128, 128, 3, 9), 128, 90);
    });
    EXPECT_EQ(d.count, 0);
}

TEST_F(HwCompare, SemiTransparencyModesOverABackground) {
    for (int mode = 0; mode < 4; ++mode) {
        Diff d = run([mode](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
            PrimitiveFlags bg;
            r.rect(v, s, bg, gv(0, 0, 200, 120, 60), 200, 200);
            s.semi_mode = static_cast<uint8_t>(mode);
            PrimitiveFlags f;
            f.semi_transparent = true;
            r.rect(v, s, f, gv(50, 50, 180, 90, 250), 80, 80);
            // Overlapping semi-transparent sprites in sequence depend on each other.
            r.rect(v, s, f, gv(80, 80, 100, 200, 30), 80, 80);
            s.depth = TexDepth::Bit15;
            PrimitiveFlags t;
            t.textured = true;
            t.semi_transparent = true;
            r.rect(v, s, t, gv(60, 60, 128, 128, 128, 0, 0), 100, 100);
        });
        EXPECT_EQ(d.count, 0) << "mode " << mode;
    }
}

TEST_F(HwCompare, MaskBitSetAndCheck) {
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        PrimitiveFlags f;
        s.set_mask = true;
        r.rect(v, s, f, gv(20, 20, 255, 0, 0), 50, 50);
        s.set_mask = false;
        s.check_mask = true;
        r.rect(v, s, f, gv(40, 40, 0, 0, 255), 50, 50);  // must not overwrite the masked part
        s.check_mask = false;
        s.semi_mode = 1;
        f.semi_transparent = true;
        r.rect(v, s, f, gv(30, 30, 0, 255, 0), 60, 60);
    });
    EXPECT_EQ(d.count, 0);
}

TEST_F(HwCompare, LinesAreExact) {
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        PrimitiveFlags f;
        r.line(v, s, f, gv(5, 5, 255, 255, 255), gv(300, 200, 255, 255, 255));
        r.line(v, s, f, gv(300, 10, 255, 0, 0), gv(10, 230, 255, 0, 0));
        r.line(v, s, f, gv(50, 50), gv(50, 50));
        f.gouraud = true;
        s.dither = true;
        r.line(v, s, f, gv(10, 100, 255, 0, 0), gv(250, 120, 0, 0, 255));
    });
    EXPECT_EQ(d.count, 0);
}

TEST_F(HwCompare, TrianglesMatchWithinRasterizationTolerance) {
    // Edge coverage and attribute interpolation follow the GPU's rules, so a few edge pixels may differ.
    Scene probe;
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        PrimitiveFlags f;
        GpuVertex t0[3] = {gv(20, 20, 255, 0, 0), gv(200, 40, 255, 0, 0), gv(60, 200, 255, 0, 0)};
        r.triangle(v, s, f, t0);
        f.gouraud = true;
        s.dither = true;
        GpuVertex t1[3] = {gv(100, 10, 255, 0, 0), gv(300, 120, 0, 255, 0), gv(120, 230, 0, 0, 255)};
        r.triangle(v, s, f, t1);
        f.gouraud = false;
        s.dither = false;
        f.textured = true;
        s.depth = TexDepth::Bit15;
        GpuVertex t2[3] = {gv(150, 100, 128, 128, 128, 0, 0), gv(250, 100, 128, 128, 128, 100, 0),
                           gv(150, 200, 128, 128, 128, 0, 100)};
        r.triangle(v, s, f, t2);
    });
    // About 100 of the ~60000 drawn pixels differ, all by one colour step: interpolation rounding.
    EXPECT_LT(d.count, 200) << "differing pixels";
    EXPECT_LE(d.max_channel, 1);
}

TEST_F(HwCompare, TransfersStayCoherentWithDraws) {
    // Draw, read back through the hooks, modify on the CPU, draw again.
    Scene sc;
    SoftRasterizer soft;
    Scene ref;
    hw->vram_written(sc.vram, {});
    PrimitiveFlags f;
    auto both = [&](auto fn) {
        fn(soft, ref.vram, ref.s);
        fn(*hw, sc.vram, sc.s);
    };
    both([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) { r.rect(v, s, f, gv(10, 10, 255, 0, 0), 32, 32); });
    hw->vram_read_begin(sc.vram, {0, 0, 64, 64});
    EXPECT_EQ(sc.vram[10 * kVramWidth + 10], ref.vram[10 * kVramWidth + 10]);
    // CPU-side change (as an image load would do), then a draw that samples it.
    for (int x = 0; x < 16; ++x) sc.vram[100 * kVramWidth + x] = ref.vram[100 * kVramWidth + x] = static_cast<uint16_t>(0x7FFF - x);
    hw->vram_written(sc.vram, {0, 100, 16, 1});
    both([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        s.tex_base_x = 0;
        s.tex_base_y = 0;
        s.depth = TexDepth::Bit15;
        PrimitiveFlags t;
        t.textured = true;
        t.raw_texture = true;
        r.rect(v, s, t, gv(100, 100, 128, 128, 128, 0, 100), 16, 1);
    });
    std::vector<uint16_t> got(kPixels);
    hw->read_vram(got);
    EXPECT_EQ(compare(ref.vram, got).count, 0);
}

TEST_F(HwCompare, DrawnRegionsAreSampledAfterwards) {
    // A sprite whose texture was just drawn by the GPU must see the new pixels.
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        PrimitiveFlags f;
        r.rect(v, s, f, gv(0, 256, 255, 128, 0), 64, 64);
        s.tex_base_x = 0;
        s.tex_base_y = 256;
        s.depth = TexDepth::Bit15;
        PrimitiveFlags t;
        t.textured = true;
        t.raw_texture = true;
        r.rect(v, s, t, gv(100, 100, 128, 128, 128, 0, 0), 64, 64);
    });
    EXPECT_EQ(d.count, 0);
}

TEST_F(HwCompare, RandomPrimitives) {
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        std::mt19937 rng(99);
        auto rnd = [&](int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)); };
        for (int i = 0; i < 400; ++i) {
            s.depth = static_cast<TexDepth>(rnd(0, 2));
            s.semi_mode = static_cast<uint8_t>(rnd(0, 3));
            s.dither = rnd(0, 1);
            s.set_mask = rnd(0, 7) == 0;
            s.check_mask = rnd(0, 7) == 0;
            PrimitiveFlags f;
            f.textured = rnd(0, 1);
            f.raw_texture = rnd(0, 3) == 0;
            f.semi_transparent = rnd(0, 2) == 0;
            f.clut = static_cast<uint16_t>(((480 + rnd(0, 3)) << 6) | rnd(0, 8));
            const int x = rnd(0, 280), y = rnd(0, 200);
            GpuVertex o = gv(x, y, rnd(0, 255), rnd(0, 255), rnd(0, 255), rnd(0, 120), rnd(0, 120));
            if (i % 3 == 0) {
                r.rect(v, s, f, o, rnd(1, 40), rnd(1, 40));
            } else if (i % 3 == 1) {
                r.line(v, s, f, o, gv(rnd(0, 300), rnd(0, 230), rnd(0, 255), rnd(0, 255), rnd(0, 255)));
            } else {
                f.gouraud = rnd(0, 1);
                GpuVertex t[3] = {o, gv(x + rnd(5, 60), y + rnd(-10, 40), rnd(0, 255), rnd(0, 255), rnd(0, 255), rnd(0, 120), rnd(0, 120)),
                                  gv(x + rnd(-10, 40), y + rnd(5, 60), rnd(0, 255), rnd(0, 255), rnd(0, 255), rnd(0, 120), rnd(0, 120))};
                r.triangle(v, s, f, t);
            }
        }
    });
    // Triangle edge pixels and interpolation rounding account for the differences.
    EXPECT_LT(d.count, 400) << "differing pixels";
}

}  // namespace
#endif  // DW3_HW_RENDERER
