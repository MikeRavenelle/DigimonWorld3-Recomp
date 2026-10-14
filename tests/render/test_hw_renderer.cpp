// Tests for the shader blobs and, when a GPU device is available, a comparison of the GPU renderer against the
// software rasterizer on the same primitives. The comparison runs on every graphics API the build contains (Vulkan,
// Direct3D 12 on Windows and Metal on macOS); all of them must produce the software rasterizer's pixels.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include "dw3/soft_rasterizer.h"
#include "dw3/texrep/blocks.h"
#include "dw3/texrep/pack.h"
#include "dw3/hdr.h"
#include "dw3/vertex_builder.h"
#include "dw3/widescreen.h"

#if DW3_HW_RENDERER
#include "dw3/hw_renderer.h"
#include "shaders/osd.frag.h"
#include "shaders/osd.vert.h"
#include "shaders/present.frag.h"
#include "shaders/present.vert.h"
#include "shaders/psx_draw.frag.h"
#include "shaders/psx_draw_layer.frag.h"
#include "shaders/psx_draw.vert.h"
#include "shaders/post.vert.h"
#include "shaders/post_scale.frag.h"
#include "shaders/post_cas.frag.h"
#include "shaders/post_grade.frag.h"
#include "shaders/post_crt.frag.h"
#include "shaders/post_easu.frag.h"
#include "shaders/post_rcas.frag.h"
#include "shaders/post_smaa_edges.frag.h"
#include "shaders/post_smaa_weights.frag.h"
#include "shaders/post_smaa_blend.frag.h"
#ifdef DW3_HAVE_METAL
#include "shaders/post.vert.msl.h"
#include "shaders/post_scale.frag.msl.h"
#include "shaders/post_cas.frag.msl.h"
#include "shaders/post_grade.frag.msl.h"
#include "shaders/post_crt.frag.msl.h"
#include "shaders/post_easu.frag.msl.h"
#include "shaders/post_rcas.frag.msl.h"
#include "shaders/post_smaa_edges.frag.msl.h"
#include "shaders/post_smaa_weights.frag.msl.h"
#include "shaders/post_smaa_blend.frag.msl.h"
#include "shaders/osd.frag.msl.h"
#include "shaders/osd.vert.msl.h"
#include "shaders/present.frag.msl.h"
#include "shaders/present.vert.msl.h"
#include "shaders/psx_draw.frag.msl.h"
#include "shaders/psx_draw_layer.frag.msl.h"
#include "shaders/psx_draw.vert.msl.h"
#include "shaders/vram_copy.frag.msl.h"
#include "shaders/vram_copy.vert.msl.h"
#endif
#ifdef DW3_HAVE_D3D12
#include "shaders/osd.frag.dxil.h"
#include "shaders/osd.vert.dxil.h"
#include "shaders/present.frag.dxil.h"
#include "shaders/present.vert.dxil.h"
#include "shaders/psx_draw.frag.dxil.h"
#include "shaders/psx_draw_layer.frag.dxil.h"
#include "shaders/psx_draw.vert.dxil.h"
#include "shaders/vram_copy.frag.dxil.h"
#include "shaders/vram_copy.vert.dxil.h"
#include "shaders/post.vert.dxil.h"
#include "shaders/post_scale.frag.dxil.h"
#include "shaders/post_cas.frag.dxil.h"
#include "shaders/post_grade.frag.dxil.h"
#include "shaders/post_crt.frag.dxil.h"
#include "shaders/post_easu.frag.dxil.h"
#include "shaders/post_rcas.frag.dxil.h"
#include "shaders/post_smaa_edges.frag.dxil.h"
#include "shaders/post_smaa_weights.frag.dxil.h"
#include "shaders/post_smaa_blend.frag.dxil.h"
#endif

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
    EXPECT_TRUE(is_spirv(psx_draw_layer_frag_spv, psx_draw_layer_frag_spv_size));
    EXPECT_TRUE(is_spirv(present_vert_spv, present_vert_spv_size));
    EXPECT_TRUE(is_spirv(present_frag_spv, present_frag_spv_size));
    EXPECT_TRUE(is_spirv(osd_vert_spv, osd_vert_spv_size));
    EXPECT_TRUE(is_spirv(osd_frag_spv, osd_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_vert_spv, post_vert_spv_size));
    EXPECT_TRUE(is_spirv(post_scale_frag_spv, post_scale_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_cas_frag_spv, post_cas_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_grade_frag_spv, post_grade_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_crt_frag_spv, post_crt_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_easu_frag_spv, post_easu_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_rcas_frag_spv, post_rcas_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_smaa_edges_frag_spv, post_smaa_edges_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_smaa_weights_frag_spv, post_smaa_weights_frag_spv_size));
    EXPECT_TRUE(is_spirv(post_smaa_blend_frag_spv, post_smaa_blend_frag_spv_size));
}

#ifdef DW3_HAVE_D3D12
namespace {
bool is_dxil(const uint8_t* data, std::size_t size) { return size > 4 && std::memcmp(data, "DXBC", 4) == 0; }
}  // namespace

TEST(Shaders, CompiledToDxil) {
    using namespace dw3::shaders;
    EXPECT_TRUE(is_dxil(psx_draw_vert_dxil, psx_draw_vert_dxil_size));
    EXPECT_TRUE(is_dxil(psx_draw_frag_dxil, psx_draw_frag_dxil_size));
    EXPECT_TRUE(is_dxil(psx_draw_layer_frag_dxil, psx_draw_layer_frag_dxil_size));
    EXPECT_TRUE(is_dxil(present_vert_dxil, present_vert_dxil_size));
    EXPECT_TRUE(is_dxil(present_frag_dxil, present_frag_dxil_size));
    EXPECT_TRUE(is_dxil(osd_vert_dxil, osd_vert_dxil_size));
    EXPECT_TRUE(is_dxil(osd_frag_dxil, osd_frag_dxil_size));
    EXPECT_TRUE(is_dxil(vram_copy_vert_dxil, vram_copy_vert_dxil_size));
    EXPECT_TRUE(is_dxil(vram_copy_frag_dxil, vram_copy_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_vert_dxil, post_vert_dxil_size));
    EXPECT_TRUE(is_dxil(post_scale_frag_dxil, post_scale_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_cas_frag_dxil, post_cas_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_grade_frag_dxil, post_grade_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_crt_frag_dxil, post_crt_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_easu_frag_dxil, post_easu_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_rcas_frag_dxil, post_rcas_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_smaa_edges_frag_dxil, post_smaa_edges_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_smaa_weights_frag_dxil, post_smaa_weights_frag_dxil_size));
    EXPECT_TRUE(is_dxil(post_smaa_blend_frag_dxil, post_smaa_blend_frag_dxil_size));
}
#endif

#ifdef DW3_HAVE_METAL
namespace {
// Source text with its terminating NUL, defining the entry point the renderer asks for.
bool is_msl(const uint8_t* data, std::size_t size) {
    if (size < 2 || data[size - 1] != 0) return false;
    const std::string text(reinterpret_cast<const char*>(data), size - 1);
    return text.find("#include <metal_stdlib>") != std::string::npos && text.find(" main0(") != std::string::npos;
}
}  // namespace

