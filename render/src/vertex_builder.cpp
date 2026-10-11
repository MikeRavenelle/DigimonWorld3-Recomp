#include "dw3/vertex_builder.h"

#include <algorithm>
#include <bit>
#include <cstdlib>

namespace dw3 {

namespace {

int64_t floor_div(int64_t a, int64_t b) {  // b > 0
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

struct Plane {
    int64_t base = 0, dx = 0, dy = 0;
};

struct Planes {
    Plane p[5];  // r, g, b, u, v
};

Plane flat_plane(int value) { return {(static_cast<int64_t>(value) << 12) + 0x800, 0, 0}; }

// Same setup as the software rasterizer (20.12 fixed point, division truncating toward zero), for vertices
// ordered counter-clockwise on screen with the given doubled area.
Plane triangle_plane(const GpuVertex* const p[3], int64_t area, int a0, int a1, int a2) {
    const int64_t d1 = a1 - a0, d2 = a2 - a0;
    Plane q;
    q.base = (static_cast<int64_t>(a0) << 12) + 0x800;
    q.dx = ((d1 * (p[2]->y - p[0]->y) - d2 * (p[1]->y - p[0]->y)) * 4096) / area;
    q.dy = ((d2 * (p[1]->x - p[0]->x) - d1 * (p[2]->x - p[0]->x)) * 4096) / area;
    return q;
}

// Stores a plane relative to the VRAM origin instead of the first vertex, so the shader needs no vertex position.
void store_plane(int32_t* out, const Plane& q, int x0, int y0) {
    out[0] = static_cast<int32_t>(static_cast<uint32_t>(q.base - q.dx * x0 - q.dy * y0));
    out[1] = static_cast<int32_t>(static_cast<uint32_t>(q.dx));
    out[2] = static_cast<int32_t>(static_cast<uint32_t>(q.dy));
}

uint32_t pack_rect(int umin, int umax, int vmin, int vmax) {
    return static_cast<uint32_t>(umin) | (static_cast<uint32_t>(umax) << 8) | (static_cast<uint32_t>(vmin) << 16) |
           (static_cast<uint32_t>(vmax) << 24);
}

HwVertex make_vertex(const HwState& st, float x, float y, const Planes& pl, int ox, int oy, uint32_t uv_rect) {
    HwVertex out;
    out.x = x;
    out.y = y;
    for (int i = 0; i < 5; ++i) store_plane(out.plane + i * 3, pl.p[i], ox, oy);
    out.state0 = st.s0;
    out.state1 = st.s1;
    out.state2 = st.s2;
    out.uv_rect = uv_rect;
    return out;
}

Planes flat_colour(int r, int g, int b) {
    Planes pl;
    pl.p[0] = flat_plane(r);
    pl.p[1] = flat_plane(g);
    pl.p[2] = flat_plane(b);
    return pl;
}

// A plane is a one-to-one mapping when each texel coordinate follows exactly one screen axis at a step of one.
bool unit_mapping(const Plane& u, const Plane& v) {
    constexpr int64_t kSlack = 64;
    auto near = [](int64_t value, int64_t target) { return std::abs(value - target) <= kSlack; };
    auto step = [&](int64_t value) { return near(value, 4096) || near(value, -4096); };
    const bool straight = step(u.dx) && near(u.dy, 0) && near(v.dx, 0) && step(v.dy);
    const bool turned = near(u.dx, 0) && step(u.dy) && step(v.dx) && near(v.dy, 0);
    return straight || turned;
}

Region clipped(const DrawState& s, int x0, int y0, int x1, int y1) {
    return clip_region(s).intersection({x0, y0, x1, y1});
}

}  // namespace

bool triangle_dithers(const DrawState& s, const PrimitiveFlags& f) {
    return s.dither && (f.gouraud || (uses_texture(s, f) && !f.raw_texture));
}

bool line_dithers(const DrawState& s, const PrimitiveFlags& f) { return s.dither && f.gouraud; }

HwState pack_state(const DrawState& s, const PrimitiveFlags& f, bool dither) {
    using namespace state_bits;
    HwState st;
    const bool tex = uses_texture(s, f);
    st.s0 = (tex ? kTextured : 0u) | (tex && f.raw_texture ? kRaw : 0u) | (f.semi_transparent ? kSemi : 0u) |
            (dither ? kDither : 0u) | (s.check_mask ? kCheckMask : 0u) | (s.set_mask ? kSetMask : 0u) |
            (static_cast<uint32_t>(s.semi_mode & 3) << 6) | (static_cast<uint32_t>(s.depth) << 8);
    if (tex) {
        st.s1 = static_cast<uint32_t>(f.clut) | (static_cast<uint32_t>(s.tex_base_x / 64) << 16) |
                (static_cast<uint32_t>(s.tex_base_y / 256) << 20);
        st.s2 = static_cast<uint32_t>(s.win_mask_x & 31) | (static_cast<uint32_t>(s.win_mask_y & 31) << 5) |
                (static_cast<uint32_t>(s.win_off_x & 31) << 10) | (static_cast<uint32_t>(s.win_off_y & 31) << 15);
    }
    return st;
}

Region clip_region(const DrawState& s) {
    Region r{std::max(s.area_x1, 0), std::max(s.area_y1, 0), std::min(s.area_x2, kVramWidth - 1) + 1,
             std::min(s.area_y2, kVramHeight - 1) + 1};
    return r.empty() ? Region{} : r;
}

void build_triangle(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3],
                    int scale) {
    HwState st = pack_state(s, f, triangle_dithers(s, f));
    const bool tex = uses_texture(s, f);
    // Attribute planes, set up in the software rasterizer's vertex order.
    const GpuVertex* p[3] = {&v[0], &v[1], &v[2]};
    int64_t area = static_cast<int64_t>(p[1]->x - p[0]->x) * (p[2]->y - p[0]->y) -
                   static_cast<int64_t>(p[2]->x - p[0]->x) * (p[1]->y - p[0]->y);
    if (area < 0) {
        std::swap(p[1], p[2]);
        area = -area;
    }
    Planes pl = flat_colour(p[0]->r, p[0]->g, p[0]->b);
    uint32_t rect = 0;
    if (area != 0) {
        if (f.gouraud) {
            pl.p[0] = triangle_plane(p, area, p[0]->r, p[1]->r, p[2]->r);
            pl.p[1] = triangle_plane(p, area, p[0]->g, p[1]->g, p[2]->g);
            pl.p[2] = triangle_plane(p, area, p[0]->b, p[1]->b, p[2]->b);
        }
        if (tex) {
            pl.p[3] = triangle_plane(p, area, p[0]->u, p[1]->u, p[2]->u);
            pl.p[4] = triangle_plane(p, area, p[0]->v, p[1]->v, p[2]->v);
            if (unit_mapping(pl.p[3], pl.p[4])) st.s0 |= state_bits::kUnitUv;
            rect = pack_rect(std::min({v[0].u, v[1].u, v[2].u}), std::max({v[0].u, v[1].u, v[2].u}),
                             std::min({v[0].v, v[1].v, v[2].v}), std::max({v[0].v, v[1].v, v[2].v}));
        }
    }
    // The sample of a native pixel sits at its centre at 1x. At higher resolutions the top-left subpixel of each
    // native pixel is that sample, so the vertices move by half a subpixel: that subpixel then rasterizes exactly
    // as the native pixel does, and a quad of triangles covers the same subpixels as a rectangle of the same size.
    const float origin = 0.5f / static_cast<float>(scale);
    for (int i = 0; i < 3; ++i)
        out.push_back(make_vertex(st, static_cast<float>(v[i].x) + origin, static_cast<float>(v[i].y) + origin, pl, p[0]->x, p[0]->y, rect));
}

bool has_precise(const GpuVertex v[3]) { return v[0].precise || v[1].precise || v[2].precise; }

void build_precise_triangle(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3],
                            int scale) {
    HwState st = pack_state(s, f, triangle_dithers(s, f));
    st.s0 |= state_bits::kPrecise;
    const bool tex = uses_texture(s, f);
    const bool persp = v[0].precise && v[1].precise && v[2].precise;
    uint32_t rect = 0;
    if (tex)
        rect = pack_rect(std::min({v[0].u, v[1].u, v[2].u}), std::max({v[0].u, v[1].u, v[2].u}),
                         std::min({v[0].v, v[1].v, v[2].v}), std::max({v[0].v, v[1].v, v[2].v}));
    // Same half-subpixel origin as build_triangle. The +0.5 on the attributes matches the planes' rounding offset:
    // the shader takes the floor.
    const float origin = 0.5f / static_cast<float>(scale);
    for (int i = 0; i < 3; ++i) {
        const GpuVertex& c = f.gouraud ? v[i] : v[0];
        const bool sub = scale > 1 && v[i].precise;
        const float attr[6] = {static_cast<float>(c.r) + 0.5f, static_cast<float>(c.g) + 0.5f,
                               static_cast<float>(c.b) + 0.5f, tex ? static_cast<float>(v[i].u) + 0.5f : 0.0f,
                               tex ? static_cast<float>(v[i].v) + 0.5f : 0.0f, persp ? v[i].w : 0.0f};
        HwVertex h;
        h.x = (sub ? v[i].fx : static_cast<float>(v[i].x)) + origin;
        h.y = (sub ? v[i].fy : static_cast<float>(v[i].y)) + origin;
        for (int k = 0; k < 6; ++k) h.plane[k] = std::bit_cast<int32_t>(attr[k]);
        h.state0 = st.s0;
        h.state1 = st.s1;
        h.state2 = st.s2;
        h.uv_rect = rect;
        out.push_back(h);
    }
}

