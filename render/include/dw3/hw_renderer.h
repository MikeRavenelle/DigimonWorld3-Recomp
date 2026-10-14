#pragma once

// GPU renderer built on the plume render hardware interface (Vulkan, Direct3D 12 and Metal backends). It implements the same Rasterizer
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
// Texture replacement. Uploads of background images are tracked as blocks (texrep/blocks.h). A sprite that samples
// 8-bit texels one to one and lies inside a block whose content and palette hashes are in the active texture pack
// is split along block edges and drawn with state_bits::kHd: in the display layer its texel colour is fetched from
// an atlas of replacement images at (texel position * pack scale), then modulated by the vertex colour as usual.
// The exact PS1 target still receives the original texels, so what the game reads back never changes and the
// replacement only exists where the display layer is shown. With no pack nothing is tracked and the output is
// byte-identical to a build without this feature.
// Seamless field backgrounds. With HwOptions::stage_library the renderer follows the field backgrounds the game draws
// (texrep/field_backdrop.h): it finds the map and the scroll from the sprites, and draws the pieces of the map that the
// game has not drawn, as "direct" sprites that read their texels from the replacement atlas (state_bits::kDirect, the
// original picture of the cell or the pack's image of it), and hides the squares the game draws over unloaded cells. The
// game's own sprites are never changed, so a frame without missing pieces is the game's frame bit for bit. Implies
// the display layer.
// HDR output. With HwOptions::hdr the swap chain is created as R16G16B16A16_FLOAT in the scRGB colour space where the
// backend can (plume patch: Vulkan VK_EXT_swapchain_colorspace extended sRGB, Direct3D 12 DXGI G10_NONE_P709).
// The final post-processing pass then turns the SDR picture into scRGB (render/shaders/post/hdr.glsl, hdr.h):
// SDR white lands on the paper white level and only the brightest part is lifted towards the peak.
// Wide frame copy. With HwOptions::wide_frame the targets get extra columns to the right of VRAM. While mirroring is
// on, every primitive and fill that lands in a frame buffer is drawn a second time, shifted into those columns with
// the drawing area widened (see widescreen.h for how edge-touching 2D shapes are stretched); the copy is never read
// back. 3D geometry is clipped by the game's 320 px drawing area only, so the copy shows what lies beyond it.
// Fields (WideMode::Field) need HwOptions::stage_library: the margins are filled with the pieces of the map's background
// for the widened view, the game's own draws follow them, and a frame whose map is not resolved is shown at 4:3.
// Precise geometry. With HwOptions::precise_geometry, triangles that have sub-pixel vertex positions are drawn from
// interpolated float attributes instead of the fixed-point planes (see build_precise_triangle); everything else
// is unchanged. At scale 1 the triangle covers the same pixels as the integer one.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dw3/gpu.h"
#include "dw3/postfx.h"
#include "dw3/present_layout.h"
#include "dw3/texrep/pack.h"
#include "dw3/texrep/stage_library.h"
#include "dw3/widescreen.h"

namespace dw3 {

// A window to present to. On Windows the native handle is enough; on macOS the NSWindow and its CAMetalLayer; elsewhere
// the windowing library supplies the Vulkan surface through callbacks.
struct NativeWindow {
    void* hwnd = nullptr;  // Windows: the HWND (used by both the Direct3D 12 and the Vulkan backend)
    void* cocoa_window = nullptr;  // macOS: the NSWindow
    void* metal_layer = nullptr;   // macOS: the CAMetalLayer to draw into
    void* user = nullptr;
    // Creates a VkSurfaceKHR for the given VkInstance and returns its handle (0 on failure).
    uint64_t (*create_surface)(void* user, void* vk_instance) = nullptr;
    // Current drawable size in pixels.
    void (*get_size)(void* user, uint32_t* width, uint32_t* height) = nullptr;
};

enum class TextureFilter { Nearest, Bilinear, Xbr };

// The graphics API the renderer runs on. Direct3D 12 exists on Windows builds only, Metal on macOS builds only.
enum class GraphicsApi { Vulkan, D3D12, Metal };

// Display options that add precision or smoothing on top of the exact PS1 output. The defaults leave the output
// untouched.
struct HwOptions {
    GraphicsApi api = GraphicsApi::Vulkan;
    bool true_color = false;                           // no dithering, full precision shading and blending
    TextureFilter filter = TextureFilter::Nearest;     // bilinear for triangles, xBR for sprites (internal res > 1)
    // Triangles whose vertices carry GTE sub-pixel positions (GpuVertex::precise) are drawn on a float path:
    // positions at sub-pixel precision when the internal resolution is above 1, and perspective-correct texel
    // coordinates. Other triangles keep the exact fixed-point path.
    bool precise_geometry = false;
    // Replacement images for background blocks (texrep/blocks.h). When set, the display layer shows them in place of
    // the matching original images; the exact picture underneath is unchanged. Implies the display layer.
    std::shared_ptr<const texrep::TexturePack> texture_pack;
    // Load replacement images on the calling thread the moment a draw wants them, instead of decoding them on worker
    // threads while the original picture is drawn. For tests and tools that need the same picture on every run; a
    // running game stutters with it.
    bool synchronous_replacements = false;
    // Seamless field backgrounds (texrep/field_backdrop.h): the maps of the game, as indexed from the disc. When set, the
    // renderer draws the pieces of a field map's background that the game has not drawn yet. Implies the display layer.
    std::shared_ptr<texrep::StageLibrary> stage_library;
    // Whether the pieces are also drawn inside the 4:3 area. Without it a library only fills the margins of the wide
    // field picture (and nothing outside the wide copy).
    bool seamless_inside = true;
    // Extra render-target columns that hold a wider copy of the frame buffers (widescreen.h). Margin 0: none.
    WideFrame wide_frame;
    // Ask for an HDR swap chain (scRGB: linear Rec. 709 half floats, 1.0 = 80 nits) where the window system and the
    // display offer one. Whether it was obtained is hdr_active(); without it the picture stays SDR.
    bool hdr = false;
};

// A small RGBA8 image (red in the lowest byte, straight alpha) blended over the window at (x, y) in window pixels.
// `serial` changes whenever the pixels do, so the upload is skipped for an unchanged image.
struct HwOverlay {
    const uint32_t* rgba = nullptr;
    int w = 0, h = 0;
    int x = 0, y = 0;
    uint64_t serial = 0;
    float opacity = 1.0f;
};

struct HwStats {
    // Seamless field backgrounds: pieces drawn by the renderer, and the opaque 64x64 squares the game draws over cells it
    // has not loaded, hidden or let through.
    uint64_t backdrop_pieces = 0, squares_hidden = 0, squares_drawn = 0;
    // Wide field mode: finished frames shown wide (the map was resolved) and shown at 4:3 instead.
    uint64_t wide_field_frames = 0, wide_field_fallbacks = 0;
    // Textured polygons (the game's characters and objects) that were drawn into the margins of the wide copy.
    uint64_t wide_margin_polygons = 0;
    uint64_t draw_calls = 0, vertices = 0, uploads = 0, readbacks = 0, copy_syncs = 0, submits = 0, fills = 0, copies = 0;
    // Host time spent (monotonic, nanoseconds) waiting for the GPU, submitting command lists, and loading replacement
    // images (PNG decode, atlas upload recording), with the number of images loaded.
    uint64_t gpu_wait_ns = 0, submit_ns = 0, replacement_ns = 0, replacement_loads = 0;
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
    void display_flipped() override;