TEST(Shaders, ConvertedToMsl) {
    using namespace dw3::shaders;
    EXPECT_TRUE(is_msl(osd_frag_msl, osd_frag_msl_size));
    EXPECT_TRUE(is_msl(osd_vert_msl, osd_vert_msl_size));
    EXPECT_TRUE(is_msl(present_frag_msl, present_frag_msl_size));
    EXPECT_TRUE(is_msl(present_vert_msl, present_vert_msl_size));
    EXPECT_TRUE(is_msl(psx_draw_frag_msl, psx_draw_frag_msl_size));
    EXPECT_TRUE(is_msl(psx_draw_layer_frag_msl, psx_draw_layer_frag_msl_size));
    EXPECT_TRUE(is_msl(psx_draw_vert_msl, psx_draw_vert_msl_size));
    EXPECT_TRUE(is_msl(vram_copy_frag_msl, vram_copy_frag_msl_size));
    EXPECT_TRUE(is_msl(vram_copy_vert_msl, vram_copy_vert_msl_size));
    EXPECT_TRUE(is_msl(post_vert_msl, post_vert_msl_size));
    EXPECT_TRUE(is_msl(post_scale_frag_msl, post_scale_frag_msl_size));
    EXPECT_TRUE(is_msl(post_cas_frag_msl, post_cas_frag_msl_size));
    EXPECT_TRUE(is_msl(post_grade_frag_msl, post_grade_frag_msl_size));
    EXPECT_TRUE(is_msl(post_crt_frag_msl, post_crt_frag_msl_size));
    EXPECT_TRUE(is_msl(post_easu_frag_msl, post_easu_frag_msl_size));
    EXPECT_TRUE(is_msl(post_rcas_frag_msl, post_rcas_frag_msl_size));
    EXPECT_TRUE(is_msl(post_smaa_edges_frag_msl, post_smaa_edges_frag_msl_size));
    EXPECT_TRUE(is_msl(post_smaa_weights_frag_msl, post_smaa_weights_frag_msl_size));
    EXPECT_TRUE(is_msl(post_smaa_blend_frag_msl, post_smaa_blend_frag_msl_size));
}
#endif

namespace {

using namespace dw3;

constexpr int kPixels = kVramWidth * kVramHeight;

// Test parameter: the graphics API and the internal resolution. It converts to the resolution so that the tests can
// use it as one.
struct GpuParam {
    GraphicsApi api;
    int scale;
    operator int() const { return scale; }
};

const char* api_name(GraphicsApi api) {
    return api == GraphicsApi::D3D12 ? "D3D12" : api == GraphicsApi::Metal ? "Metal" : "Vulkan";
}

void PrintTo(const GpuParam& p, std::ostream* os) { *os << api_name(p.api) << " " << p.scale << "x"; }

std::string gpu_param_name(const ::testing::TestParamInfo<GpuParam>& info) {
    return std::string(api_name(info.param.api)) + "_" + std::to_string(info.param.scale) + "x";
}

// Every compiled-in graphics API at each of the given internal resolutions.
std::vector<GpuParam> gpu_params(std::initializer_list<int> scales) {
    std::vector<GpuParam> out;
    for (GraphicsApi api : {GraphicsApi::Vulkan, GraphicsApi::D3D12, GraphicsApi::Metal})
        if (HwRenderer::api_compiled_in(api))
            for (int scale : scales) out.push_back({api, scale});
    return out;
}

std::unique_ptr<HwRenderer> create_renderer(const GpuParam& p, std::string& error, HwOptions options = {}) {
    options.api = p.api;
    return HwRenderer::create(nullptr, {}, error, p.scale, options);
}

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

class HwCompare : public ::testing::TestWithParam<GpuParam> {
protected:
    void SetUp() override {
        std::string error;
        hw = create_renderer(GetParam(), error);
        if (!hw) GTEST_SKIP() << "no GPU device: " << error;
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

TEST_P(HwCompare, TrianglesAreExact) {
    // Colour and texel coordinates are evaluated from the software rasterizer's own planes.
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
    EXPECT_EQ(d.count, 0) << "differing pixels";
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
    EXPECT_EQ(d.count, 0) << "differing pixels";
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

INSTANTIATE_TEST_SUITE_P(InternalRes, HwCompare, ::testing::ValuesIn(gpu_params({1, 2, 3, 4})), gpu_param_name);
INSTANTIATE_TEST_SUITE_P(InternalRes, HwVramOps, ::testing::ValuesIn(gpu_params({1, 2, 3, 4})), gpu_param_name);

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
        for (int x = 0; x < r.w * n; ++x) bad += img[static_cast<std::size_t>(y) * r.w * n + x] != v[(r.y + y / n) * kVramWidth + r.x + x / n];
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

INSTANTIATE_TEST_SUITE_P(InternalRes, HwScaled, ::testing::ValuesIn(gpu_params({2, 3, 4, 8})), gpu_param_name);

// ---- display layer: true colour and texture filtering ---------------------------------------------------

// 5-bit word -> the RGBA8 colour a default renderer shows for it.
uint32_t expand_word(uint16_t w) {
    auto e = [](uint32_t v) { return (v << 3) | (v >> 2); };
    return e(w & 31) | (e((w >> 5) & 31) << 8) | (e((w >> 10) & 31) << 16) | 0xFF000000u;
}
int chan(uint32_t px, int c) { return static_cast<int>((px >> (8 * c)) & 255); }

// Base of the display layer tests: a renderer with the given options, run against the software rasterizer.
class HwLayerBase : public ::testing::TestWithParam<GpuParam> {
protected:
    virtual HwOptions options() const = 0;
    void SetUp() override {
        std::string error;
        hw = create_renderer(GetParam(), error, options());
        if (!hw) GTEST_SKIP() << "no GPU device: " << error;
        vram.assign(kPixels, 0);
        ref.assign(kPixels, 0);
        s.area_x2 = 319;
        s.area_y2 = 299;
    }
    int scale() const { return GetParam(); }
    // Draws on both renderers; the GPU's VRAM must stay equal to the software one.
    template <typename F>
    void draw(F fn) {
        SoftRasterizer soft;
        fn(soft, ref, s);
        DrawState copy = s;
        fn(*hw, vram, copy);
    }
    void upload() {
        ref = vram;
        hw->vram_written(vram, {});
    }
    // Game-visible state is identical to the software rasterizer's.
    int vram_mismatches() {
        std::vector<uint16_t> got(kPixels);
        hw->read_vram(got);
        return compare(ref, got).count;
    }
    // Colour shown for a native-pixel region, (w*N) x (h*N).
    std::vector<uint32_t> shown(const VramRect& r) {
        std::vector<uint32_t> img;
        EXPECT_TRUE(hw->read_display(r, img));
        return img;
    }
    // Colour shown at a subpixel of the region.
    uint32_t at(const std::vector<uint32_t>& img, const VramRect& r, int sx, int sy) const {
        return img[static_cast<std::size_t>(sy) * r.w * scale() + sx];
    }
    // Number of subpixels of the region that differ from the exact 5-bit colour of their native pixel.
    int differs_from_exact(const VramRect& r) {
        const auto img = shown(r);
        std::vector<uint16_t> got(kPixels);
        hw->read_vram(got);
        int n = 0;
        for (int y = 0; y < r.h * scale(); ++y)
            for (int x = 0; x < r.w * scale(); ++x)
                n += at(img, r, x, y) != expand_word(got[(r.y + y / scale()) * kVramWidth + r.x + x / scale()]);
        return n;
    }
    // The random primitive mix of RandomPrimitives.
    void random_mix() {
        draw([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) {
            std::mt19937 rng(99);
            auto rnd = [&](int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)); };
            for (int i = 0; i < 400; ++i) {
                st.depth = static_cast<TexDepth>(rnd(0, 2));
                st.semi_mode = static_cast<uint8_t>(rnd(0, 3));
                st.dither = rnd(0, 1);
                st.set_mask = rnd(0, 7) == 0;
                st.check_mask = rnd(0, 7) == 0;
                PrimitiveFlags f;
                f.textured = rnd(0, 1);
                f.raw_texture = rnd(0, 3) == 0;
                f.semi_transparent = rnd(0, 2) == 0;
                f.clut = static_cast<uint16_t>(((480 + rnd(0, 3)) << 6) | rnd(0, 8));
                const int x = rnd(0, 280), y = rnd(0, 200);
                GpuVertex o = gv(x, y, rnd(0, 255), rnd(0, 255), rnd(0, 255), rnd(0, 120), rnd(0, 120));
                if (i % 3 == 0) {
                    r.rect(v, st, f, o, rnd(1, 40), rnd(1, 40));
                } else if (i % 3 == 1) {
                    r.line(v, st, f, o, gv(rnd(0, 300), rnd(0, 230), rnd(0, 255), rnd(0, 255), rnd(0, 255)));
                } else {
                    f.gouraud = rnd(0, 1);
                    GpuVertex t[3] = {o, gv(x + rnd(5, 60), y + rnd(-10, 40), rnd(0, 255), rnd(0, 255), rnd(0, 255), rnd(0, 120), rnd(0, 120)),
                                      gv(x + rnd(-10, 40), y + rnd(5, 60), rnd(0, 255), rnd(0, 255), rnd(0, 255), rnd(0, 120), rnd(0, 120))};
                    r.triangle(v, st, f, t);
                }
            }
        });
    }
    // Two triangles covering the box with the given corner texel coordinates (u0,v0 top-left, u1,v1 bottom-right).
    static void quad(Rasterizer& r, std::vector<uint16_t>& v, const DrawState& st, const PrimitiveFlags& f, int x0, int y0,
                     int x1, int y1, int u0, int v0, int u1, int v1) {
        GpuVertex a[3] = {gv(x0, y0, 128, 128, 128, u0, v0), gv(x1, y0, 128, 128, 128, u1, v0), gv(x0, y1, 128, 128, 128, u0, v1)};
        GpuVertex b[3] = {gv(x1, y0, 128, 128, 128, u1, v0), gv(x1, y1, 128, 128, 128, u1, v1), gv(x0, y1, 128, 128, 128, u0, v1)};
        r.triangle(v, st, f, a);
        r.triangle(v, st, f, b);
    }

    std::unique_ptr<HwRenderer> hw;
    std::vector<uint16_t> vram, ref;
    DrawState s;
};

class HwTrueColor : public HwLayerBase {
protected:
    HwOptions options() const override {
        HwOptions o;
        o.true_color = true;
        return o;
    }
    // Horizontal red ramp: red = x - 0 over a large triangle (x up to 256).
    void ramp(Rasterizer& r, std::vector<uint16_t>& v, const DrawState& st, int y0) {
        PrimitiveFlags f;
        f.gouraud = true;
        GpuVertex t[3] = {gv(0, y0, 0, 0, 0), gv(256, y0, 255, 0, 0), gv(0, y0 + 40, 0, 0, 0)};
        r.triangle(v, st, f, t);
        GpuVertex u[3] = {gv(256, y0, 255, 0, 0), gv(256, y0 + 40, 255, 0, 0), gv(0, y0 + 40, 0, 0, 0)};
        r.triangle(v, st, f, u);
    }
};

TEST_P(HwTrueColor, GameVisibleVramStaysExact) {
    upload();
    random_mix();
    EXPECT_EQ(vram_mismatches(), 0);
}

TEST_P(HwTrueColor, ShadedAreasKeepFullPrecisionWithoutDither) {
    upload();
    s.dither = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { ramp(r, v, st, 10); });
    EXPECT_EQ(vram_mismatches(), 0);  // the exact state still dithers and quantises
    const VramRect row{20, 25, 200, 1};
    const auto img = shown(row);
    std::vector<int> reds;
    for (int x = 0; x < row.w * scale(); ++x) reds.push_back(chan(at(img, row, x, 0), 0));
    EXPECT_TRUE(std::is_sorted(reds.begin(), reds.end())) << "dither or quantisation noise";
    std::sort(reds.begin(), reds.end());
    reds.erase(std::unique(reds.begin(), reds.end()), reds.end());
    EXPECT_GT(reds.size(), 150u) << "distinct red levels (a 5-bit image has at most 32)";
}

TEST_P(HwTrueColor, DrawnTexturesAreQuantisedAsOnTheConsole) {
    upload();
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) {
        ramp(r, v, st, 256);
        st.tex_base_x = 0;
        st.tex_base_y = 256;
        st.depth = TexDepth::Bit15;
        PrimitiveFlags t;
        t.textured = true;
        t.raw_texture = true;
        r.rect(v, st, t, gv(20, 100, 128, 128, 128, 0, 10), 200, 4);
    });
    EXPECT_EQ(vram_mismatches(), 0);
    // The copy made through the texture only has 5-bit levels; the ramp it came from has more.
    const VramRect copy{20, 100, 200, 1}, source{20, 270, 200, 1};
    auto levels = [&](const VramRect& r) {
        const auto img = shown(r);
        std::vector<int> reds;
        for (int x = 0; x < r.w * scale(); ++x) reds.push_back(chan(at(img, r, x, 0), 0));
        std::sort(reds.begin(), reds.end());
        reds.erase(std::unique(reds.begin(), reds.end()), reds.end());
        return reds.size();
    };
    EXPECT_LE(levels(copy), 32u);
    EXPECT_GT(levels(source), 100u);
}