void build_rect(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
                int height) {
    HwState st = pack_state(s, f, false);
    const bool tex = uses_texture(s, f);
    // Texel coordinates run by +-1 per pixel from the start coordinate at the first pixel.
    Planes pl = flat_colour(v.r, v.g, v.b);
    uint32_t rect = 0;
    if (tex) {
        const int64_t su = s.rect_flip_x ? -4096 : 4096, sv = s.rect_flip_y ? -4096 : 4096;
        pl.p[3] = {(static_cast<int64_t>(v.u) << 12) + 0x800 - su * v.x, su, 0};
        pl.p[4] = {(static_cast<int64_t>(v.v) << 12) + 0x800 - sv * v.y, 0, sv};
        st.s0 |= state_bits::kUnitUv;
        auto span = [](int start, int len, bool flip, int& lo, int& hi) {
            const int end = flip ? start - (len - 1) : start + (len - 1);
            lo = std::min(start, end);
            hi = std::max(start, end);
            if (lo < 0 || hi > 255) lo = 0, hi = 255;
        };
        int u0, u1, v0, v1;
        span(v.u, width, s.rect_flip_x, u0, u1);
        span(v.v, height, s.rect_flip_y, v0, v1);
        rect = pack_rect(u0, u1, v0, v1);
    }
    const float x0 = static_cast<float>(v.x), y0 = static_cast<float>(v.y);
    const float x1 = x0 + static_cast<float>(width), y1 = y0 + static_cast<float>(height);
    auto vert = [&](float x, float y) { return make_vertex(st, x, y, pl, 0, 0, rect); };
    const HwVertex tl = vert(x0, y0), tr = vert(x1, y0), bl = vert(x0, y1), br = vert(x1, y1);
    out.insert(out.end(), {tl, tr, bl, tr, br, bl});
}

