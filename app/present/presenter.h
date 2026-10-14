// Host-side presentation of the emulated display, once per VBLANK. Implementations: SoftwarePresenter (the
// CPU rasterizer's VRAM shown through an SDL renderer) and HwPresenter (the plume GPU renderer).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dw3/gpu.h"
#include "dw3/postfx.h"
#include "dw3/present_layout.h"
#include "dw3/texrep/pack.h"
#include "dw3/texrep/stage_library.h"
#include "dw3/widescreen.h"
#include "osd.h"

struct SDL_Window;

namespace dw3app {

enum class VsyncMode { Off, On, Adaptive };

struct PresentOptions {
    dw3::ScaleMode scale = dw3::ScaleMode::Fit;
    bool linear = false;  // linear filtering instead of nearest
    VsyncMode vsync = VsyncMode::On;
    int internal_res = 1;  // GPU renderer internal resolution multiplier (1..8)
    bool true_color = false;  // GPU renderer: no dithering, full precision shading
    int texture_filter = 0;   // GPU renderer: 0 nearest, 1 bilinear, 2 xBR
    bool precise_geometry = false;  // GPU renderer: sub-pixel 3D vertices from the GTE
    dw3::WideAspect widescreen = dw3::WideAspect::Off;  // GPU renderer: wider battles (see dw3/widescreen.h)
    dw3::PostFx post;               // GPU renderer: post-processing (the software path ignores it)
    std::shared_ptr<const dw3::texrep::TexturePack> texture_pack;  // GPU renderer: replacement backgrounds (null: off)
    std::shared_ptr<dw3::texrep::StageLibrary> stage_library;      // GPU renderer: map pieces for field backgrounds (null: none)
    bool seamless_bg = false;                                      // ... also inside the 4:3 area, not only in widescreen margins
};

// Window contents as seen by the user (borders included), RGBA8888.
struct Capture {
    std::vector<uint32_t> rgba;
    int w = 0, h = 0;
};

class Presenter {
public:
    virtual ~Presenter() = default;
    virtual const char* name() const = 0;
    virtual void set_vsync(VsyncMode mode) = 0;
    // Whether this path can output HDR (the display must be in HDR mode as well, see window_hdr_enabled).
    virtual bool supports_hdr() const { return false; }
    // Whether VsyncMode::Adaptive does anything beyond On on this path.
    virtual bool supports_adaptive_vsync() const { return false; }
    // Applies changed display options to the running presenter: scaling, filtering, vsync and, on the GPU renderer,
    // internal resolution, true colour, texture filter and precise geometry (the render targets are rebuilt and the
    // current VRAM image carried over). Returns false if the renderer could not take the new configuration; it
    // then keeps the old one. Must be called from the thread that presents.
    virtual bool update_options(const PresentOptions& options, dw3::Gpu& gpu) = 0;
    // Turns the wide picture on while the game is in a mode that has one (battles). Ignored without the GPU renderer
    // or a widescreen option.
    virtual void set_wide_mode(dw3::WideMode) {}
    // Starts recording the background images the game draws into `directory` (see dw3::HwRenderer::set_texture_dump).
    // Returns false if this presenter cannot (the software path).
    virtual bool set_texture_dump(const std::string& directory) { return false; }
    virtual bool write_texture_dump(std::string& error) { return true; }
    // Draws and presents one frame. When capture is non-null it receives the window image of this frame.
    virtual bool present(dw3::Gpu& gpu, Capture* capture) = 0;
    // Called once with the machine's GPU before the game starts, so a presenter can install its rasterizer.
    virtual void attach(dw3::Gpu&) {}
    // Shows a short message over the top-left of the window for a couple of seconds. It is drawn after the
    // emulated picture and is not part of captures.
    void notify(std::string text);
    // A small line of text kept in the top-right corner (the frame rate); empty removes it. Not part of captures.
    void set_status(std::string text);

protected:
    Osd osd_;
};

enum class RendererChoice { Auto, Gpu, Vulkan, D3D12, Metal, Software };

// Creates the window and the requested presenter for it. Auto tries the GPU renderer on each graphics API the
// platform offers (Direct3D 12, then Vulkan, on Windows; Metal on macOS; Vulkan elsewhere) and falls back to a plain
// window with the software path; Gpu does the same without the fallback; Vulkan, D3D12 and Metal try only that API. `log` receives
// diagnostics. Returns null if no path works.
std::unique_ptr<Presenter> create_presenter(const char* title, int width, int height, RendererChoice choice,
                                            const PresentOptions& options, void (*log)(const std::string&),
                                            SDL_Window*& window);

}  // namespace dw3app
