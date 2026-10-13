#include "dw3/texrep/window.h"

#include <algorithm>

namespace dw3::texrep {

Image crop_clamped(const Image& src, int x, int y, int w, int h) {
    Image out;
    out.w = w;
    out.h = h;
    out.rgba.resize(static_cast<std::size_t>(w) * h);
    for (int j = 0; j < h; ++j) {
        const int sy = std::clamp(y + j, 0, src.h - 1);
        for (int i = 0; i < w; ++i) {
            const int sx = std::clamp(x + i, 0, src.w - 1);
            out.rgba[static_cast<std::size_t>(j) * w + i] = src.rgba[static_cast<std::size_t>(sy) * src.w + sx];
        }
    }
    return out;
}

void paste(Image& dst, const Image& src, int x, int y) {
    for (int j = 0; j < src.h; ++j) {
        const int dy = y + j;
        if (dy < 0 || dy >= dst.h) continue;
        for (int i = 0; i < src.w; ++i) {
            const int dx = x + i;
            if (dx < 0 || dx >= dst.w) continue;
            dst.rgba[static_cast<std::size_t>(dy) * dst.w + dx] = src.rgba[static_cast<std::size_t>(j) * src.w + i];
        }
    }
}

Image context_window(const Image& component, int bx, int by, const Image& block, int pad) {
    Image win = crop_clamped(component, bx - pad, by - pad, block.w + 2 * pad, block.h + 2 * pad);
    paste(win, block, pad, pad);
    return win;
}

Image crop_block(const Image& upscaled, int pad, int scale, int block_w, int block_h) {
    return crop_clamped(upscaled, pad * scale, pad * scale, block_w * scale, block_h * scale);
}

}  // namespace dw3::texrep