int build_line(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a,
               const GpuVertex& b) {
    PrimitiveFlags lf = f;
    lf.textured = false;
    const HwState st = pack_state(s, lf, line_dithers(s, f));
    const Region clip = clip_region(s);
    const int dx = b.x - a.x, dy = b.y - a.y;
    const int n = std::max(std::abs(dx), std::abs(dy));
    auto lerp = [n](int p0, int p1, int i) -> int {
        if (n == 0) return p0;
        return p0 + static_cast<int>(floor_div(static_cast<int64_t>(p1 - p0) * i * 2 + n, 2 * static_cast<int64_t>(n)));
    };
    int count = 0;
    for (int i = 0; i <= n; ++i) {
        const int x = lerp(a.x, b.x, i), y = lerp(a.y, b.y, i);
        if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1) continue;
        uint8_t r = a.r, g = a.g, bl = a.b;
        if (f.gouraud) {
            auto ch = [&](int c0, int c1) { return static_cast<uint8_t>(std::clamp(lerp(c0, c1, i), 0, 255)); };
            r = ch(a.r, b.r);
            g = ch(a.g, b.g);
            bl = ch(a.b, b.b);
        }
        const float fx = static_cast<float>(x), fy = static_cast<float>(y);
        const Planes pl = flat_colour(r, g, bl);
        const HwVertex tl = make_vertex(st, fx, fy, pl, 0, 0, 0), tr = make_vertex(st, fx + 1, fy, pl, 0, 0, 0),
                       bl_ = make_vertex(st, fx, fy + 1, pl, 0, 0, 0), br = make_vertex(st, fx + 1, fy + 1, pl, 0, 0, 0);
        out.insert(out.end(), {tl, tr, bl_, tr, br, bl_});
        ++count;
    }
    return count;
}

