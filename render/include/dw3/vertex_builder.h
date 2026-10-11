#pragma once

// Conversion of decoded GPU primitives into vertices for the GPU renderer, plus the bookkeeping needed to know
// which parts of VRAM a primitive reads and writes. Pure functions with no graphics API dependency.

#include <cstdint>
#include <vector>

#include "dw3/gpu.h"
#include "dw3/vram_regions.h"

namespace dw3 {

// Vertex layout shared with psx_draw.vert (32 bytes).
struct HwVertex {
    float x = 0, y = 0;                    // VRAM pixel coordinates
    uint8_t r = 0, g = 0, b = 0, a = 255;  // vertex colour
    float u = 0, v = 0;                    // texel coordinates, sampled at pixel centres
    uint32_t state0 = 0, state1 = 0, state2 = 0;
};
static_assert(sizeof(HwVertex) == 32);

// Packed per-primitive state (see the header comment of psx_draw.frag for the bit layout).
struct HwState {
    uint32_t s0 = 0, s1 = 0, s2 = 0;
};

namespace state_bits {
constexpr uint32_t kTextured = 1u << 0;
constexpr uint32_t kRaw = 1u << 1;
constexpr uint32_t kSemi = 1u << 2;
constexpr uint32_t kDither = 1u << 3;
constexpr uint32_t kCheckMask = 1u << 4;
constexpr uint32_t kSetMask = 1u << 5;
}  // namespace state_bits

// True when textures are sampled for this primitive.
inline bool uses_texture(const DrawState& s, const PrimitiveFlags& f) { return f.textured && !s.texture_disable; }

// Packs the draw state. `dither` is the effective dithering decision for the primitive kind.
HwState pack_state(const DrawState& s, const PrimitiveFlags& f, bool dither);

// Effective dithering, following the rules of the software rasterizer: polygons dither when shaded or
// modulated, lines when gouraud shaded, rectangles never.
bool triangle_dithers(const DrawState& s, const PrimitiveFlags& f);
bool line_dithers(const DrawState& s, const PrimitiveFlags& f);

// The drawing area clipped to VRAM, as a half-open region (empty if the area is inverted).
Region clip_region(const DrawState& s);

// Appends the three vertices of a triangle. Vertices sit on pixel centres so that GPU rasterization of the
// pixel-centre samples follows the PS1 top-left fill rule.
void build_triangle(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3]);

// Appends six vertices (two triangles) covering the width x height rectangle.
void build_rect(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
                int height);

// Appends one pixel quad (six vertices) per line pixel, inside the drawing area, using the same stepping and
// rounding as the software rasterizer. Returns the number of pixels.
int build_line(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a,
               const GpuVertex& b);

// VRAM pixels a primitive may write: its bounding box, clipped to the drawing area.
Region triangle_bounds(const DrawState& s, const GpuVertex v[3]);
Region rect_bounds(const DrawState& s, const GpuVertex& v, int width, int height);
Region line_bounds(const DrawState& s, const GpuVertex& a, const GpuVertex& b);

// VRAM words a textured primitive may read: the texel area for texel coordinates in [umin,umax] x [vmin,vmax]
// (the whole texture page if a texture window is active), and the CLUT row.
Region texture_region(const DrawState& s, int umin, int umax, int vmin, int vmax);
Region clut_region(const DrawState& s, const PrimitiveFlags& f);
Region triangle_texture_region(const DrawState& s, const GpuVertex v[3]);
Region rect_texture_region(const DrawState& s, const GpuVertex& v, int width, int height);

}  // namespace dw3
