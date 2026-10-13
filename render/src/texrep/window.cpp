#include "dw3/texrep/window.h"

#include <algorithm>
#include <vector>

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

void bleed_transparent(Image& img, int iterations) {
    const std::size_t n = img.rgba.size();
    std::vector<uint8_t> known(n);
    uint64_t sum[3] = {0, 0, 0};
    std::size_t opaque = 0;
    for (std::size_t i = 0; i < n; ++i) {
        known[i] = (img.rgba[i] >> 24) != 0;
        if (known[i]) {
            ++opaque;
            for (int c = 0; c < 3; ++c) sum[c] += (img.rgba[i] >> (8 * c)) & 255;
        }
    }
    for (int it = 0; it < iterations; ++it) {
        std::vector<uint32_t> next = img.rgba;
        std::vector<uint8_t> next_known = known;
        bool changed = false;
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * img.w + x;
                if (known[i]) continue;
                int acc[3] = {0, 0, 0}, count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= img.w || ny >= img.h) continue;
                        const std::size_t j = static_cast<std::size_t>(ny) * img.w + nx;
                        if (!known[j]) continue;
                        for (int c = 0; c < 3; ++c) acc[c] += static_cast<int>((img.rgba[j] >> (8 * c)) & 255);
                        ++count;
                    }
                if (!count) continue;
                uint32_t p = 0xFF000000u;
                for (int c = 0; c < 3; ++c) p |= static_cast<uint32_t>((acc[c] + count / 2) / count) << (8 * c);
                next[i] = p;
                next_known[i] = 1;
                changed = true;
            }
        img.rgba = std::move(next);
        known = std::move(next_known);
        if (!changed) break;
    }
    uint32_t fill = 0xFF808080u;
    if (opaque) {
        fill = 0xFF000000u;
        for (int c = 0; c < 3; ++c) fill |= static_cast<uint32_t>(sum[c] / opaque) << (8 * c);
    }
    for (std::size_t i = 0; i < n; ++i) img.rgba[i] = known[i] ? (img.rgba[i] | 0xFF000000u) : fill;
}

Image crop_block(const Image& upscaled, int pad, int scale, int block_w, int block_h) {
    return crop_clamped(upscaled, pad * scale, pad * scale, block_w * scale, block_h * scale);
}

}  // namespace dw3::texrep