TEST_P(HwTrueColor, SemiTransparencyBlendsWithTheFullPrecisionBackground) {
    upload();
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { ramp(r, v, st, 10); });
    const VramRect row{20, 25, 200, 2};
    const auto before = shown(row);
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) {
        PrimitiveFlags f;
        f.semi_transparent = true;
        st.semi_mode = 0;
        r.rect(v, st, f, gv(20, 24, 200, 0, 0), 200, 4);
    });
    EXPECT_EQ(vram_mismatches(), 0);
    const auto after = shown(row);
    int worst = 0;
    for (int x = 0; x < row.w * scale(); ++x) {
        const int expect = (chan(at(before, row, x, 0), 0) + 200) / 2;
        worst = std::max(worst, std::abs(chan(at(after, row, x, 0), 0) - expect));
    }
    EXPECT_LE(worst, 1);
}

TEST_P(HwTrueColor, FillsAndUploadsOverEnhancedPixelsShowTheirExactColour) {
    upload();
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { ramp(r, v, st, 10); });
    ASSERT_TRUE(hw->vram_fill({40, 12, 20, 20}, 0x03E0));
    for (int y = 12; y < 32; ++y)
        for (int x = 40; x < 60; ++x) vram[y * kVramWidth + x] = ref[y * kVramWidth + x] = 0x03E0;
    for (int y = 12; y < 32; ++y)
        for (int x = 100; x < 120; ++x) vram[y * kVramWidth + x] = ref[y * kVramWidth + x] = 0x4210;
    hw->vram_written(vram, {100, 12, 20, 20});
    for (const VramRect& r : {VramRect{40, 12, 20, 20}, VramRect{100, 12, 20, 20}}) {
        const auto img = shown(r);
        const uint32_t want = expand_word(r.x == 40 ? 0x03E0 : 0x4210);
        int bad = 0;
        for (int y = 0; y < r.h * scale(); ++y)
            for (int x = 0; x < r.w * scale(); ++x) bad += at(img, r, x, y) != want;
        EXPECT_EQ(bad, 0) << "region at x " << r.x;
    }
}

INSTANTIATE_TEST_SUITE_P(InternalRes, HwTrueColor, ::testing::ValuesIn(gpu_params({1, 2, 4})), gpu_param_name);

class HwBilinear : public HwLayerBase {
protected:
    HwOptions options() const override {
        HwOptions o;
        o.filter = TextureFilter::Bilinear;
        return o;
    }
};

TEST_P(HwBilinear, GameVisibleVramStaysExact) {
    upload();
    random_mix();
    EXPECT_EQ(vram_mismatches(), 0);
}

TEST_P(HwBilinear, StretchedTexturesBlendThroughTheClut) {
    // 8-bit texture, two texels: index 0 (red) and index 1 (blue) through the CLUT.
    vram[512] = 0x0100;  // u = 0 -> index 0, u = 1 -> index 1
    vram[480 * kVramWidth + 0] = 0x001F;
    vram[480 * kVramWidth + 1] = 0x7C00;
    upload();
    s.tex_base_x = 512;
    s.depth = TexDepth::Bit8;
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    f.clut = static_cast<uint16_t>(480 << 6);
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { quad(r, v, st, f, 20, 20, 52, 24, 0, 0, 1, 0); });
    EXPECT_EQ(vram_mismatches(), 0);  // exact state is the nearest texel
    const VramRect row{20, 22, 32, 1};
    const auto img = shown(row);
    std::vector<int> reds, blues;
    for (int x = 0; x < row.w * scale(); ++x) {
        reds.push_back(chan(at(img, row, x, 0), 0));
        blues.push_back(chan(at(img, row, x, 0), 2));
    }
    EXPECT_TRUE(std::is_sorted(reds.rbegin(), reds.rend())) << "red falls from texel 0 to texel 1";
    EXPECT_TRUE(std::is_sorted(blues.begin(), blues.end()));
    // Drawn pixels start where the nearest texel is opaque; in the middle the colours are mixed.
    const int mid = row.w * scale() * 3 / 4;
    EXPECT_GT(reds[mid], 20);
    EXPECT_GT(blues[mid], 20);
    EXPECT_GT(blues.back(), reds.back());
}

