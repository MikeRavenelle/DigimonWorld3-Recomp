#include "dw3/upload_recorder.h"

#include "dw3/vertex_builder.h"
#include "dw3/vram_regions.h"

namespace dw3 {

void UploadRecorder::vram_read_begin(std::span<uint16_t> vram, const VramRect& r) {
    if (inner_) inner_->vram_read_begin(vram, r);
}

void UploadRecorder::vram_written(std::span<const uint16_t> vram, const VramRect& r) {
    blocks_.add_upload(region_from(r));
    if (inner_) inner_->vram_written(vram, r);
}

bool UploadRecorder::vram_fill(const VramRect& r, uint16_t color) {
    blocks_.invalidate(region_from(r));
    return inner_ && inner_->vram_fill(r, color);
}

bool UploadRecorder::vram_copy(const VramRect& src, int dx, int dy, bool check_mask, bool set_mask) {
    blocks_.invalidate(region_from({dx, dy, src.w, src.h}));
    return inner_ && inner_->vram_copy(src, dx, dy, check_mask, set_mask);
}

void UploadRecorder::triangle(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3]) {
    blocks_.invalidate(triangle_bounds(s, v));
    if (inner_) inner_->triangle(vram, s, f, v);
}

void UploadRecorder::line(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a, const GpuVertex& b) {
    blocks_.invalidate(line_bounds(s, a, b));
    if (inner_) inner_->line(vram, s, f, a, b);
}

void UploadRecorder::rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width, int height) {
    const Region bounds = rect_bounds(s, v, width, height);
    if (!bounds.empty()) {
        blocks_.invalidate(bounds);
        // The same candidates as the renderer's background path: opaque sprites reading 8-bit texels one to one.
        if (s.depth == TexDepth::Bit8 && uses_texture(s, f) && !f.semi_transparent && !s.rect_flip_x && !s.rect_flip_y &&
            s.win_mask_x == 0 && s.win_mask_y == 0) {
            const int clut_x = (f.clut & 0x3F) * 16, clut_y = (f.clut >> 6) & 0x1FF;
            bool palette_hashed = false;
            uint64_t palette = 0;
            for (const texrep::Piece& p : texrep::split_by_blocks(blocks_, s.tex_base_x, s.tex_base_y, v.u, v.v, width, height)) {
                if (!p.block) continue;
                if (!palette_hashed) {
                    palette = texrep::hash_clut(vram, clut_x, clut_y);
                    palette_hashed = true;
                }
                keys_.insert({blocks_.hash_of(*p.block, vram), palette});
            }
        }
    }
    if (inner_) inner_->rect(vram, s, f, v, width, height);
}

}  // namespace dw3
