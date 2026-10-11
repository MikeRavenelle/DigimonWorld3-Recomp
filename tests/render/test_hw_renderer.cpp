// Tests for the shader blobs and, when a Vulkan device is available, a comparison of the GPU renderer against the
// software rasterizer on the same primitives.

#include <gtest/gtest.h>

#include <algorithm>
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

class HwCompare : public ::testing::TestWithParam<int> {
protected:
    void SetUp() override {
        std::string error;
        hw = HwRenderer::create(nullptr, {}, error, GetParam());
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

TEST_P(HwCompare, FlatRectanglesAreExact) {
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        PrimitiveFlags f;
        r.rect(v, s, f, gv(10, 10, 255, 0, 0), 40, 30);
        r.rect(v, s, f, gv(30, 20, 0, 255, 0), 1, 1);
        r.rect(v, s, f, gv(300, 220, 10, 20, 30), 50, 50);  // clipped by the drawing area
        r.rect(v, s, f, gv(-10, -10, 9, 99, 199), 20, 20);
    });
    EXPECT_EQ(d.count, 0);
}

TEST_P(HwCompare, TexturedSpritesAreExact) {
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

TEST_P(HwCompare, TextureWindow) {
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

TEST_P(HwCompare, SemiTransparencyModesOverABackground) {
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

TEST_P(HwCompare, MaskBitSetAndCheck) {
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

TEST_P(HwCompare, LinesAreExact) {
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

TEST_P(HwCompare, TrianglesMatchWithinRasterizationTolerance) {
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

TEST_P(HwCompare, TransfersStayCoherentWithDraws) {
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

TEST_P(HwCompare, DrawnRegionsAreSampledAfterwards) {
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

TEST_P(HwCompare, RandomPrimitives) {
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

// Software reference for the two VRAM operations the Gpu front end performs on its array (the same row-by-row
// loops as Gpu::copy_vram and Gpu::fill_rect).
void ref_copy(std::vector<uint16_t>& v, const VramRect& src, int dx, int dy, bool check_mask, bool set_mask) {
    std::vector<uint16_t> row(static_cast<std::size_t>(src.w));
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) row[x] = v[((src.y + y) & 511) * kVramWidth + ((src.x + x) & 1023)];
        for (int x = 0; x < src.w; ++x) {
            uint16_t& d = v[((dy + y) & 511) * kVramWidth + ((dx + x) & 1023)];
            if (check_mask && (d & 0x8000)) continue;
            d = static_cast<uint16_t>(row[x] | (set_mask ? 0x8000 : 0));
        }
    }
}

void ref_fill(std::vector<uint16_t>& v, const VramRect& r, uint16_t color) {
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) v[((r.y + y) & 511) * kVramWidth + ((r.x + x) & 1023)] = color;
}

class HwVramOps : public HwCompare {
protected:
    // VRAM full of noise, mask bits included, uploaded to the GPU and mirrored in `ref`.
    void SetUp() override {
        HwCompare::SetUp();
        if (!hw) return;
        std::mt19937 rng(7);
        ref.resize(kPixels);
        for (auto& w : ref) w = static_cast<uint16_t>(rng());
        hw->vram_written(ref, {});
    }
    void copy(const VramRect& src, int dx, int dy, bool check = false, bool set = false) {
        ASSERT_TRUE(hw->vram_copy(src, dx, dy, check, set));
        ref_copy(ref, src, dx, dy, check, set);
    }
    void fill(const VramRect& r, uint16_t c) {
        ASSERT_TRUE(hw->vram_fill(r, c));
        ref_fill(ref, r, c);
    }
    int mismatches() {
        std::vector<uint16_t> got(kPixels);
        hw->read_vram(got);
        return compare(ref, got).count;
    }
    std::vector<uint16_t> ref;
};

TEST_P(HwVramOps, CopyBetweenDisjointAreas) {
    copy({10, 20, 100, 60}, 400, 300);
    EXPECT_EQ(mismatches(), 0);
}

TEST_P(HwVramOps, CopyWrapsAtTheEdges) {
    copy({1000, 500, 60, 30}, 5, 5);       // source wraps on both axes
    copy({30, 40, 80, 50}, 990, 490);      // destination wraps on both axes
    copy({1010, 505, 50, 20}, 1000, 500);  // both wrap
    EXPECT_EQ(mismatches(), 0);
}

TEST_P(HwVramOps, OverlappingCopiesFollowRowOrder) {
    copy({100, 100, 200, 100}, 110, 100);  // same rows, shifted right
    copy({100, 100, 200, 100}, 90, 100);   // same rows, shifted left
    copy({100, 100, 200, 100}, 100, 101);  // one row down: later rows re-read earlier output
    copy({100, 150, 200, 100}, 150, 120);  // up and across
    copy({0, 500, 64, 30}, 10, 510);       // overlap across the bottom edge
    EXPECT_EQ(mismatches(), 0);
}

TEST_P(HwVramOps, CopyHonoursMaskBits) {
    copy({0, 0, 300, 200}, 300, 250, true, false);
    copy({0, 0, 300, 200}, 320, 260, false, true);
    copy({50, 50, 200, 100}, 250, 150, true, true);
    EXPECT_EQ(mismatches(), 0);
}

TEST_P(HwVramOps, CopyOfFreshlyDrawnAndFilledAreas) {
    // Sources that only exist on the GPU (drawn or filled, not yet read back) must be copied as they are.
    Scene sc;
    DrawState s = sc.s;
    PrimitiveFlags f;
    SoftRasterizer soft;
    hw->rect(ref, s, f, gv(20, 20, 250, 10, 10), 60, 40);
    soft.rect(ref, s, f, gv(20, 20, 250, 10, 10), 60, 40);
    fill({200, 100, 64, 32}, 0x1234);
    copy({0, 0, 300, 150}, 400, 200);
    copy({200, 100, 64, 32}, 0, 400, true, true);
    EXPECT_EQ(mismatches(), 0);
}

TEST_P(HwVramOps, FillsWrapAndIgnoreMask) {
    fill({0, 0, 64, 64}, 0x7C1F);
    fill({1000, 500, 100, 40}, 0x03E0);
    fill({512, 256, 1024, 512}, 0x001F);
    fill({16, 8, 16, 1}, 0x1111);
    EXPECT_EQ(mismatches(), 0);
}

TEST_P(HwVramOps, CpuReadsSeeGpuSideOperations) {
    std::vector<uint16_t> cpu = ref;
    copy({10, 10, 64, 64}, 500, 100);
    fill({600, 300, 32, 32}, 0x4321);
    hw->vram_read_begin(cpu, {500, 100, 64, 64});
    hw->vram_read_begin(cpu, {600, 300, 32, 32});
    for (int y = 100; y < 164; ++y)
        for (int x = 500; x < 564; ++x) ASSERT_EQ(cpu[y * kVramWidth + x], ref[y * kVramWidth + x]);
    for (int y = 300; y < 332; ++y)
        for (int x = 600; x < 632; ++x) ASSERT_EQ(cpu[y * kVramWidth + x], ref[y * kVramWidth + x]);
}

TEST_P(HwVramOps, ManyFramesOfWorkStayCorrect) {
    // Enough submissions to cycle through the frame slots several times, with reads in between.
    std::mt19937 rng(5);
    auto rnd = [&](int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)); };
    std::vector<uint16_t> cpu = ref;
    for (int i = 0; i < 60; ++i) {
        copy({rnd(0, 1023), rnd(0, 511), rnd(1, 200), rnd(1, 100)}, rnd(0, 1023), rnd(0, 511), rnd(0, 3) == 0, rnd(0, 3) == 0);
        if (i % 7 == 0) fill({rnd(0, 1023) & ~15, rnd(0, 511), rnd(1, 4) * 16, rnd(1, 50)}, static_cast<uint16_t>(rng() & 0x7FFF));
        if (i % 5 == 0) {
            hw->vram_read_begin(cpu, {100, 100, 300, 200});
            for (int y = 100; y < 300; ++y)
                for (int x = 100; x < 400; ++x) ASSERT_EQ(cpu[y * kVramWidth + x], ref[y * kVramWidth + x]);
        }
    }
    EXPECT_EQ(mismatches(), 0);
}

INSTANTIATE_TEST_SUITE_P(InternalRes, HwCompare, ::testing::Values(1, 2, 3, 4));
INSTANTIATE_TEST_SUITE_P(InternalRes, HwVramOps, ::testing::Values(1, 2, 3, 4));

// Tests that look at the scaled image itself (internal resolution above 1).
class HwScaled : public HwCompare {
protected:
    // The scaled image of a native region, as 16-bit words.
    std::vector<uint16_t> scaled(const VramRect& r) {
        std::vector<uint16_t> out;
        EXPECT_TRUE(hw->read_scaled(r, out));
        return out;
    }
    // Counts native pixels of `r` whose subpixels are not all equal to the pixel's top-left subpixel.
    int non_blocky(const std::vector<uint16_t>& img, const VramRect& r) {
        const int n = hw->internal_scale(), w = r.w * n;
        int bad = 0;
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const uint16_t tl = img[static_cast<std::size_t>(y * n) * w + x * n];
                bool same = true;
                for (int sy = 0; sy < n; ++sy)
                    for (int sx = 0; sx < n; ++sx) same &= img[static_cast<std::size_t>(y * n + sy) * w + x * n + sx] == tl;
                bad += same ? 0 : 1;
            }
        return bad;
    }
    int zeros(const std::vector<uint16_t>& img) { return static_cast<int>(std::count(img.begin(), img.end(), uint16_t{0})); }
};

TEST_P(HwScaled, UploadsAreReplicatedAndReadBackExactly) {
    std::mt19937 rng(11);
    std::vector<uint16_t> v(kPixels);
    for (auto& w : v) w = static_cast<uint16_t>(rng());
    hw->vram_written(v, {});
    const VramRect r{100, 50, 90, 40};
    const auto img = scaled(r);
    const int n = hw->internal_scale();
    ASSERT_EQ(img.size(), static_cast<std::size_t>(r.w * n) * r.h * n);
    int bad = 0;
    for (int y = 0; y < r.h * n; ++y)
        for (int x = 0; x < r.w * n; ++x) bad += img[static_cast<std::size_t>(y) * r.w * n + x] != (v[(r.y + y / n) * kVramWidth + r.x + x / n] & 0x7FFF | (v[(r.y + y / n) * kVramWidth + r.x + x / n] & 0x8000));
    EXPECT_EQ(bad, 0);
    std::vector<uint16_t> got(kPixels);
    hw->read_vram(got);
    EXPECT_EQ(compare(v, got).count, 0);
}

TEST_P(HwScaled, SpritesAndTilesLeaveNoGapsAndStayOnNativePixels) {
    // Opaque texture data, no zero words, so any untouched (zero) pixel in the covered area is a gap.
    std::vector<uint16_t> v(kPixels, 0);
    std::mt19937 rng(21);
    for (int y = 0; y < 256; ++y)
        for (int x = 512; x < 768; ++x) v[y * kVramWidth + x] = static_cast<uint16_t>((rng() & 0x7FFF) | 1);
    hw->vram_written(v, {});
    DrawState s;
    s.area_x2 = 319;
    s.area_y2 = 239;
    s.tex_base_x = 512;
    s.depth = TexDepth::Bit15;
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    Rasterizer& r = *hw;
    // 8x6 tiles of 16x16 sprites at the left, the same tiles as pairs of triangles to the right.
    for (int ty = 0; ty < 6; ++ty)
        for (int tx = 0; tx < 8; ++tx) {
            const int u = (tx * 16 + ty * 5) & 0xEF, w = (ty * 16 + tx * 3) & 0xEF;
            r.rect(v, s, f, gv(8 + tx * 16, 8 + ty * 16, 128, 128, 128, u, w), 16, 16);
            const int x = 160 + tx * 16, y = 8 + ty * 16;
            GpuVertex a[3] = {gv(x, y, 128, 128, 128, u, w), gv(x + 16, y, 128, 128, 128, u + 16, w),
                              gv(x, y + 16, 128, 128, 128, u, w + 16)};
            GpuVertex b[3] = {gv(x + 16, y, 128, 128, 128, u + 16, w), gv(x + 16, y + 16, 128, 128, 128, u + 16, w + 16),
                              gv(x, y + 16, 128, 128, 128, u, w + 16)};
            r.triangle(v, s, f, a);
            r.triangle(v, s, f, b);
        }
    const VramRect rects{8, 8, 128, 96}, quads{160, 8, 128, 96};
    for (const VramRect& area : {rects, quads}) {
        const auto img = scaled(area);
        EXPECT_EQ(zeros(img), 0) << "gap subpixels";
        EXPECT_EQ(non_blocky(img, area), 0) << "native pixels split across texels";
    }
    // The triangle tiles are identical to the rectangle tiles, subpixel by subpixel.
    const auto a = scaled(rects), b = scaled(quads);
    EXPECT_EQ(a, b);
    // Nothing leaks outside the tiled areas.
    const auto around = scaled({4, 4, 136, 4});
    EXPECT_EQ(zeros(around), static_cast<int>(around.size()));
}

TEST_P(HwScaled, FlatTrianglesCoverTheSameSubpixelsAsRectangles) {
    std::vector<uint16_t> v(kPixels, 0);
    hw->vram_written(v, {});
    DrawState s;
    s.area_x2 = 319;
    s.area_y2 = 239;
    PrimitiveFlags f;
    Rasterizer& r = *hw;
    // Left: one rectangle. Right: the same box as two triangles. Odd sizes and positions.
    r.rect(v, s, f, gv(11, 7, 255, 0, 0), 37, 23);
    GpuVertex a[3] = {gv(111, 7, 255, 0, 0), gv(148, 7, 255, 0, 0), gv(111, 30, 255, 0, 0)};
    GpuVertex b[3] = {gv(148, 7, 255, 0, 0), gv(148, 30, 255, 0, 0), gv(111, 30, 255, 0, 0)};
    r.triangle(v, s, f, a);
    r.triangle(v, s, f, b);
    const auto left = scaled({8, 4, 45, 30}), right = scaled({108, 4, 45, 30});
    EXPECT_EQ(left, right);
    EXPECT_EQ(zeros(left), static_cast<int>(left.size()) - 37 * 23 * GetParam() * GetParam());
}

TEST_P(HwScaled, DitherPatternScalesWithTheResolution) {
    std::vector<uint16_t> v(kPixels, 0);
    hw->vram_written(v, {});
    DrawState s;
    s.area_x2 = 319;
    s.area_y2 = 239;
    s.dither = true;
    PrimitiveFlags f;
    f.gouraud = true;
    GpuVertex t[3] = {gv(20, 20, 105, 105, 105), gv(120, 20, 105, 105, 105), gv(20, 120, 105, 105, 105)};
    hw->triangle(v, s, f, t);
    const VramRect interior{24, 24, 40, 40};
    const auto img = scaled(interior);
    EXPECT_EQ(non_blocky(img, interior), 0);
    // The native look: the downsampled image has the 4x4 pattern and uses more than one value.
    std::vector<uint16_t> got(kPixels);
    hw->read_vram(got);
    std::vector<uint16_t> uniq;
    for (int y = 24; y < 64; ++y)
        for (int x = 24; x < 64; ++x) uniq.push_back(got[y * kVramWidth + x]);
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    EXPECT_GT(uniq.size(), 1u);
}

TEST_P(HwScaled, SemiTransparencyAndMaskWorkPerSubpixel) {
    std::vector<uint16_t> v(kPixels, 0);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) v[(y + 10) * kVramWidth + x + 10] = static_cast<uint16_t>(((x / 8 + y / 8) & 1) ? 0xC210 : 0x4210);
    hw->vram_written(v, {});
    DrawState s;
    s.area_x2 = 319;
    s.area_y2 = 239;
    PrimitiveFlags f;
    f.semi_transparent = true;
    s.semi_mode = 1;
    s.check_mask = true;
    s.set_mask = true;
    hw->rect(v, s, f, gv(5, 5, 90, 60, 30), 80, 80);
    const VramRect area{5, 5, 80, 80};
    const auto img = scaled(area);
    EXPECT_EQ(non_blocky(img, area), 0);
    // Masked (bit 15) source pixels were left alone, the others blended and marked.
    std::vector<uint16_t> got(kPixels);
    hw->read_vram(got);
    int masked = 0, marked = 0;
    for (int y = 10; y < 74; ++y)
        for (int x = 10; x < 74; ++x) {
            const uint16_t w = got[y * kVramWidth + x];
            if (v[y * kVramWidth + x] & 0x8000) masked += w == v[y * kVramWidth + x];
            else marked += (w & 0x8000) != 0;
        }
    EXPECT_EQ(masked, 32 * 64);
    EXPECT_EQ(marked, 32 * 64);
}

INSTANTIATE_TEST_SUITE_P(InternalRes, HwScaled, ::testing::Values(2, 3, 4, 8));

}  // namespace
#endif  // DW3_HW_RENDERER