TEST_P(HwBilinear, TransparentTexelsLeaveNoDarkHalo) {
    // Texel 0 transparent, texels 1.. one opaque colour; stretched, the pixels near the edge must keep that colour.
    const uint16_t c = 20 | (25 << 5) | (10 << 10);
    vram[512] = 0;
    for (int x = 1; x < 8; ++x) vram[512 + x] = c;
    upload();
    s.tex_base_x = 512;
    s.depth = TexDepth::Bit15;
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { quad(r, v, st, f, 20, 20, 44, 26, 0, 0, 3, 0); });
    EXPECT_EQ(vram_mismatches(), 0);
    const VramRect box{16, 20, 32, 6};
    const auto img = shown(box);
    int opaque = 0, off = 0;
    for (int y = 0; y < box.h * scale(); ++y)
        for (int x = 0; x < box.w * scale(); ++x) {
            const uint32_t px = at(img, box, x, y);
            if (px == 0xFF000000u) continue;
            ++opaque;
            off += px != expand_word(c);
        }
    EXPECT_GT(opaque, 100);
    EXPECT_EQ(off, 0) << "pixels not equal to the opaque texel colour";
}

TEST_P(HwBilinear, TapsStayInsideTheTextureBoundsOfThePrimitive) {
    // Red texels sit just outside the used texels, including the wrap-around neighbour of texel 0.
    const uint16_t blue = 0x7C00, red = 0x001F;
    for (int x = 0; x < 256; ++x) vram[512 + x] = red;
    vram[512] = blue;
    vram[513] = blue;
    upload();
    s.tex_base_x = 512;
    s.depth = TexDepth::Bit15;
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { quad(r, v, st, f, 20, 20, 36, 24, 0, 0, 1, 0); });
    const VramRect box{16, 20, 24, 4};
    const auto img = shown(box);
    int reddish = 0, drawn = 0;
    for (int y = 0; y < box.h * scale(); ++y)
        for (int x = 0; x < box.w * scale(); ++x) {
            const uint32_t px = at(img, box, x, y);
            if (px == 0xFF000000u) continue;
            ++drawn;
            reddish += chan(px, 0) != 0;
        }
    EXPECT_GT(drawn, 50);
    EXPECT_EQ(reddish, 0);
}

TEST_P(HwBilinear, SpritesAndTilesStayPixelExact) {
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) vram[y * kVramWidth + 512 + x] = static_cast<uint16_t>(((x * 7 + y * 13) & 0x7FFF) | 1);
    upload();
    s.tex_base_x = 512;
    s.depth = TexDepth::Bit15;
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) {
        r.rect(v, st, f, gv(20, 20, 128, 128, 128, 3, 5), 40, 30);
        quad(r, v, st, f, 100, 20, 140, 50, 3, 5, 43, 35);  // one-to-one triangles
    });
    EXPECT_EQ(vram_mismatches(), 0);
    EXPECT_EQ(differs_from_exact({20, 20, 40, 30}), 0);
    EXPECT_EQ(differs_from_exact({100, 20, 40, 30}), 0);
}

INSTANTIATE_TEST_SUITE_P(InternalRes, HwBilinear, ::testing::ValuesIn(gpu_params({1, 2, 4})), gpu_param_name);

class HwXbr : public HwLayerBase {
protected:
    HwOptions options() const override {
        HwOptions o;
        o.filter = TextureFilter::Xbr;
        return o;
    }
};

TEST_P(HwXbr, GameVisibleVramStaysExact) {
    upload();
    random_mix();
    EXPECT_EQ(vram_mismatches(), 0);
}

TEST_P(HwXbr, DiagonalEdgesAreSmoothedOnlyAboveNativeResolution) {
    const uint16_t a = 0x001F, b = 0x7C00;
    for (int v = 0; v < 16; ++v)
        for (int u = 0; u < 16; ++u) vram[v * kVramWidth + 512 + u] = (u + v < 16) ? a : b;
    upload();
    s.tex_base_x = 512;
    s.depth = TexDepth::Bit15;
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { r.rect(v, st, f, gv(40, 40, 128, 128, 128, 0, 0), 16, 16); });
    EXPECT_EQ(vram_mismatches(), 0);
    const VramRect box{40, 40, 16, 16};
    const int changed = differs_from_exact(box);
    if (scale() == 1) {
        EXPECT_EQ(changed, 0);
        return;
    }
    const int total = 16 * 16 * scale() * scale();
    EXPECT_GT(changed, 0) << "the diagonal should be smoothed";
    EXPECT_LT(changed, total / 4) << "only the neighbourhood of the edge changes";
    // Only mixes of the two sprite colours appear.
    const auto img = shown(box);
    int green = 0;
    for (int y = 0; y < box.h * scale(); ++y)
        for (int x = 0; x < box.w * scale(); ++x) green += chan(at(img, box, x, y), 1) != 0;
    EXPECT_EQ(green, 0);
}

TEST_P(HwXbr, OpaqueSpriteEdgesAreNotEaten) {
    // A solid square on a transparent background keeps its silhouette and colour.
    const uint16_t c = 0x3DEF;
    for (int v = 4; v < 12; ++v)
        for (int u = 4; u < 12; ++u) vram[v * kVramWidth + 512 + u] = c;
    upload();
    s.tex_base_x = 512;
    s.depth = TexDepth::Bit15;
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { r.rect(v, st, f, gv(40, 40, 128, 128, 128, 0, 0), 16, 16); });
    EXPECT_EQ(vram_mismatches(), 0);
    EXPECT_EQ(differs_from_exact({40, 40, 16, 16}), 0);
}

INSTANTIATE_TEST_SUITE_P(InternalRes, HwXbr, ::testing::ValuesIn(gpu_params({1, 4})), gpu_param_name);

// Without any display option the shown image is the exact image.
TEST_P(HwCompare, DefaultOutputIsTheExactColour) {
    Scene sc;
    SoftRasterizer soft;
    hw->vram_written(sc.vram, {});
    DrawState st = sc.s;
    PrimitiveFlags f;
    f.gouraud = true;
    st.dither = true;
    GpuVertex t[3] = {gv(20, 20, 255, 0, 0), gv(200, 40, 0, 255, 0), gv(60, 200, 0, 0, 255)};
    hw->triangle(sc.vram, st, f, t);
    std::vector<uint32_t> img;
    const VramRect box{30, 40, 60, 40};
    ASSERT_TRUE(hw->read_display(box, img));
    std::vector<uint16_t> got(kPixels);
    hw->read_vram(got);
    const int n = hw->internal_scale();
    int bad = 0;
    for (int y = 0; y < box.h * n; ++y)
        for (int x = 0; x < box.w * n; ++x)
            bad += (img[static_cast<std::size_t>(y) * box.w * n + x] & 0xFFFFFF) != (expand_word(got[(box.y + y / n) * kVramWidth + box.x + x / n]) & 0xFFFFFF);
    // With scale above 1 the gouraud subpixels follow the plane smoothly; only the top-left one is the exact word.
    if (n == 1) EXPECT_EQ(bad, 0);
}

// Precise vertices are only an input; without the option the renderer must not look at them.
TEST_P(HwCompare, PreciseVerticesAreIgnoredWithoutTheOption) {
    Diff d = run([](Rasterizer& r, std::vector<uint16_t>& v, DrawState& s) {
        PrimitiveFlags f;
        f.gouraud = true;
        f.textured = true;
        s.depth = TexDepth::Bit15;
        f.clut = static_cast<uint16_t>(480 << 6);
        GpuVertex t[3] = {gv(20, 20, 255, 0, 0, 0, 0), gv(200, 40, 0, 255, 0, 200, 10), gv(60, 200, 0, 0, 255, 30, 220)};
        for (GpuVertex& g : t) {
            g.fx = static_cast<float>(g.x) + 0.6f;
            g.fy = static_cast<float>(g.y) + 0.3f;
            g.w = 100.0f + static_cast<float>(g.x);
            g.precise = true;
        }
        r.triangle(v, s, f, t);
    });
    EXPECT_EQ(d.count, 0);
}