Region triangle_bounds(const DrawState& s, const GpuVertex v[3]) {
    return clipped(s, std::min({v[0].x, v[1].x, v[2].x}), std::min({v[0].y, v[1].y, v[2].y}),
                   std::max({v[0].x, v[1].x, v[2].x}) + 1, std::max({v[0].y, v[1].y, v[2].y}) + 1);
}

Region rect_bounds(const DrawState& s, const GpuVertex& v, int width, int height) {
    return clipped(s, v.x, v.y, v.x + width, v.y + height);
}

Region line_bounds(const DrawState& s, const GpuVertex& a, const GpuVertex& b) {
    return clipped(s, std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x) + 1, std::max(a.y, b.y) + 1);
}

Region texture_region(const DrawState& s, int umin, int umax, int vmin, int vmax) {
    if (s.win_mask_x || s.win_mask_y) {
        umin = vmin = 0;
        umax = vmax = 255;
    }
    umin = std::clamp(umin, 0, 255);
    umax = std::clamp(umax, 0, 255);
    vmin = std::clamp(vmin, 0, 255);
    vmax = std::clamp(vmax, 0, 255);
    const int div = s.depth == TexDepth::Bit4 ? 4 : s.depth == TexDepth::Bit8 ? 2 : 1;
    VramRect r;
    r.x = s.tex_base_x + umin / div;
    r.w = umax / div - umin / div + 1;
    r.y = s.tex_base_y + vmin;
    r.h = vmax - vmin + 1;
    return region_from(r);
}

Region clut_region(const DrawState& s, const PrimitiveFlags& f) {
    if (s.depth == TexDepth::Bit15) return {};
    VramRect r;
    r.x = (f.clut & 0x3F) * 16;
    r.y = (f.clut >> 6) & 0x1FF;
    r.w = s.depth == TexDepth::Bit4 ? 16 : 256;
    r.h = 1;
    return region_from(r);
}

Region triangle_texture_region(const DrawState& s, const GpuVertex v[3]) {
    return texture_region(s, std::min({v[0].u, v[1].u, v[2].u}), std::max({v[0].u, v[1].u, v[2].u}),
                          std::min({v[0].v, v[1].v, v[2].v}), std::max({v[0].v, v[1].v, v[2].v}));
}

Region rect_texture_region(const DrawState& s, const GpuVertex& v, int width, int height) {
    // Texel coordinates wrap at 256; a span that wraps touches the whole page.
    auto span = [](int start, int len, bool flip, int& lo, int& hi) {
        const int end = flip ? start - (len - 1) : start + (len - 1);
        lo = std::min(start, end);
        hi = std::max(start, end);
        if (lo < 0 || hi > 255) {
            lo = 0;
            hi = 255;
        }
    };
    int u0, u1, v0, v1;
    span(v.u, width, s.rect_flip_x, u0, u1);
    span(v.v, height, s.rect_flip_y, v0, v1);
    return texture_region(s, u0, u1, v0, v1);
}

}  // namespace dw3
