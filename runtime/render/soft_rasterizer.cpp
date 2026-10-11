// PS1 GPU software reference rasterizer. See soft_rasterizer.h for references.

#include "dw3/soft_rasterizer.h"

#include <algorithm>
#include <cstdlib>

namespace dw3 {

namespace {

constexpr int kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

int64_t floor_div(int64_t a, int64_t b) {  // b > 0
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}
int64_t ceil_div(int64_t a, int64_t b) { return -floor_div(-a, b); }

// Per-primitive pixel pipeline state.
struct Ctx {
    uint16_t* vram = nullptr;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;  // inclusive clip, already limited to VRAM
    bool textured = false, raw = false, semi = false, dither = false, check = false, set = false;
    uint8_t mode = 0;
    TexDepth depth = TexDepth::Bit4;
    int base_x = 0, base_y = 0;
    uint32_t win_and_u = 0xFF, win_or_u = 0, win_and_v = 0xFF, win_or_v = 0;
    uint16_t clut[256];
};

void init_ctx(Ctx& c, std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, bool dither) {
    c.vram = vram.data();
    c.x1 = std::max(s.area_x1, 0);
    c.y1 = std::max(s.area_y1, 0);
    c.x2 = std::min(s.area_x2, kVramWidth - 1);
    c.y2 = std::min(s.area_y2, kVramHeight - 1);
    c.textured = f.textured && !s.texture_disable;
    c.raw = f.raw_texture;
    c.semi = f.semi_transparent;
    c.dither = dither;
    c.check = s.check_mask;
    c.set = s.set_mask;
    c.mode = s.semi_mode & 3;
    if (!c.textured) return;
    c.depth = s.depth;
    c.base_x = s.tex_base_x;
    c.base_y = s.tex_base_y;
    c.win_and_u = ~(static_cast<uint32_t>(s.win_mask_x) * 8) & 0xFF;
    c.win_or_u = (static_cast<uint32_t>(s.win_off_x & s.win_mask_x) * 8) & 0xFF;
    c.win_and_v = ~(static_cast<uint32_t>(s.win_mask_y) * 8) & 0xFF;
    c.win_or_v = (static_cast<uint32_t>(s.win_off_y & s.win_mask_y) * 8) & 0xFF;
    if (c.depth != TexDepth::Bit15) {
        int n = c.depth == TexDepth::Bit4 ? 16 : 256;
        int cx = (f.clut & 0x3F) * 16, cy = (f.clut >> 6) & 0x1FF;
        for (int i = 0; i < n; ++i) c.clut[i] = c.vram[cy * kVramWidth + ((cx + i) & (kVramWidth - 1))];
    }
}

inline uint16_t fetch(const Ctx& c, uint32_t u, uint32_t v) {
    u = (u & c.win_and_u) | c.win_or_u;
    v = (v & c.win_and_v) | c.win_or_v;
    const uint16_t* row = c.vram + ((c.base_y + static_cast<int>(v)) & (kVramHeight - 1)) * kVramWidth;
    switch (c.depth) {
        case TexDepth::Bit4: {
            uint16_t w = row[(c.base_x + static_cast<int>(u >> 2)) & (kVramWidth - 1)];
            return c.clut[(w >> ((u & 3) * 4)) & 0xF];
        }
        case TexDepth::Bit8: {
            uint16_t w = row[(c.base_x + static_cast<int>(u >> 1)) & (kVramWidth - 1)];
            return c.clut[(w >> ((u & 1) * 8)) & 0xFF];
        }
        default: return row[(c.base_x + static_cast<int>(u)) & (kVramWidth - 1)];
    }
}

inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Writes one pixel: 8-bit colour -> optional dither -> 5 bit -> optional blend -> mask handling.
inline void plot(const Ctx& c, int x, int y, int r, int g, int b, bool blend, uint16_t bit15) {
    uint16_t& d = c.vram[y * kVramWidth + x];
    if (c.check && (d & 0x8000)) return;
    if (c.dither) {
        int o = kDither[y & 3][x & 3];
        r = clamp8(r + o);
        g = clamp8(g + o);
        b = clamp8(b + o);
    }
    r >>= 3;
    g >>= 3;
    b >>= 3;
    if (blend) {
        int br = d & 31, bg = (d >> 5) & 31, bb = (d >> 10) & 31;
        switch (c.mode) {
            case 0: r = (br + r) >> 1; g = (bg + g) >> 1; b = (bb + b) >> 1; break;
            case 1: r = std::min(31, br + r); g = std::min(31, bg + g); b = std::min(31, bb + b); break;
            case 2: r = std::max(0, br - r); g = std::max(0, bg - g); b = std::max(0, bb - b); break;
            default: r = std::min(31, br + (r >> 2)); g = std::min(31, bg + (g >> 2)); b = std::min(31, bb + (b >> 2));
        }
    }
    d = static_cast<uint16_t>(r | (g << 5) | (b << 10) | bit15 | (c.set ? 0x8000 : 0));
}

// Shades a pixel with vertex colour (r,g,b) and texture coords (u,v) when textured.
inline void shade(const Ctx& c, int x, int y, int r, int g, int b, uint32_t u, uint32_t v) {
    if (!c.textured) {
        plot(c, x, y, r, g, b, c.semi, 0);
        return;
    }
    uint16_t t = fetch(c, u, v);
    if (t == 0) return;
    int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
    if (!c.raw) {
        tr = std::min(255, (tr * r) >> 7);
        tg = std::min(255, (tg * g) >> 7);
        tb = std::min(255, (tb * b) >> 7);
    }
    plot(c, x, y, tr, tg, tb, c.semi && (t & 0x8000), t & 0x8000);
}

}  // namespace

void SoftRasterizer::triangle(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f,
                              const GpuVertex v[3]) {
    const GpuVertex* p[3] = {&v[0], &v[1], &v[2]};
    int64_t area = static_cast<int64_t>(p[1]->x - p[0]->x) * (p[2]->y - p[0]->y) -
                   static_cast<int64_t>(p[2]->x - p[0]->x) * (p[1]->y - p[0]->y);
    if (area == 0) return;
    if (area < 0) {
        std::swap(p[1], p[2]);
        area = -area;
    }
    const bool textured = f.textured && !s.texture_disable;
    Ctx c;
    init_ctx(c, vram, s, f, s.dither && (f.gouraud || (textured && !f.raw_texture)));

    const int ymin = std::max(std::min({p[0]->y, p[1]->y, p[2]->y}), c.y1);
    const int ymax = std::min(std::max({p[0]->y, p[1]->y, p[2]->y}), c.y2);
    const int xmin = std::max(std::min({p[0]->x, p[1]->x, p[2]->x}), c.x1);
    const int xmax = std::min(std::max({p[0]->x, p[1]->x, p[2]->x}), c.x2);
    if (ymin > ymax || xmin > xmax) return;

    // Attribute planes in 20.12 fixed point: value = base + dx*(x-x0) + dy*(y-y0).
    struct Plane {
        int64_t base = 0, dx = 0, dy = 0;
    } pl[5];
    auto setup = [&](Plane& q, int a0, int a1, int a2) {
        int64_t d1 = a1 - a0, d2 = a2 - a0;
        q.base = (static_cast<int64_t>(a0) << 12) + 0x800;
        q.dx = ((d1 * (p[2]->y - p[0]->y) - d2 * (p[1]->y - p[0]->y)) * 4096) / area;
        q.dy = ((d2 * (p[1]->x - p[0]->x) - d1 * (p[2]->x - p[0]->x)) * 4096) / area;
    };
    if (f.gouraud) {
        setup(pl[0], p[0]->r, p[1]->r, p[2]->r);
        setup(pl[1], p[0]->g, p[1]->g, p[2]->g);
        setup(pl[2], p[0]->b, p[1]->b, p[2]->b);
    }
    if (textured) {
        setup(pl[3], p[0]->u, p[1]->u, p[2]->u);
        setup(pl[4], p[0]->v, p[1]->v, p[2]->v);
    }

    // Edge setup: edge i runs a->b; inside when w(x,y) = dx*(y-ay) - dy*(x-ax) >= bias (top/left edges include 0).
    struct Edge {
        int64_t ax, ay, dx, dy, bias;
    } e[3];
    for (int i = 0; i < 3; ++i) {
        const GpuVertex& a = *p[i];
        const GpuVertex& b = *p[(i + 1) % 3];
        e[i].ax = a.x;
        e[i].ay = a.y;
        e[i].dx = b.x - a.x;
        e[i].dy = b.y - a.y;
        e[i].bias = ((e[i].dy == 0 && e[i].dx > 0) || e[i].dy < 0) ? 0 : 1;
    }

    for (int y = ymin; y <= ymax; ++y) {
        int64_t lo = xmin, hi = xmax;
        for (const Edge& ed : e) {
            int64_t cst = ed.dx * (y - ed.ay) + ed.dy * ed.ax - ed.bias;  // w' = cst - dy*x >= 0
            int64_t k = -ed.dy;
            if (k == 0) {
                if (cst < 0) lo = hi + 1;
            } else if (k > 0) {
                lo = std::max(lo, ceil_div(-cst, k));
            } else {
                hi = std::min(hi, floor_div(cst, -k));
            }
        }
        if (lo > hi) continue;
        int64_t acc[5];
        for (int i = 0; i < 5; ++i)
            acc[i] = pl[i].base + pl[i].dx * (lo - p[0]->x) + pl[i].dy * (y - p[0]->y);
        for (int x = static_cast<int>(lo); x <= static_cast<int>(hi); ++x) {
            int r, g, b;
            if (f.gouraud) {
                r = clamp8(static_cast<int>(acc[0] >> 12));
                g = clamp8(static_cast<int>(acc[1] >> 12));
                b = clamp8(static_cast<int>(acc[2] >> 12));
            } else {
                r = p[0]->r;
                g = p[0]->g;
                b = p[0]->b;
            }
            shade(c, x, y, r, g, b, static_cast<uint32_t>(acc[3] >> 12) & 0xFF,
                  static_cast<uint32_t>(acc[4] >> 12) & 0xFF);
            for (int i = 0; i < 5; ++i) acc[i] += pl[i].dx;
        }
    }
}

void SoftRasterizer::rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v,
                          int width, int height) {
    Ctx c;
    init_ctx(c, vram, s, f, false);
    const int xa = std::max(v.x, c.x1), xb = std::min(v.x + width - 1, c.x2);
    const int ya = std::max(v.y, c.y1), yb = std::min(v.y + height - 1, c.y2);
    for (int y = ya; y <= yb; ++y) {
        int dy = y - v.y;
        uint32_t tv = static_cast<uint32_t>(s.rect_flip_y ? v.v - dy : v.v + dy) & 0xFF;
        for (int x = xa; x <= xb; ++x) {
            int dx = x - v.x;
            uint32_t tu = static_cast<uint32_t>(s.rect_flip_x ? v.u - dx : v.u + dx) & 0xFF;
            shade(c, x, y, v.r, v.g, v.b, tu, tv);
        }
    }
}

void SoftRasterizer::line(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a,
                          const GpuVertex& b) {
    PrimitiveFlags lf = f;
    lf.textured = false;
    Ctx c;
    init_ctx(c, vram, s, lf, s.dither && f.gouraud);
    const int dx = b.x - a.x, dy = b.y - a.y;
    const int n = std::max(std::abs(dx), std::abs(dy));
    // Both endpoints are drawn; positions and colours are interpolated with round-to-nearest.
    auto lerp = [n](int p0, int p1, int i) -> int {
        if (n == 0) return p0;
        return p0 + static_cast<int>(floor_div(static_cast<int64_t>(p1 - p0) * i * 2 + n, 2 * static_cast<int64_t>(n)));
    };
    for (int i = 0; i <= n; ++i) {
        int x = lerp(a.x, b.x, i), y = lerp(a.y, b.y, i);
        if (x < c.x1 || x > c.x2 || y < c.y1 || y > c.y2) continue;
        if (f.gouraud) plot(c, x, y, lerp(a.r, b.r, i), lerp(a.g, b.g, i), lerp(a.b, b.b, i), c.semi, 0);
        else plot(c, x, y, a.r, a.g, a.b, c.semi, 0);
    }
}

}  // namespace dw3