class HwPrecise : public ::testing::TestWithParam<GpuParam> {
protected:
    void SetUp() override {
        std::string error;
        HwOptions o;
        o.precise_geometry = true;
        hw = create_renderer(GetParam(), error, o);
        if (!hw) GTEST_SKIP() << "no GPU device: " << error;
        vram.assign(kPixels, 0);
        s.area_x2 = 319;
        s.area_y2 = 239;
        s.tex_base_x = 512;
        s.depth = TexDepth::Bit15;
        // 15-bit texels that encode their own coordinates: u in the red field, v in green, blue set so that no
        // texel is transparent.
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) vram[y * kVramWidth + 512 + x] = static_cast<uint16_t>(x | (y << 5) | (1 << 10));
        hw->vram_written(vram, {});
    }
    int scale() const { return GetParam(); }

    static GpuVertex pv(int x, int y, float dx, float dy, float w, int u = 0, int v = 0) {
        GpuVertex g = gv(x, y, 255, 255, 255, u, v);
        g.fx = static_cast<float>(x) + dx;
        g.fy = static_cast<float>(y) + dy;
        g.w = w;
        g.precise = true;
        return g;
    }
    void clear() { ASSERT_TRUE(hw->vram_fill({0, 0, 320, 240}, 0)); }

    // Mean subpixel column of the lit pixels of a region of the scaled target.
    double centroid_x(const VramRect& r, int* count = nullptr) {
        std::vector<uint16_t> px;
        EXPECT_TRUE(hw->read_scaled(r, px));
        double sum = 0;
        int n = 0;
        const int w = r.w * scale();
        for (std::size_t i = 0; i < px.size(); ++i)
            if (px[i]) {
                sum += static_cast<double>(i % static_cast<std::size_t>(w));
                ++n;
            }
        if (count) *count = n;
        return n ? sum / n : 0.0;
    }

    std::unique_ptr<HwRenderer> hw;
    std::vector<uint16_t> vram;
    DrawState s;
};

// A triangle moved by a quarter pixel at a time must move on the scaled target instead of snapping to the pixel
// grid. At native resolution there is no finer grid: positions stay integer there.
TEST_P(HwPrecise, SlowlyMovingTriangleDoesNotSnapAboveNativeResolution) {
    const VramRect box{10, 10, 110, 110};
    PrimitiveFlags f;
    double prev = 0, first = 0;
    for (int step = 0; step < 4; ++step) {
        clear();
        const float d = 0.25f * static_cast<float>(step);
        GpuVertex t[3] = {pv(20, 20, d, 0, 100), pv(100, 20, d, 0, 100), pv(20, 100, d, 0, 100)};
        hw->triangle(vram, s, f, t);
        const double cx = centroid_x(box);
        if (step == 0) first = cx;
        else if (scale() > 1) EXPECT_GT(cx, prev + 0.3) << "step " << step;
        prev = cx;
    }
    // Three quarter pixels at scale N are 0.75 * N subpixels.
    EXPECT_NEAR(prev - first, scale() > 1 ? 0.75 * scale() : 0.0, 0.35);
}

// The same movement of integer vertices snaps: nothing changes until a whole pixel.
TEST_P(HwPrecise, IntegerVerticesStillSnapToPixels) {
    const VramRect box{10, 10, 110, 110};
    PrimitiveFlags f;
    clear();
    GpuVertex a[3] = {gv(20, 20), gv(100, 20), gv(20, 100)};
    hw->triangle(vram, s, f, a);
    const double base = centroid_x(box);
    clear();
    GpuVertex b[3] = {gv(21, 20), gv(101, 20), gv(21, 100)};
    hw->triangle(vram, s, f, b);
    EXPECT_NEAR(centroid_x(box) - base, static_cast<double>(scale()), 0.05);
}

// At native resolution a precise triangle covers exactly the pixels of the integer one; above it the sub-pixel
// positions only nudge the edges.
TEST_P(HwPrecise, CoverageMatchesTheIntegerTriangleToWithinItsEdges) {
    PrimitiveFlags f;
    GpuVertex p[3] = {pv(20, 20, 0.7f, 0.2f, 500), pv(100, 30, 0.1f, 0.9f, 900), pv(30, 100, 0.5f, 0.5f, 700)};
    GpuVertex i[3] = {gv(20, 20, 255, 255, 255), gv(100, 30, 255, 255, 255), gv(30, 100, 255, 255, 255)};
    const VramRect box{10, 10, 110, 110};
    clear();
    hw->triangle(vram, s, f, p);
    int count_p = 0, count_i = 0;
    std::vector<uint16_t> got_p(kPixels), got_i(kPixels);
    centroid_x(box, &count_p);
    hw->read_vram(got_p);
    clear();
    hw->triangle(vram, s, f, i);
    centroid_x(box, &count_i);
    hw->read_vram(got_i);
    if (scale() == 1) EXPECT_EQ(compare(got_p, got_i).count, 0);
    EXPECT_NEAR(static_cast<double>(count_p), static_cast<double>(count_i), 0.03 * count_i);
}

// Texel coordinates of a quad whose right side is four times as deep follow 1/w, not the screen.
TEST_P(HwPrecise, TexelCoordinatesArePerspectiveCorrectOnAQuadWithDepth) {
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    const float wl = 1000.0f, wr = 4000.0f;
    const int x0 = 40, x1 = 200;
    GpuVertex v0 = pv(x0, 20, 0, 0, wl, 0, 0), v1 = pv(x1, 20, 0, 0, wr, 31, 0);
    GpuVertex v2 = pv(x0, 60, 0, 0, wl, 0, 31), v3 = pv(x1, 60, 0, 0, wr, 31, 31);
    clear();
    GpuVertex t0[3] = {v0, v1, v2}, t1[3] = {v1, v2, v3};
    hw->triangle(vram, s, f, t0);
    hw->triangle(vram, s, f, t1);
    std::vector<uint16_t> got(kPixels);
    hw->read_vram(got);
    int worst = 0, midpoint_gap = 0;
    for (int x = x0 + 4; x < x1 - 4; ++x) {
        const double t = static_cast<double>(x - x0) / (x1 - x0);
        const double persp = (31.0 / wr * t) / ((1 - t) / wl + t / wr);
        const double affine = 31.0 * t;
        const int u = got[40 * kVramWidth + x] & 31;
        worst = std::max(worst, static_cast<int>(std::abs(u - std::floor(persp + 0.5))));
        if (x == (x0 + x1) / 2) midpoint_gap = static_cast<int>(std::abs(std::floor(persp + 0.5) - std::floor(affine + 0.5)));
    }
    EXPECT_LE(worst, 1);
    EXPECT_GT(midpoint_gap, 5);  // the test would not tell the two apart otherwise
}

// Without a depth on every vertex the triangle is affine, like the integer path.
TEST_P(HwPrecise, TexelCoordinatesStayAffineWhenAVertexHasNoDepth) {
    PrimitiveFlags f;
    f.textured = true;
    f.raw_texture = true;
    GpuVertex a = pv(40, 20, 0, 0, 1000, 0, 0), b = pv(200, 20, 0, 0, 4000, 31, 0), c = gv(40, 60, 128, 128, 128, 0, 31);
    clear();
    GpuVertex t[3] = {a, b, c};
    hw->triangle(vram, s, f, t);
    std::vector<uint16_t> got(kPixels);
    hw->read_vram(got);
    const int u = got[22 * kVramWidth + 120] & 31;
    EXPECT_NEAR(u, 15.5, 1.5);  // halfway along the top edge
}

INSTANTIATE_TEST_SUITE_P(InternalRes, HwPrecise, ::testing::ValuesIn(gpu_params({1, 4})), gpu_param_name);

// ---- replacement textures ------------------------------------------------------------------------------------------

