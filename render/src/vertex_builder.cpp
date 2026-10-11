#include "dw3/vertex_builder.h"

#include <algorithm>
#include <cstdlib>

namespace dw3 {

namespace {

int64_t floor_div(int64_t a, int64_t b) {  // b > 0
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

HwVertex make_vertex(const HwState& st, float x, float y, uint8_t r, uint8_t g, uint8_t b, float u, float v) {
    HwVertex out;
    out.x = x;
    out.y = y;
    out.r = r;
    out.g = g;
    out.b = b;
    out.u = u;
    out.v = v;
    out.state0 = st.s0;
    out.state1 = st.s1;
    out.state2 = st.s2;
    return out;
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

void build_triangle(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3]) {
    const HwState st = pack_state(s, f, triangle_dithers(s, f));
    const bool tex = uses_texture(s, f);
    for (int i = 0; i < 3; ++i) {
        const GpuVertex& c = f.gouraud ? v[i] : v[0];
        out.push_back(make_vertex(st, static_cast<float>(v[i].x) + 0.5f, static_cast<float>(v[i].y) + 0.5f, c.r, c.g, c.b,
                                  tex ? v[i].u : 0.0f, tex ? v[i].v : 0.0f));
    }
}

void build_rect(std::vector<HwVertex>& out, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
                int height) {
    const HwState st = pack_state(s, f, false);
    const bool tex = uses_texture(s, f);
    // Texel coordinates run by +-1 per pixel; the quad spans pixel edges, so the value at the first pixel centre
    // must equal the start coordinate: offset the corner values by half a pixel.
    const float su = s.rect_flip_x ? -1.0f : 1.0f, sv = s.rect_flip_y ? -1.0f : 1.0f;
    const float u0 = static_cast<float>(v.u) - 0.5f * su, u1 = u0 + static_cast<float>(width) * su;
    const float v0 = static_cast<float>(v.v) - 0.5f * sv, v1 = v0 + static_cast<float>(height) * sv;
    const float x0 = static_cast<float>(v.x), y0 = static_cast<float>(v.y);
    const float x1 = x0 + static_cast<float>(width), y1 = y0 + static_cast<float>(height);
    auto vert = [&](float x, float y, float u, float t) {
        return make_vertex(st, x, y, v.r, v.g, v.b, tex ? u : 0.0f, tex ? t : 0.0f);
    };
    const HwVertex tl = vert(x0, y0, u0, v0), tr = vert(x1, y0, u1, v0), bl = vert(x0, y1, u0, v1),
                   br = vert(x1, y1, u1, v1);
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
        const HwVertex tl = make_vertex(st, fx, fy, r, g, bl, 0, 0), tr = make_vertex(st, fx + 1, fy, r, g, bl, 0, 0),
                       bl_ = make_vertex(st, fx, fy + 1, r, g, bl, 0, 0),
                       br = make_vertex(st, fx + 1, fy + 1, r, g, bl, 0, 0);
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
