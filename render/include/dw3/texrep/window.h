#pragma once

// Image helpers for pack generation: a block is upscaled together with the pixels around it so that the upscaler
// sees the neighbouring blocks and the seams between blocks come out continuous.

#include "dw3/texrep/pack.h"

namespace dw3::texrep {

// The w x h rectangle at (x, y) of `src`; positions outside the image repeat the nearest edge pixel.
Image crop_clamped(const Image& src, int x, int y, int w, int h);
// Copies `src` onto `dst` at (x, y), clipped to dst.
void paste(Image& dst, const Image& src, int x, int y);
// The context window of a block: the block's rectangle in `component` grown by `pad` pixels on every side, with
// `block` pasted over its own place (so the window shows that palette variant), edge pixels repeated beyond the
// component. The result is (w + 2 pad) x (h + 2 pad).
Image context_window(const Image& component, int bx, int by, const Image& block, int pad);
// The block's part of an upscaled context window: the window was upscaled by `scale`, so the block starts at
// (pad * scale, pad * scale) and measures block_w * scale by block_h * scale.
Image crop_block(const Image& upscaled, int pad, int scale, int block_w, int block_h);

}  // namespace dw3::texrep
