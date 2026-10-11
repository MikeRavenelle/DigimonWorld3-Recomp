#pragma once

// GPU renderer built on the plume render hardware interface (Vulkan backend). It implements the same Rasterizer
// interface as the software rasterizer and keeps the 1024x512 VRAM as a GPU texture, so primitives are drawn on
// the GPU and the CLUT/texture lookups, blending, dithering and mask handling run in the fragment shader.
//
// VRAM coherency. The Gpu front end owns the CPU-side VRAM array and does transfers on it directly; the
// renderer's texture is the authority for everything drawn since the last read-back.
//   * vram_written(): the CPU changed a region; it is uploaded immediately (after queued draws, preserving order).
//   * vram_read_begin(): the CPU is about to read a region; if the GPU drew into it, the dirty regions are read
//     back first.
//   * Draws that sample VRAM or read the destination (semi-transparency, mask check) first make sure the second,
//     sampled copy of VRAM is current for the areas they touch.
// Output is bit-exact with the software rasterizer except where noted in the implementation (rasterization of
// triangle edges and attribute interpolation use the GPU's rules).

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dw3/gpu.h"
#include "dw3/present_layout.h"

namespace dw3 {

// A window to present to, described by callbacks so that the windowing library supplies the Vulkan surface.
struct NativeWindow {
    void* user = nullptr;
    // Creates a VkSurfaceKHR for the given VkInstance and returns its handle (0 on failure).
    uint64_t (*create_surface)(void* user, void* vk_instance) = nullptr;
    // Current drawable size in pixels.
    void (*get_size)(void* user, uint32_t* width, uint32_t* height) = nullptr;
};

struct HwStats {
    uint64_t draw_calls = 0, vertices = 0, uploads = 0, readbacks = 0, copy_syncs = 0, submits = 0;
};

class HwRenderer final : public Rasterizer {
public:
    // Creates the device. With a window, also creates a swap chain for it; `instance_extensions` are the Vulkan
    // instance extensions its surface needs (empty for a headless device). Returns null and sets `error` if no
    // suitable device or swap chain is available.
    static std::unique_ptr<HwRenderer> create(const NativeWindow* window, const std::vector<std::string>& instance_extensions,
                                              std::string& error);
    ~HwRenderer() override;
    HwRenderer(const HwRenderer&) = delete;
    HwRenderer& operator=(const HwRenderer&) = delete;

    void triangle(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex v[3]) override;
    void rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
              int height) override;
    void line(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a,
              const GpuVertex& b) override;
    void vram_read_begin(std::span<uint16_t> vram, const VramRect& r) override;
    void vram_written(std::span<const uint16_t> vram, const VramRect& r) override;

    // Shows the display area in the window and ends the frame. When `capture` is given the window image is also
    // read back into it (RGBA8, `capture_w` x `capture_h`). Returns false if nothing could be presented.
    bool present(const DisplayState& display, ScaleMode mode, bool linear, std::vector<uint32_t>* capture = nullptr,
                 int* capture_w = nullptr, int* capture_h = nullptr);
    void set_vsync(bool enabled);

    // Reads the whole GPU-side VRAM into `out` (1024x512 words). For tests and diagnostics.
    void read_vram(std::span<uint16_t> out);

    const HwStats& stats() const;
    std::string device_name() const;

private:
    HwRenderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dw3
