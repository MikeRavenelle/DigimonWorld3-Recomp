#pragma once

// Conversion of decoded GPU primitives into vertices for the GPU renderer, plus the bookkeeping needed to know
// which parts of VRAM a primitive reads and writes. Pure functions with no graphics API dependency.

#include <cstdint>
#include <vector>

#include "dw3/gpu.h"
#include "dw3/vram_regions.h"

namespace dw3 {

// Attribute planes. Colour and texel coordinates are not interpolated by the GPU: every vertex of a primitive
// carries the same fixed-point planes the software rasterizer sets up, and the fragment shader evaluates them
// at its native pixel, so the values are the software rasterizer's bit for bit.
// A plane gives value(x, y) = (base + dx * x + dy * y) / 4096 in 20.12 fixed point, with the rounding offset
// (0x800) folded into the base and the terms taken modulo 2^32 (the sum of a pixel inside the primitive is small).
enum PlaneIndex { kPlaneR = 0, kPlaneG = 3, kPlaneB = 6, kPlaneU = 9, kPlaneV = 12, kPlaneInts = 15 };

// Vertex layout shared with psx_draw.vert (84 bytes).
struct HwVertex {
    float x = 0, y = 0;                // VRAM pixel coordinates
    int32_t plane[kPlaneInts] = {};    // colour r, g, b and texel coordinates u, v: base, dx, dy each
    uint32_t state0 = 0, state1 = 0, state2 = 0;
    uint32_t uv_rect = 0;              // texel coordinate bounds of the primitive: u min, u max, v min, v max (bytes)
};
static_assert(sizeof(HwVertex) == 84);

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
constexpr uint32_t kUnitUv = 1u << 10;  // texels map 1:1 to pixels on the axes (sprites, tiles): not bilinear filtered
constexpr uint32_t kPrecise = 1u << 11;  // float attributes instead of planes (see build_precise_triangle)
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

// Appends the three vertices of a triangle. Vertices sit on the sample position of the first subpixel of their
// native pixel (the pixel centre at scale 1) so that GPU rasterization follows the PS1 top-left fill rule.
// `scale` is the internal resolution multiplier.
void build_triangle(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3],
                    int scale = 1);

// True when at least one of the three vertices carries a sub-pixel position.
bool has_precise(const GpuVertex v[3]);

// Float path for triangles with GTE sub-pixel positions (precise geometry). Instead of the fixed-point planes the
// vertices carry float attributes in the plane slots (colour r, g, b, texel u, v and depth w, as float bits) that
// the GPU interpolates itself: colour in screen space, texel coordinates perspective-correct when all vertices
// have a depth. At scale > 1 vertices with a precise position are placed at it; at scale 1 positions stay on the
// integer grid so coverage equals build_triangle's and only the attribute interpolation differs. Vertices
// without a precise position use their integer position.
void build_precise_triangle(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3],
                            int scale = 1);

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