// A 32x16 texel (16x16 word) 8-bit image at (640, 0) with its palette at (0, 480), and a pack that replaces it at
// scale 2 with an image whose colour encodes the position.
struct ReplacementScene {
    static constexpr int kPackScale = 2;
    static constexpr int kBlockX = 640, kBlockY = 0, kBlockWords = 16, kBlockRows = 16;
    static void fill(std::vector<uint16_t>& vram) {
        for (int y = 0; y < kBlockRows; ++y)
            for (int x = 0; x < kBlockWords; ++x) {
                const int t = x * 2 + y * 3;
                vram[(kBlockY + y) * kVramWidth + kBlockX + x] = static_cast<uint16_t>((t & 255) | (((t + 1) & 255) << 8));
            }
        for (int i = 0; i < 256; ++i)
            vram[480 * kVramWidth + i] = static_cast<uint16_t>((i & 31) | (((i >> 3) & 31) << 5) | (((i * 7) & 31) << 10) | 0x0400);
    }
    static uint32_t pixel(int x, int y) {
        return 0xFF000000u | (static_cast<uint32_t>((x * 4) & 255)) | (static_cast<uint32_t>((y * 8) & 255) << 8) | (150u << 16);
    }
    static std::shared_ptr<const texrep::TexturePack> pack() {
        std::vector<uint16_t> vram(kPixels, 0);
        fill(vram);
        const auto dir = std::filesystem::temp_directory_path() / "dw3_hw_replacement_pack";
        std::filesystem::create_directories(dir);
        texrep::Image img;
        img.w = kBlockWords * 2 * kPackScale;
        img.h = kBlockRows * kPackScale;
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) img.rgba.push_back(pixel(x, y));
        std::string error;
        texrep::write_png(dir / "block.png", img, error);
        texrep::PackManifest m;
        m.scale = kPackScale;
        m.entries.push_back({texrep::hash_vram_rect(vram, {kBlockX, kBlockY, kBlockX + kBlockWords, kBlockY + kBlockRows}),
                             texrep::hash_clut(vram, 0, 480), kBlockWords * 2, kBlockRows, "block.png"});
        return texrep::TexturePack::from_manifest(m, dir);
    }
};

class HwReplacement : public HwLayerBase {
protected:
    HwOptions options() const override {
        HwOptions o;
        o.texture_pack = ReplacementScene::pack();
        return o;
    }
    void upload_scene() {
        ReplacementScene::fill(vram);
        ref = vram;
        hw->vram_written(vram, {ReplacementScene::kBlockX, ReplacementScene::kBlockY, ReplacementScene::kBlockWords,
                                ReplacementScene::kBlockRows});
        hw->vram_written(vram, {0, 480, 256, 1});
        s.tex_base_x = 640;
        s.tex_base_y = 0;
        s.depth = TexDepth::Bit8;
    }
    PrimitiveFlags sprite_flags() const {
        PrimitiveFlags f;
        f.textured = true;
        f.clut = static_cast<uint16_t>(480 << 6);
        return f;
    }
    void draw_sprite(int x, int y, int w, int h, int tint = 128) {
        const PrimitiveFlags f = sprite_flags();
        draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { r.rect(v, st, f, gv(x, y, tint, tint, tint, 0, 0), w, h); });
    }
    // Bilinear sample of the replacement image at the middle of subpixel (sx, sy) of the sprite drawn at (x, y).
    static uint32_t expected(int sx, int sy, int n, int tint) {
        const int rs = ReplacementScene::kPackScale, iw = ReplacementScene::kBlockWords * 2 * rs, ih = ReplacementScene::kBlockRows * rs;
        const double px = (sx + 0.5) / n * rs - 0.5, py = (sy + 0.5) / n * rs - 0.5;
        const int x0 = static_cast<int>(std::floor(px)), y0 = static_cast<int>(std::floor(py));
        const double fx = px - x0, fy = py - y0;
        auto at = [&](int x, int y) { return pixel_of(std::clamp(x, 0, iw - 1), std::clamp(y, 0, ih - 1)); };
        uint32_t out = 0xFF000000u;
        for (int c = 0; c < 3; ++c) {
            auto ch = [&](int x, int y) { return static_cast<double>((at(x, y) >> (8 * c)) & 255); };
            const double v = (ch(x0, y0) * (1 - fx) + ch(x0 + 1, y0) * fx) * (1 - fy) + (ch(x0, y0 + 1) * (1 - fx) + ch(x0 + 1, y0 + 1) * fx) * fy;
            out |= static_cast<uint32_t>(std::min(255.0, v * tint / 128.0) + 0.5) << (8 * c);
        }
        return out;
    }
    static uint32_t pixel_of(int x, int y) { return ReplacementScene::pixel(x, y); }
    int worst_channel_error(const VramRect& r, int tint, int first_col = 0) {
        const auto img = shown(r);
        int worst = 0;
        for (int y = 0; y < r.h * scale(); ++y)
            for (int x = first_col * scale(); x < r.w * scale(); ++x) {
                const uint32_t want = expected(x, y, scale(), tint), got = at(img, r, x, y);
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(chan(want, c) - chan(got, c)));
            }
        return worst;
    }
};

TEST_P(HwReplacement, KnownImageIsShownFromTheReplacement) {
    upload_scene();
    draw_sprite(20, 20, 32, 16);
    const VramRect r{20, 20, 32, 16};
    EXPECT_LE(worst_channel_error(r, 128), 2);
    EXPECT_EQ(vram_mismatches(), 0) << "the exact picture is the original image";
}

TEST_P(HwReplacement, ReplacementIsModulatedByTheSpriteColourLikeTheOriginal) {
    upload_scene();
    draw_sprite(20, 20, 32, 16, 64);
    EXPECT_LE(worst_channel_error({20, 20, 32, 16}, 64), 2);
    EXPECT_EQ(vram_mismatches(), 0);
}

TEST_P(HwReplacement, PartialSpritesUseTheMatchingPartOfTheReplacement) {
    upload_scene();
    s.tex_base_x = 640;
    const PrimitiveFlags f = sprite_flags();
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { r.rect(v, st, f, gv(60, 40, 128, 128, 128, 10, 4), 12, 8); });
    const VramRect r{60, 40, 12, 8};
    const auto img = shown(r);
    // Replacement position of the sprite's first subpixel: texel (10, 4) of the original, scaled.
    const int rs = ReplacementScene::kPackScale;
    int worst = 0;
    for (int y = 0; y < r.h * scale(); ++y)
        for (int x = 0; x < r.w * scale(); ++x) {
            const double px = (10.0 + (x + 0.5) / scale()) * rs - 0.5, py = (4.0 + (y + 0.5) / scale()) * rs - 0.5;
            if (scale() != rs) continue;  // exact only where samples fall on replacement texel centres
            const uint32_t want = ReplacementScene::pixel(static_cast<int>(std::lround(px)), static_cast<int>(std::lround(py)));
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(chan(want, c) - chan(at(img, r, x, y), c)));
        }
    EXPECT_LE(worst, 2);
    EXPECT_EQ(vram_mismatches(), 0);
}

TEST_P(HwReplacement, SpriteLargerThanTheBlockIsReplacedOnlyOverTheBlock) {
    upload_scene();
    draw_sprite(20, 20, 48, 16);  // 16 texels beyond the block's right edge
    const VramRect inside{20, 20, 32, 16}, outside{52, 20, 16, 16};
    EXPECT_LE(worst_channel_error(inside, 128), 2);
    EXPECT_EQ(differs_from_exact(outside), 0) << "what lies outside the block is drawn as the game drew it";
    EXPECT_EQ(vram_mismatches(), 0);
}

TEST_P(HwReplacement, ChangedPaletteMeansNoReplacement) {
    upload_scene();
    vram[480 * kVramWidth + 5] ^= 0x0021;  // one palette entry: another variant of the image, not in the pack
    ref = vram;
    hw->vram_written(vram, {0, 480, 256, 1});
    draw_sprite(20, 20, 32, 16);
    EXPECT_EQ(differs_from_exact({20, 20, 32, 16}), 0);
    EXPECT_EQ(vram_mismatches(), 0);
}