    // Shows the display area in the window and ends the frame. When `capture` is given the window image is also
    // read back into it (RGBA8, `capture_w` x `capture_h`). Returns false if nothing could be presented.
    // `overlay` is blended over the window image after the picture; it is not part of `capture`.
    // `post` selects the post-processing passes (postfx.h); null, or options with everything off, takes the plain
    // path, which scales with `linear` and leaves the picture untouched. With post-processing the scaler in `post`
    // replaces `linear`.
    bool present(const DisplayState& display, ScaleMode mode, bool linear, std::vector<uint32_t>* capture = nullptr,
                 int* capture_w = nullptr, int* capture_h = nullptr, const HwOverlay* overlay = nullptr,
                 const PostFx* post = nullptr);
    // Draws the display area the way present() would into an offscreen image of width x height pixels and reads it back
    // (RGBA8, rows top to bottom). Works without a window. For tests and diagnostics.
    bool render_to_image(const DisplayState& display, ScaleMode mode, bool linear, const PostFx* post, int width, int height,
                         std::vector<uint32_t>& rgba);
    void set_vsync(bool enabled);
    // True when the swap chain presents scRGB. Post-processing then ends in the HDR output transform of PostFx::hdr
    // (scaled to the nits it carries), and window captures are not available.
    bool hdr_active() const;
    // Starts or stops duplicating frame-buffer draws into the wide copy (needs HwOptions::wide_frame). While it
    // runs, present() shows the copy, at its own aspect, whenever the displayed buffer has one.
    void set_wide_mirror(bool active) { set_wide_mode(active ? WideMode::Battle : WideMode::Off); }
    void set_wide_mode(WideMode mode);
    // Blocks until every replacement image that was queued has been decoded and moved into the atlas (they normally
    // arrive a few per frame). For tests and tools.
    void wait_for_replacements();

    // Starts recording the background images the game draws (empty directory stops it). write_texture_dump() saves
    // what was seen so far (see texrep/dump.h); the pack generator takes it from there.
    void set_texture_dump(const std::string& directory);
    bool write_texture_dump(std::string& error);

    // Changes the internal resolution and the display options of a running renderer. `vram` is the machine's VRAM
    // (1024x512 words): it is refreshed from the GPU side first and uploaded again afterwards, so the picture
    // carries over at native detail until the game redraws it. Returns false and sets `error` if the new
    // configuration could not be created; the previous one is then restored. The `api` field is ignored.
    bool reconfigure(int internal_scale, const HwOptions& options, std::span<uint16_t> vram, std::string& error);

    // Reads the whole GPU-side VRAM into `out` (1024x512 words). For tests and diagnostics.
    void read_vram(std::span<uint16_t> out);

    // Reads a native-pixel region of the scaled target as it is on the GPU, (w*N) x (h*N) words row by row. For
    // tests and diagnostics; returns false if the region is too large for the read-back buffer.
    bool read_scaled(const VramRect& r, std::vector<uint16_t>& out);

    // Reads the colour that is shown for a native-pixel region of the scaled target (display layer where valid,
    // else the exact colour), (w*N) x (h*N) pixels as RGBA8. For tests and diagnostics.
    bool read_display(const VramRect& r, std::vector<uint32_t>& rgba);

    // True if this build contains the backend for `api`.
    static bool api_compiled_in(GraphicsApi api);

    int internal_scale() const;
    const HwStats& stats() const;
    std::string device_name() const;

private:
    HwRenderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dw3
