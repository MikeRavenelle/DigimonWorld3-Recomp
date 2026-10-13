#pragma once

// A rasterizer wrapper that watches what the game uploads and draws the way the GPU renderer's texture replacement
// does, and records the (image, palette) key of every background-style sprite draw. It exists so that tests can
// compare the keys of a real play session with the keys computed from the game files, using the very same block
// registry and hash functions as the renderer. Drawing itself is passed on to the wrapped rasterizer.

#include <set>
#include <span>
#include <utility>

#include "dw3/gpu.h"
#include "dw3/texrep/blocks.h"

namespace dw3 {

class UploadRecorder final : public Rasterizer {
public:
    using Key = std::pair<uint64_t, uint64_t>;  // block hash, palette hash

    explicit UploadRecorder(Rasterizer* inner) : inner_(inner) {}

    const std::set<Key>& keys() const { return keys_; }

    void vram_read_begin(std::span<uint16_t> vram, const VramRect& r) override;
    void vram_written(std::span<const uint16_t> vram, const VramRect& r) override;
    bool vram_fill(const VramRect& r, uint16_t color) override;
    bool vram_copy(const VramRect& src, int dx, int dy, bool check_mask, bool set_mask) override;
    void triangle(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3]) override;
    void rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width, int height) override;
    void line(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a, const GpuVertex& b) override;

private:
    Rasterizer* inner_;
    texrep::BlockRegistry blocks_;
    std::set<Key> keys_;
};

}  // namespace dw3