TEST_P(HwReplacement, ReuploadedBlockWithOtherContentIsNotReplaced) {
    upload_scene();
    for (int y = 0; y < ReplacementScene::kBlockRows; ++y) vram[(ReplacementScene::kBlockY + y) * kVramWidth + ReplacementScene::kBlockX + 3] ^= 0x0101;
    ref = vram;
    hw->vram_written(vram, {ReplacementScene::kBlockX, ReplacementScene::kBlockY, ReplacementScene::kBlockWords,
                            ReplacementScene::kBlockRows});
    draw_sprite(20, 20, 32, 16);
    EXPECT_EQ(differs_from_exact({20, 20, 32, 16}), 0);
}

TEST_P(HwReplacement, DrawingOverTheBlockEndsItsReplacement) {
    upload_scene();
    draw_sprite(20, 20, 32, 16);
    EXPECT_LE(worst_channel_error({20, 20, 32, 16}, 128), 2);
    ASSERT_TRUE(hw->vram_fill({ReplacementScene::kBlockX, ReplacementScene::kBlockY, 4, 4}, 0x1234));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) ref[(ReplacementScene::kBlockY + y) * kVramWidth + ReplacementScene::kBlockX + x] = vram[(ReplacementScene::kBlockY + y) * kVramWidth + ReplacementScene::kBlockX + x] = 0x1234;
    draw_sprite(80, 20, 32, 16);
    EXPECT_EQ(differs_from_exact({80, 20, 32, 16}), 0);
}

TEST_P(HwReplacement, SemiTransparentAndFlippedSpritesAreNeverReplaced) {
    upload_scene();
    PrimitiveFlags f = sprite_flags();
    f.semi_transparent = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { r.rect(v, st, f, gv(20, 20, 128, 128, 128, 0, 0), 32, 16); });
    EXPECT_EQ(differs_from_exact({20, 20, 32, 16}), 0);
    f.semi_transparent = false;
    s.rect_flip_x = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { r.rect(v, st, f, gv(80, 20, 128, 128, 128, 31, 0), 32, 16); });
    EXPECT_EQ(differs_from_exact({80, 20, 32, 16}), 0);
}

TEST_P(HwReplacement, OtherTexturesAndTrianglesAreUntouched) {
    upload_scene();
    s.depth = TexDepth::Bit15;
    PrimitiveFlags f;
    f.textured = true;
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { r.rect(v, st, f, gv(120, 20, 128, 128, 128, 0, 0), 32, 16); });
    EXPECT_EQ(differs_from_exact({120, 20, 32, 16}), 0);
    EXPECT_EQ(vram_mismatches(), 0);
}

TEST_P(HwReplacement, ManySpritesAndAtlasReuseKeepWorking) {
    upload_scene();
    for (int i = 0; i < 40; ++i) draw_sprite(10 + (i % 8) * 36, 10 + (i / 8) * 20, 32, 16);
    EXPECT_LE(worst_channel_error({10, 10, 32, 16}, 128), 2);
    EXPECT_EQ(vram_mismatches(), 0);
}

INSTANTIATE_TEST_SUITE_P(InternalRes, HwReplacement, ::testing::ValuesIn(gpu_params({2, 4})), gpu_param_name);

// Without a pack nothing changes: the shown picture is the exact one even though the same blocks are uploaded.
class HwReplacementOff : public HwLayerBase {
protected:
    HwOptions options() const override { return {}; }
};

TEST_P(HwReplacementOff, NoPackShowsTheOriginal) {
    ReplacementScene::fill(vram);
    ref = vram;
    hw->vram_written(vram, {ReplacementScene::kBlockX, ReplacementScene::kBlockY, ReplacementScene::kBlockWords, ReplacementScene::kBlockRows});
    hw->vram_written(vram, {0, 480, 256, 1});
    s.tex_base_x = 640;
    s.depth = TexDepth::Bit8;
    PrimitiveFlags f;
    f.textured = true;
    f.clut = static_cast<uint16_t>(480 << 6);
    draw([&](Rasterizer& r, std::vector<uint16_t>& v, DrawState& st) { r.rect(v, st, f, gv(20, 20, 128, 128, 128, 0, 0), 32, 16); });
    EXPECT_EQ(differs_from_exact({20, 20, 32, 16}), 0);
    EXPECT_EQ(vram_mismatches(), 0);
}

INSTANTIATE_TEST_SUITE_P(InternalRes, HwReplacementOff, ::testing::ValuesIn(gpu_params({1, 2})), gpu_param_name);

// A texture pack can be switched on and off on a running renderer.
TEST(HwReplacementReconfigure, PackCanBeEnabledAndDisabled) {
    for (GraphicsApi api : {GraphicsApi::Vulkan, GraphicsApi::D3D12, GraphicsApi::Metal}) {
        if (!HwRenderer::api_compiled_in(api)) continue;
        std::string error;
        HwOptions o;
        o.api = api;
        auto hw = HwRenderer::create(nullptr, {}, error, 2, o);
        if (!hw) GTEST_SKIP() << "no GPU device: " << error;
        std::vector<uint16_t> vram(kPixels, 0);
        ReplacementScene::fill(vram);
        hw->vram_written(vram, {});
        o.texture_pack = ReplacementScene::pack();
        ASSERT_TRUE(hw->reconfigure(2, o, vram, error)) << error;
        hw->vram_written(vram, {ReplacementScene::kBlockX, ReplacementScene::kBlockY, ReplacementScene::kBlockWords, ReplacementScene::kBlockRows});
        DrawState s;
        s.area_x2 = 319;
        s.area_y2 = 239;
        s.tex_base_x = 640;
        s.depth = TexDepth::Bit8;
        PrimitiveFlags f;
        f.textured = true;
        f.clut = static_cast<uint16_t>(480 << 6);
        hw->rect(vram, s, f, gv(20, 20, 128, 128, 128, 0, 0), 32, 16);
        std::vector<uint32_t> img;
        ASSERT_TRUE(hw->read_display({20, 20, 32, 16}, img));
        EXPECT_EQ(img[0] & 0xFFFFFF, ReplacementScene::pixel(0, 0) & 0xFFFFFF) << "replacement on";
        o.texture_pack = nullptr;
        ASSERT_TRUE(hw->reconfigure(2, o, vram, error)) << error;
        EXPECT_EQ(hw->internal_scale(), 2);
    }
}

// The wide frame copy: while mirroring is on, draws into a frame buffer also land, shifted, in extra render-target
// columns, 3D geometry beyond the 4:3 area fills the margins and 2D shapes stay centred.
class HwWideFrame : public ::testing::TestWithParam<GpuParam> {};

TEST_P(HwWideFrame, MarginsShowTheWorldAndTheHudStaysCentred) {
    std::string error;
    HwOptions o;
    o.wide_frame = make_wide_frame(WideAspect::Ratio16x9);
    auto hw = create_renderer(GetParam(), error, o);
    if (!hw) GTEST_SKIP() << "no GPU device: " << error;
    std::vector<uint16_t> vram(kPixels, 0);
    hw->vram_written(vram, {});
    DrawState world;
    world.area_x2 = 319;
    world.area_y2 = 239;
    world.off_x = 160;
    world.off_y = 120;
    DrawState hud = world;
    hud.off_x = hud.off_y = 0;
    PrimitiveFlags flat;
    DisplayState display;
    display.enabled = true;

    // Mirroring off: nothing reaches the copy, and the 4:3 picture is shown.
    hw->rect(vram, world, flat, gv(0, 0, 0, 0, 0), 320, 240);
    std::vector<uint32_t> img;
    ASSERT_TRUE(hw->render_to_image(display, ScaleMode::Fit, false, nullptr, 424, 240, img));
    EXPECT_EQ(img[100 * 424 + 4] & 0xFFFFFF, 0u) << "pillarbox";

    hw->set_wide_mirror(true);
    hw->rect(vram, world, flat, gv(0, 0, 0, 0, 0), 320, 240);                               // clear, full width
    const GpuVertex red[3] = {gv(-40, 100, 248, 0, 0), gv(-10, 100, 248, 0, 0), gv(-40, 130, 248, 0, 0)};
    hw->triangle(vram, world, flat, red);                                                  // wholly outside the 4:3 area
    hw->rect(vram, hud, flat, gv(10, 10, 0, 248, 0), 20, 20);                               // HUD box
    hw->rect(vram, hud, flat, gv(0, 200, 0, 0, 248), 320, 10);                              // HUD bar touching both edges
    ASSERT_TRUE(hw->render_to_image(display, ScaleMode::Fit, false, nullptr, 424, 240, img));
    auto px = [&](int x, int y) { return img[static_cast<std::size_t>(y) * 424 + x] & 0xFFFFFF; };
    constexpr uint32_t kGreen = 0x00FF00, kBlue = 0xFF0000;
    EXPECT_EQ(px(17, 105) & 0xFF, 0xFFu) << "world triangle in the left margin";
    EXPECT_EQ(px(17, 105) & 0xFFFF00, 0u);
    EXPECT_EQ(px(52 + 20, 20), kGreen) << "HUD box centred";
    EXPECT_EQ(px(20, 20), 0u) << "nothing of the HUD in the margin";
    EXPECT_EQ(px(3, 205), kBlue) << "edge-touching bar stretches into the left margin";
    EXPECT_EQ(px(420, 205), kBlue) << "and the right one";

    // The picture the game sees is unchanged: the triangle was clipped away and the extra columns are not VRAM.
    std::vector<uint16_t> back(kPixels);
    hw->read_vram(back);
    EXPECT_EQ(back[105 * kVramWidth + 2], 0u);
    EXPECT_EQ(back[205 * kVramWidth + 100], static_cast<uint16_t>(31 << 10));

    // Stopping the mirror returns to the 4:3 picture.
    hw->set_wide_mirror(false);
    ASSERT_TRUE(hw->render_to_image(display, ScaleMode::Fit, false, nullptr, 424, 240, img));
    EXPECT_EQ(px(3, 205), 0u);
    EXPECT_EQ(px(53 + 20, 20), kGreen);
}

