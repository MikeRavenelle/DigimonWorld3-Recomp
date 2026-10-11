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
//   * vram_fill()/vram_copy(): fills and VRAM-to-VRAM copies run on the GPU; the destination is marked dirty and the
//     CPU array is only refreshed if something reads it later.
//   * Draws that sample VRAM or read the destination (semi-transparency, mask check) first make sure the second,
//     sampled copy of VRAM is current for the areas they touch.
// Frames in flight. Work is recorded into one of two slots (command buffer, fence, vertex and upload buffers) and
// submitted without waiting, so the CPU runs ahead while the GPU executes the previous frame. A slot is only
// waited for when it is reused, and a read-back only waits for the submission that contains its copy.
// Internal resolution. With a scale N the draw target and the sampled copy are (1024*N) x (512*N). CPU uploads are
// replicated N x N on the GPU, read-backs keep the top-left subpixel of each native pixel (so the CPU array holds
// exactly what the game can see), and texture and CLUT data is fetched at that same top-left subpixel, so indexed
// data is never filtered or enlarged. Triangle vertices are offset by half a subpixel so the top-left subpixel
// rasterizes like the native pixel; rectangles and triangle quads on the same pixel boundaries cover identical
// subpixels, and texel selection is anchored to the native pixel, so sprites and tiles neither gap nor bleed.
// Colour and texel coordinates are evaluated from the software rasterizer's fixed-point planes, so at any scale the
// native pixels match it; only the rasterization of triangle edges follows the GPU's rules.
// Display layer. The VRAM target always holds exactly what a PS1 would (5-bit colour, dithering, mask bit), because
// the game reads it back and samples it. True colour and texture filtering do not belong in that state, so when
// either is enabled a second target (16 bits per channel) is drawn alongside it with the enhanced colour. Each of
// its pixels records the VRAM word it was produced with; presentation shows the enhanced colour only where that
// word is still the one in VRAM, so uploads, fills and copies need no mirroring and stale pixels fall back to the
// exact colour.
// Precise geometry. With HwOptions::precise_geometry, triangles that have sub-pixel vertex positions are drawn from
// interpolated float attributes instead of the fixed-point planes (see build_precise_triangle); everything else
// is unchanged. At scale 1 the triangle covers the same pixels as the integer one.

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

enum class TextureFilter { Nearest, Bilinear, Xbr };

// Display options that add precision or smoothing on top of the exact PS1 output. The defaults leave the output
// untouched.
struct HwOptions {
    bool true_color = false;                           // no dithering, full precision shading and blending
    TextureFilter filter = TextureFilter::Nearest;     // bilinear for triangles, xBR for sprites (internal res > 1)
    // Triangles whose vertices carry GTE sub-pixel positions (GpuVertex::precise) are drawn on a float path:
    // positions at sub-pixel precision when the internal resolution is above 1, and perspective-correct texel
    // coordinates. Other triangles keep the exact fixed-point path.
    bool precise_geometry = false;
};

struct HwStats {
    uint64_t draw_calls = 0, vertices = 0, uploads = 0, readbacks = 0, copy_syncs = 0, submits = 0, fills = 0, copies = 0;
};

class HwRenderer final : public Rasterizer {
public:
    // Creates the device. With a window, also creates a swap chain for it; `instance_extensions` are the Vulkan
    // instance extensions its surface needs (empty for a headless device). Returns null and sets `error` if no
    // suitable device or swap chain is available. `internal_scale` (1..8) multiplies the resolution of the VRAM
    // render target; the CPU-visible VRAM stays at native resolution.
    static std::unique_ptr<HwRenderer> create(const NativeWindow* window, const std::vector<std::string>& instance_extensions,
                                              std::string& error, int internal_scale = 1,
                                              const HwOptions& options = {});
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
    bool vram_fill(const VramRect& r, uint16_t color) override;
    bool vram_copy(const VramRect& src, int dx, int dy, bool check_mask, bool set_mask) override;

    // Shows the display area in the window and ends the frame. When `capture` is given the window image is also
    // read back into it (RGBA8, `capture_w` x `capture_h`). Returns false if nothing could be presented.
    bool present(const DisplayState& display, ScaleMode mode, bool linear, std::vector<uint32_t>* capture = nullptr,
                 int* capture_w = nullptr, int* capture_h = nullptr);
    void set_vsync(bool enabled);

    // Reads the whole GPU-side VRAM into `out` (1024x512 words). For tests and diagnostics.
    void read_vram(std::span<uint16_t> out);

    // Reads a native-pixel region of the scaled target as it is on the GPU, (w*N) x (h*N) words row by row. For
    // tests and diagnostics; returns false if the region is too large for the read-back buffer.
    bool read_scaled(const VramRect& r, std::vector<uint16_t>& out);

    // Reads the colour that is shown for a native-pixel region of the scaled target (display layer where valid,
    // else the exact colour), (w*N) x (h*N) pixels as RGBA8. For tests and diagnostics.
    bool read_display(const VramRect& r, std::vector<uint32_t>& rgba);

    int internal_scale() const;
    const HwStats& stats() const;
    std::string device_name() const;

private:
    HwRenderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dw3
