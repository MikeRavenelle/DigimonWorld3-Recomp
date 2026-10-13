// PS1 GPU command front end. Command formats, GPUSTAT bits and transfer rules follow psx-spx
// "GPU Render/Memory/Environment Commands", "GPU Display Control Commands (GP1)" and "GPU Status".

#include "dw3/gpu.h"

#include <algorithm>
#include <cstdlib>

namespace dw3 {

namespace {

int32_t sext11(uint32_t v) { return static_cast<int32_t>(v << 21) >> 21; }

uint16_t rgb24_to_15(uint32_t c) {
    return static_cast<uint16_t>(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
}

uint32_t rgb15_to_rgba(uint16_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

bool is_polyline_end(uint32_t w) { return (w & 0xF000F000u) == 0x50005000u; }

}  // namespace

Gpu::Gpu(Bus& bus, IrqController& irq, VideoTiming& video, Dma& dma)
    : bus_(bus), irq_(irq), video_(video), vram_(kVramWidth * kVramHeight, 0) {
    reset();
    bus_.map_io({kGp0, 8,
                 [this](uint32_t off, int) -> uint32_t { return off < 4 ? read_gpuread() : read_gpustat(); },
                 [this](uint32_t off, uint32_t v, int) {
                     if (off < 4) write_gp0(v);
                     else write_gp1(v);
                 }});
    dma.attach(DmaChannel::Gpu, this);
}

void Gpu::reset() {
    draw_ = DrawState{};
    display_ = DisplayState{};
    state_ = Gp0State::Command;
    fifo_.clear();
    store_active_ = false;
    dma_dir_ = 0;
    tex_disable_allowed_ = false;
    stat_ = 0x14802000u;  // display disabled, interlace field, ready bits are computed on read
}

// ---------------------------------------------------------------------------------------------
// GP0

int Gpu::expected_length(uint32_t first) const {
    uint32_t op = first >> 24;
    switch (op >> 5) {
        case 1: {  // polygon
            int n = (op & 8) ? 4 : 3;
            int tex = (op & 4) ? 1 : 0;
            return 1 + n * (1 + tex) + ((op & 0x10) ? n - 1 : 0);
        }
        case 2: return (op & 0x10) ? 4 : 3;  // line (poly-lines continue in PolyLine state)
        case 3: return 2 + ((op & 4) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);  // rectangle
        case 4: return 4;                                                      // VRAM->VRAM
        case 5: return 3;                                                      // CPU->VRAM
        case 6: return 3;                                                      // VRAM->CPU
        default: return op == 0x02 ? 3 : 1;
    }
}

void Gpu::write_gp0(uint32_t w) {
    if (state_ == Gp0State::ImageLoad) {
        image_load_word(w);
        return;
    }
    if (state_ == Gp0State::PolyLine) {
        if (is_polyline_end(w)) {
            state_ = Gp0State::Command;
            return;
        }
        if (line_flags_.gouraud && line_expect_color_) {
            line_color_ = w;
            line_expect_color_ = false;
            return;
        }
        // Flat poly-lines reuse the first colour; gouraud ones take the colour word preceding each vertex.
        GpuVertex nv = make_vertex(w);
        nv.r = static_cast<uint8_t>(line_color_);
        nv.g = static_cast<uint8_t>(line_color_ >> 8);
        nv.b = static_cast<uint8_t>(line_color_ >> 16);
        draw_line(line_last_, nv, line_flags_);
        line_last_ = nv;
        line_expect_color_ = true;
        return;
    }
    fifo_.push_back(w);
    if (fifo_.size() == 1) needed_ = expected_length(w);
    if (static_cast<int>(fifo_.size()) >= needed_) {
        std::vector<uint32_t> cmd;
        cmd.swap(fifo_);
        execute(cmd);
    }
}

void Gpu::execute(std::span<const uint32_t> c) {
    ++commands_;
    uint32_t op = c[0] >> 24;
    switch (op >> 5) {
        case 1: draw_polygon(c); return;
        case 2: {
            PrimitiveFlags f;
            f.gouraud = (op & 0x10) != 0;
            f.semi_transparent = (op & 2) != 0;
            GpuVertex a = make_vertex(c[1]);
            a.r = static_cast<uint8_t>(c[0]);
            a.g = static_cast<uint8_t>(c[0] >> 8);
            a.b = static_cast<uint8_t>(c[0] >> 16);
            uint32_t col_b = f.gouraud ? c[2] : c[0];
            GpuVertex b = make_vertex(f.gouraud ? c[3] : c[2]);
            b.r = static_cast<uint8_t>(col_b);
            b.g = static_cast<uint8_t>(col_b >> 8);
            b.b = static_cast<uint8_t>(col_b >> 16);
            draw_line(a, b, f);
            if (op & 8) {  // poly-line: keep consuming vertices until the terminator
                state_ = Gp0State::PolyLine;
                line_flags_ = f;
                line_last_ = b;
                line_color_ = col_b;
                line_expect_color_ = f.gouraud;
            }
            return;
        }
        case 3: draw_rect(c); return;
        case 4: copy_vram(c); return;
        case 5: begin_image_load(c); return;
        case 6: begin_image_store(c); return;
        case 7: execute_env(c[0]); return;
        default: break;
    }
    switch (op) {
        case 0x00: return;                              // nop
        case 0x01: return;                              // clear texture cache
        case 0x02: fill_rect(c); return;
        case 0x1F: stat_ |= 1u << 24; irq_.raise(IrqSource::Gpu); return;  // GPU IRQ request
        default:
            if (on_unknown_command) on_unknown_command("GP0", c[0]);
            return;
    }
}

void Gpu::apply_texpage(uint32_t a) {
    draw_.tex_base_x = static_cast<int>(a & 0xF) * 64;
    draw_.tex_base_y = static_cast<int>((a >> 4) & 1) * 256;
    draw_.semi_mode = static_cast<uint8_t>((a >> 5) & 3);
    uint32_t depth = (a >> 7) & 3;
    draw_.depth = depth == 0 ? TexDepth::Bit4 : depth == 1 ? TexDepth::Bit8 : TexDepth::Bit15;
    stat_ = (stat_ & ~0x1FFu) | (a & 0x1FF);
    draw_.texture_disable = tex_disable_allowed_ && ((a >> 11) & 1);
    stat_ = (stat_ & ~(1u << 15)) | (draw_.texture_disable ? 1u << 15 : 0);
}

void Gpu::execute_env(uint32_t w) {
    switch (w >> 24) {
        case 0xE1:
            apply_texpage(w);
            draw_.dither = (w >> 9) & 1;
            draw_.draw_to_display = (w >> 10) & 1;
            stat_ = (stat_ & ~0x7FFu) | (w & 0x7FF);
            draw_.rect_flip_x = (w >> 12) & 1;
            draw_.rect_flip_y = (w >> 13) & 1;
            return;
        case 0xE2:
            draw_.win_mask_x = w & 0x1F;
            draw_.win_mask_y = (w >> 5) & 0x1F;
            draw_.win_off_x = (w >> 10) & 0x1F;
            draw_.win_off_y = (w >> 15) & 0x1F;
            return;
        case 0xE3:
            draw_.area_x1 = static_cast<int>(w & 0x3FF);
            draw_.area_y1 = static_cast<int>((w >> 10) & 0x1FF);
            return;
        case 0xE4:
            draw_.area_x2 = static_cast<int>(w & 0x3FF);
            draw_.area_y2 = static_cast<int>((w >> 10) & 0x1FF);
            return;
        case 0xE5:
            draw_.off_x = sext11(w & 0x7FF);
            draw_.off_y = sext11((w >> 11) & 0x7FF);
            return;
        case 0xE6:
            draw_.set_mask = w & 1;
            draw_.check_mask = (w >> 1) & 1;
            stat_ = (stat_ & ~(3u << 11)) | ((w & 3) << 11);
            return;
        default:
            if (on_unknown_command) on_unknown_command("GP0", w);
    }
}

GpuVertex Gpu::make_vertex(uint32_t xy) const {
    GpuVertex v;
    v.x = sext11(xy & 0x7FF) + draw_.off_x;
    v.y = sext11((xy >> 16) & 0x7FF) + draw_.off_y;
    PreciseVertex p;
    if (precise_ && precise_->find(xy, p)) {
        v.fx = p.x + static_cast<float>(draw_.off_x);
        v.fy = p.y + static_cast<float>(draw_.off_y);
        v.w = p.w;
        v.precise = true;
    }
    return v;
}

void Gpu::draw_polygon(std::span<const uint32_t> c) {
    uint32_t op = c[0] >> 24;
    int n = (op & 8) ? 4 : 3;
    PrimitiveFlags f;
    f.textured = (op & 4) != 0;
    f.gouraud = (op & 0x10) != 0;
    f.semi_transparent = (op & 2) != 0;
    f.raw_texture = f.textured && (op & 1);
    GpuVertex v[4];
    std::size_t p = 1;
    for (int i = 0; i < n; ++i) {
        uint32_t col = (i > 0 && f.gouraud) ? c[p++] : c[0];
        v[i] = make_vertex(c[p++]);
        v[i].r = static_cast<uint8_t>(col);
        v[i].g = static_cast<uint8_t>(col >> 8);
        v[i].b = static_cast<uint8_t>(col >> 16);
        if (f.textured) {
            uint32_t uv = c[p++];
            v[i].u = static_cast<uint8_t>(uv);
            v[i].v = static_cast<uint8_t>(uv >> 8);
            if (i == 0) f.clut = static_cast<uint16_t>(uv >> 16);
            if (i == 1) apply_texpage(uv >> 16);
        }
    }
    auto tri = [&](const GpuVertex& a, const GpuVertex& b, const GpuVertex& d) {
        // Hardware skips polygons spanning more than 1023x511 pixels.
        int minx = std::min({a.x, b.x, d.x}), maxx = std::max({a.x, b.x, d.x});
        int miny = std::min({a.y, b.y, d.y}), maxy = std::max({a.y, b.y, d.y});
        if (maxx - minx > 1023 || maxy - miny > 511) return;
        ++primitives_;
        if (raster_) {
            GpuVertex t[3] = {a, b, d};
            raster_->triangle(vram_, draw_, f, t);
        }
    };
    tri(v[0], v[1], v[2]);
    if (n == 4) tri(v[1], v[2], v[3]);
}

void Gpu::draw_rect(std::span<const uint32_t> c) {
    uint32_t op = c[0] >> 24;
    PrimitiveFlags f;
    f.textured = (op & 4) != 0;
    f.semi_transparent = (op & 2) != 0;
    f.raw_texture = f.textured && (op & 1);
    GpuVertex v = make_vertex(c[1]);
    v.r = static_cast<uint8_t>(c[0]);
    v.g = static_cast<uint8_t>(c[0] >> 8);
    v.b = static_cast<uint8_t>(c[0] >> 16);
    std::size_t p = 2;
    if (f.textured) {
        uint32_t uv = c[p++];
        v.u = static_cast<uint8_t>(uv);
        v.v = static_cast<uint8_t>(uv >> 8);
        f.clut = static_cast<uint16_t>(uv >> 16);
    }
    int w = 0, h = 0;
    switch ((op >> 3) & 3) {
        case 0: w = static_cast<int>(c[p] & 0x3FF); h = static_cast<int>((c[p] >> 16) & 0x1FF); break;
        case 1: w = h = 1; break;
        case 2: w = h = 8; break;
        default: w = h = 16; break;
    }
    if (w == 0 || h == 0) return;
    ++primitives_;
    if (raster_) raster_->rect(vram_, draw_, f, v, w, h);
}

void Gpu::draw_line(const GpuVertex& a, const GpuVertex& b, const PrimitiveFlags& f) {
    if (std::abs(a.x - b.x) > 1023 || std::abs(a.y - b.y) > 511) return;
    ++primitives_;
    if (raster_) raster_->line(vram_, draw_, f, a, b);
}

void Gpu::store_pixel(int x, int y, uint16_t c) {
    uint16_t& d = vram_[static_cast<std::size_t>((y & 511) * kVramWidth + (x & 1023))];
    if (draw_.check_mask && (d & 0x8000)) return;
    d = static_cast<uint16_t>(c | (draw_.set_mask ? 0x8000 : 0));
}

void Gpu::fill_rect(std::span<const uint32_t> c) {
    // Ignores drawing area and mask; x/width in 16-pixel units, wraps around VRAM.
    uint16_t col = rgb24_to_15(c[0]);
    int x0 = static_cast<int>(c[1] & 0x3F0), y0 = static_cast<int>((c[1] >> 16) & 0x1FF);
    int w = static_cast<int>(((c[2] & 0x3FF) + 0xF) & ~0xFu), h = static_cast<int>((c[2] >> 16) & 0x1FF);
    if (w <= 0 || h <= 0) return;
    if (raster_ && raster_->vram_fill({x0, y0, std::min(w, kVramWidth), std::min(h, kVramHeight)}, col)) return;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            vram_[static_cast<std::size_t>(((y0 + y) & 511) * kVramWidth + ((x0 + x) & 1023))] = col;
    if (raster_) raster_->vram_written(vram_, {x0, y0, std::min(w, kVramWidth), std::min(h, kVramHeight)});
}

void Gpu::copy_vram(std::span<const uint32_t> c) {
    int sx = static_cast<int>(c[1] & 0x3FF), sy = static_cast<int>((c[1] >> 16) & 0x1FF);
    int dx = static_cast<int>(c[2] & 0x3FF), dy = static_cast<int>((c[2] >> 16) & 0x1FF);
    int w = static_cast<int>(((c[3] & 0xFFFF) - 1) & 0x3FF) + 1;
    int h = static_cast<int>((((c[3] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    if (raster_ && raster_->vram_copy({sx, sy, w, h}, dx, dy, draw_.check_mask, draw_.set_mask)) return;
    if (raster_) {
        raster_->vram_read_begin(vram_, {sx, sy, w, h});
        if (draw_.check_mask) raster_->vram_read_begin(vram_, {dx, dy, w, h});
    }
    std::vector<uint16_t> row(static_cast<std::size_t>(w));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            row[static_cast<std::size_t>(x)] =
                vram_[static_cast<std::size_t>(((sy + y) & 511) * kVramWidth + ((sx + x) & 1023))];
        for (int x = 0; x < w; ++x) store_pixel(dx + x, dy + y, row[static_cast<std::size_t>(x)]);
    }
    if (raster_) raster_->vram_written(vram_, {dx, dy, w, h});
}

void Gpu::begin_image_load(std::span<const uint32_t> c) {
    xfer_x = static_cast<int>(c[1] & 0x3FF);
    xfer_y = static_cast<int>((c[1] >> 16) & 0x1FF);
    xfer_w = static_cast<int>(((c[2] & 0xFFFF) - 1) & 0x3FF) + 1;
    xfer_h = static_cast<int>((((c[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    xfer_cx = xfer_cy = 0;
    state_ = Gp0State::ImageLoad;
    // The CPU array must match the renderer's copy over the target area: a transfer that is cut short, or
    // one that honours the mask bit, leaves part of it unchanged.
    if (raster_) raster_->vram_read_begin(vram_, {xfer_x, xfer_y, xfer_w, xfer_h});
}

void Gpu::image_load_word(uint32_t w) {
    for (int half = 0; half < 2 && state_ == Gp0State::ImageLoad; ++half) {
        uint16_t px = static_cast<uint16_t>(half ? w >> 16 : w);
        store_pixel(xfer_x + xfer_cx, xfer_y + xfer_cy, px);
        if (++xfer_cx == xfer_w) {
            xfer_cx = 0;
            if (++xfer_cy == xfer_h) {
                state_ = Gp0State::Command;
                if (raster_) raster_->vram_written(vram_, {xfer_x, xfer_y, xfer_w, xfer_h});
            }
        }
    }
}

void Gpu::begin_image_store(std::span<const uint32_t> c) {
    xfer_x = static_cast<int>(c[1] & 0x3FF);
    xfer_y = static_cast<int>((c[1] >> 16) & 0x1FF);
    xfer_w = static_cast<int>(((c[2] & 0xFFFF) - 1) & 0x3FF) + 1;
    xfer_h = static_cast<int>((((c[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
    xfer_cx = xfer_cy = 0;
    store_active_ = true;
    if (raster_) raster_->vram_read_begin(vram_, {xfer_x, xfer_y, xfer_w, xfer_h});
}

uint32_t Gpu::read_gpuread() {
    if (!store_active_) return gpuread_;
    uint32_t out = 0;
    for (int half = 0; half < 2 && store_active_; ++half) {
        uint16_t px = vram_[static_cast<std::size_t>(((xfer_y + xfer_cy) & 511) * kVramWidth + ((xfer_x + xfer_cx) & 1023))];
        out |= static_cast<uint32_t>(px) << (16 * half);
        if (++xfer_cx == xfer_w) {
            xfer_cx = 0;
            if (++xfer_cy == xfer_h) store_active_ = false;
        }
    }
    gpuread_ = out;
    return out;
}

// ---------------------------------------------------------------------------------------------
// GP1 / status

void Gpu::update_video_mode() {
    video_.set_standard(display_.pal ? VideoStandard::Pal : VideoStandard::Ntsc);
    video_.set_interlaced(display_.interlaced);
}

void Gpu::write_gp1(uint32_t w) {
    uint32_t op = (w >> 24) & 0x3F;
    switch (op) {
        case 0x00:
            reset();
            break;
        case 0x01:
            fifo_.clear();
            if (state_ == Gp0State::ImageLoad && raster_) raster_->vram_written(vram_, {xfer_x, xfer_y, xfer_w, xfer_h});
            state_ = Gp0State::Command;
            break;
        case 0x02: stat_ &= ~(1u << 24); break;
        case 0x03:
            display_.enabled = !(w & 1);
            stat_ = (stat_ & ~(1u << 23)) | ((w & 1) << 23);
            break;
        case 0x04:
            dma_dir_ = static_cast<uint8_t>(w & 3);
            stat_ = (stat_ & ~(3u << 29)) | ((w & 3) << 29);
            break;
        case 0x05:
            display_.vram_x = static_cast<int>(w & 0x3FE);
            display_.vram_y = static_cast<int>((w >> 10) & 0x1FF);
            if (raster_) raster_->display_flipped();
            break;
        case 0x06:
            display_.h_start = static_cast<int>(w & 0xFFF);
            display_.h_end = static_cast<int>((w >> 12) & 0xFFF);
            break;
        case 0x07:
            display_.v_start = static_cast<int>(w & 0x3FF);
            display_.v_end = static_cast<int>((w >> 10) & 0x3FF);
            break;
        case 0x08: {
            uint32_t hres1 = w & 3, vres = (w >> 2) & 1, hres2 = (w >> 6) & 1;
            display_.pal = (w >> 3) & 1;
            display_.color24 = (w >> 4) & 1;
            display_.interlaced = (w >> 5) & 1;
            static constexpr int widths[4] = {256, 320, 512, 640};
            static constexpr int dividers[4] = {10, 8, 5, 4};
            display_.width = hres2 ? 368 : widths[hres1];
            display_.height = (vres && display_.interlaced) ? 480 : 240;
            if (display_.pal) display_.height = (vres && display_.interlaced) ? 512 : 256;
            stat_ = (stat_ & ~(0x7Fu << 16)) | ((w & 0x3F) << 17) | (hres2 << 16);
            stat_ = (stat_ & ~(1u << 14)) | (((w >> 7) & 1) << 14);
            video_.set_dot_divider(hres2 ? 7 : dividers[hres1]);
            update_video_mode();
            break;
        }
        case 0x09: tex_disable_allowed_ = w & 1; break;  // allow texpage bit 11 (texture disable)
        default:
            if (op >= 0x10 && op <= 0x1F) {  // GPU info -> GPUREAD
                switch (w & 7) {
                    case 2:
                        gpuread_ = draw_.win_mask_x | (draw_.win_mask_y << 5) | (draw_.win_off_x << 10) |
                                   (draw_.win_off_y << 15);
                        break;
                    case 3: gpuread_ = static_cast<uint32_t>(draw_.area_x1 | (draw_.area_y1 << 10)); break;
                    case 4: gpuread_ = static_cast<uint32_t>(draw_.area_x2 | (draw_.area_y2 << 10)); break;
                    case 5:
                        gpuread_ = (static_cast<uint32_t>(draw_.off_x) & 0x7FF) |
                                   ((static_cast<uint32_t>(draw_.off_y) & 0x7FF) << 11);
                        break;
                    case 7: gpuread_ = 2; break;  // GPU version
                    default: break;
                }
            } else if (on_unknown_command) {
                on_unknown_command("GP1", w);
            }
    }
}

uint32_t Gpu::read_gpustat() const {
    uint32_t s = stat_ & ~((1u << 25) | (7u << 26) | (1u << 31));
    bool ready_cmd = state_ == Gp0State::Command && fifo_.empty();
    bool ready_send = store_active_;
    bool ready_dma = state_ != Gp0State::PolyLine;
    s |= (ready_cmd ? 1u : 0u) << 26;
    s |= (ready_send ? 1u : 0u) << 27;
    s |= (ready_dma ? 1u : 0u) << 28;
    switch (dma_dir_) {
        case 1: s |= 1u << 25; break;
        case 2: s |= (ready_dma ? 1u : 0u) << 25; break;
        case 3: s |= (ready_send ? 1u : 0u) << 25; break;
        default: break;
    }
    // Bit 31 alternates with the scanline outside vblank (games poll it in interlaced modes).
    int64_t now = bus_.ctx().cycles;
    if (!video_.in_vblank(now) && (video_.scanline(now) & 1)) s |= 1u << 31;
    return s;
}

// ---------------------------------------------------------------------------------------------
// DMA / output

void Gpu::dma_write(std::span<const uint32_t> words) {
    for (uint32_t w : words) write_gp0(w);
}

void Gpu::dma_read(std::span<uint32_t> out) {
    for (auto& w : out) w = read_gpuread();
}

bool Gpu::snapshot_display(std::vector<uint32_t>& rgba, int& width, int& height) {
    width = display_.width;
    height = display_.height;
    rgba.assign(static_cast<std::size_t>(width * height), 0xFF000000u);
    if (!display_.enabled) return false;
    if (raster_) {
        const int bytes_w = display_.color24 ? (display_.width * 3 + 1) / 2 : display_.width;
        raster_->vram_read_begin(vram_, {display_.vram_x, display_.vram_y, bytes_w, height});
    }
    for (int y = 0; y < height; ++y) {
        int vy = (display_.vram_y + y) & 511;
        for (int x = 0; x < width; ++x) {
            uint32_t px;
            if (display_.color24) {
                // 24-bit: 3 bytes per pixel packed in the 16-bit VRAM words.
                int byte = (display_.vram_x * 2) + x * 3;
                auto rd = [&](int b) {
                    uint16_t wv = vram_[static_cast<std::size_t>(vy * kVramWidth + ((b >> 1) & 1023))];
                    return static_cast<uint32_t>((b & 1) ? wv >> 8 : wv & 0xFF);
                };
                px = rd(byte) | (rd(byte + 1) << 8) | (rd(byte + 2) << 16) | 0xFF000000u;
            } else {
                px = rgb15_to_rgba(vram_[static_cast<std::size_t>(vy * kVramWidth + ((display_.vram_x + x) & 1023))]);
            }
            rgba[static_cast<std::size_t>(y * width + x)] = px;
        }
    }
    return true;
}

void Gpu::snapshot_vram(std::vector<uint32_t>& rgba) {
    if (raster_) raster_->vram_read_begin(vram_, {});
    rgba.resize(vram_.size());
    for (std::size_t i = 0; i < vram_.size(); ++i) rgba[i] = rgb15_to_rgba(vram_[i]);
}

namespace {

void io_draw_state(StateIo& s, DrawState& d) {
    s.io(d.tex_base_x);
    s.io(d.tex_base_y);
    s.io(d.semi_mode);
    s.io(d.depth);
    s.io(d.dither);
    s.io(d.draw_to_display);
    s.io(d.texture_disable);
    s.io(d.rect_flip_x);
    s.io(d.rect_flip_y);
    s.io(d.win_mask_x);
    s.io(d.win_mask_y);
    s.io(d.win_off_x);
    s.io(d.win_off_y);
    s.io(d.area_x1);
    s.io(d.area_y1);
    s.io(d.area_x2);
    s.io(d.area_y2);
    s.io(d.off_x);
    s.io(d.off_y);
    s.io(d.set_mask);
    s.io(d.check_mask);
}

void io_display_state(StateIo& s, DisplayState& d) {
    s.io(d.enabled);
    s.io(d.color24);
    s.io(d.interlaced);
    s.io(d.pal);
    s.io(d.vram_x);
    s.io(d.vram_y);
    s.io(d.h_start);
    s.io(d.h_end);
    s.io(d.v_start);
    s.io(d.v_end);
    s.io(d.width);
    s.io(d.height);
}

void io_vertex(StateIo& s, GpuVertex& v) {
    s.io(v.x);
    s.io(v.y);
    s.io(v.r);
    s.io(v.g);
    s.io(v.b);
    s.io(v.u);
    s.io(v.v);
    s.io(v.fx);
    s.io(v.fy);
    s.io(v.w);
    s.io(v.precise);
}

}  // namespace

void Gpu::io(StateIo& s) {
    StateIo::Section sec(s, "GPU0");
    if (!s.loading() && raster_) raster_->vram_read_begin(vram_, VramRect{});
    s.io(vram_);
    if (vram_.size() != static_cast<std::size_t>(kVramWidth) * kVramHeight) throw StateError("GPU state: bad VRAM size");
    io_draw_state(s, draw_);
    io_display_state(s, display_);
    s.io(state_);
    s.io(fifo_);
    s.io(needed_);
    s.io(xfer_x);
    s.io(xfer_y);
    s.io(xfer_w);
    s.io(xfer_h);
    s.io(xfer_cx);
    s.io(xfer_cy);
    s.io(store_active_);
    s.io(line_flags_.textured);
    s.io(line_flags_.raw_texture);
    s.io(line_flags_.semi_transparent);
    s.io(line_flags_.gouraud);
    s.io(line_flags_.clut);
    io_vertex(s, line_last_);
    s.io(line_color_);
    s.io(line_expect_color_);
    s.io(gpuread_);
    s.io(stat_);
    s.io(dma_dir_);
    s.io(tex_disable_allowed_);
    s.io(commands_);
    s.io(primitives_);
    if (s.loading() && raster_) raster_->vram_written(vram_, VramRect{});
}

}  // namespace dw3