INSTANTIATE_TEST_SUITE_P(Wide, HwWideFrame, ::testing::ValuesIn(gpu_params({1, 2})), gpu_param_name);

// Wide fields need a map library: without one the field mode is not available, and with one a frame whose map cannot be
// resolved (no background sprites drawn) is shown at 4:3 with borders, so the margins never show leftovers.
class HwWideField : public ::testing::TestWithParam<GpuParam> {};

TEST_P(HwWideField, UnresolvedFramesStayFourByThree) {
    std::string error;
    HwOptions o;
    o.wide_frame = make_wide_frame(WideAspect::Ratio16x9);
    o.stage_library = std::make_shared<texrep::StageLibrary>();
    o.stage_library->finish();
    auto hw = create_renderer(GetParam(), error, o);
    if (!hw) GTEST_SKIP() << "no GPU device: " << error;
    std::vector<uint16_t> vram(kPixels, 0);
    hw->vram_written(vram, {});
    DrawState world;
    world.area_x2 = 319;
    world.area_y2 = 239;
    PrimitiveFlags flat;
    DisplayState display;
    display.enabled = true;
    std::vector<uint32_t> img;
    auto px = [&](int x, int y) { return img[static_cast<std::size_t>(y) * 424 + x] & 0xFFFFFF; };

    hw->set_wide_mode(WideMode::Field);
    hw->rect(vram, world, flat, gv(0, 0, 0, 0, 248), 320, 240);  // a full-width fill, no background sprite
    hw->display_flipped();
    ASSERT_TRUE(hw->render_to_image(display, ScaleMode::Fit, false, nullptr, 424, 240, img));
    EXPECT_EQ(px(3, 100), 0u) << "pillarbox: the copy is not shown";
    EXPECT_EQ(px(212, 100), 0xFF0000u) << "the 4:3 picture is";
    EXPECT_EQ(hw->stats().wide_field_fallbacks, 1u);
    EXPECT_EQ(hw->stats().wide_field_frames, 0u);

    // Battles do not need a map: the same draw is shown wide.
    hw->set_wide_mode(WideMode::Battle);
    hw->rect(vram, world, flat, gv(0, 0, 0, 0, 248), 320, 240);
    ASSERT_TRUE(hw->render_to_image(display, ScaleMode::Fit, false, nullptr, 424, 240, img));
    EXPECT_EQ(px(3, 100), 0xFF0000u) << "the margin is filled";
}

TEST_P(HwWideField, FieldModeNeedsALibrary) {
    std::string error;
    HwOptions o;
    o.wide_frame = make_wide_frame(WideAspect::Ratio16x9);
    auto hw = create_renderer(GetParam(), error, o);
    if (!hw) GTEST_SKIP() << "no GPU device: " << error;
    std::vector<uint16_t> vram(kPixels, 0);
    hw->vram_written(vram, {});
    DrawState world;
    world.area_x2 = 319;
    world.area_y2 = 239;
    DisplayState display;
    display.enabled = true;
    hw->set_wide_mode(WideMode::Field);
    hw->rect(vram, world, PrimitiveFlags{}, gv(0, 0, 0, 0, 248), 320, 240);
    std::vector<uint32_t> img;
    ASSERT_TRUE(hw->render_to_image(display, ScaleMode::Fit, false, nullptr, 424, 240, img));
    EXPECT_EQ(img[100 * 424 + 3] & 0xFFFFFF, 0u);
}

INSTANTIATE_TEST_SUITE_P(WideField, HwWideField, ::testing::ValuesIn(gpu_params({1})), gpu_param_name);


// The HDR output transform of the final post-processing pass against the CPU formulas of hdr.h. A headless renderer
// has an 8-bit capture target, so the encoded values that fit it are compared: HDR10 PQ, and EDR (relative to paper
// white) without expansion.
class HwHdrOutput : public ::testing::TestWithParam<GpuParam> {};

TEST_P(HwHdrOutput, ShaderMatchesTheCpuMaths) {
    std::string error;
    auto hw = create_renderer(GetParam(), error);
    if (!hw) GTEST_SKIP() << "no GPU device: " << error;
    constexpr int kSteps = 32;
    std::vector<uint16_t> vram(kPixels, 0);
    for (int i = 0; i < kSteps; ++i)
        for (int y = 0; y < 2; ++y) vram[y * kVramWidth + i] = static_cast<uint16_t>(i | (i << 5) | (i << 10));
    hw->vram_written(vram, {0, 0, kSteps, 2});
    DisplayState display;
    display.enabled = true;
    display.width = kSteps;
    display.height = 2;

    struct Case {
        HdrParams params;
        float tolerance;  // in 8-bit steps
    };
    const Case cases[] = {{{HdrMode::Pq, 1000.0f, 203.0f}, 2.0f}, {{HdrMode::Pq, 600.0f, 300.0f}, 2.0f}, {{HdrMode::Edr, 203.0f, 203.0f}, 1.5f}};
    for (const Case& c : cases) {
        PostFx post;
        post.hdr = c.params;
        std::vector<uint32_t> img;
        ASSERT_TRUE(hw->render_to_image(display, ScaleMode::Stretch, false, &post, kSteps, 2, img));
        for (int i = 0; i < kSteps; ++i) {
            const float v = static_cast<float>((i << 3) | (i >> 2)) / 255.0f;
            const auto want = hdr_encode_pixel(v, v, v, c.params);
            for (int ch = 0; ch < 3; ++ch) {
                const float got = static_cast<float>((img[i] >> (8 * ch)) & 0xFF);
                EXPECT_NEAR(got, std::clamp(want[ch], 0.0f, 1.0f) * 255.0f, c.tolerance)
                    << "mode " << static_cast<int>(c.params.mode) << " step " << i << " channel " << ch;
            }
        }
    }

    // Off leaves the picture alone.
    PostFx plain;
    plain.sharpness = 0.0f;
    plain.scaler = OutputScaler::Linear;  // any active option sends the picture through the post-processing chain
    std::vector<uint32_t> img;
    ASSERT_TRUE(hw->render_to_image(display, ScaleMode::Stretch, false, &plain, kSteps, 2, img));
    for (int i = 0; i < kSteps; ++i) EXPECT_EQ(img[i] & 0xFF, static_cast<uint32_t>((i << 3) | (i >> 2))) << i;
}

INSTANTIATE_TEST_SUITE_P(Hdr, HwHdrOutput, ::testing::ValuesIn(gpu_params({1})), gpu_param_name);


}  // namespace
#endif  // DW3_HW_RENDERER
