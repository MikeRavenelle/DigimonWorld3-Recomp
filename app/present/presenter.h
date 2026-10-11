// Host-side presentation of the emulated display, once per VBLANK. Implementations: SoftwarePresenter (the
// CPU rasterizer's VRAM shown through an SDL renderer) and HwPresenter (the plume GPU renderer).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dw3/gpu.h"
#include "dw3/present_layout.h"

struct SDL_Window;

namespace dw3app {

struct PresentOptions {
    dw3::ScaleMode scale = dw3::ScaleMode::Fit;
    bool linear = false;  // linear filtering instead of nearest
    bool vsync = true;
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
    virtual void set_vsync(bool on) = 0;
    // Draws and presents one frame. When capture is non-null it receives the window image of this frame.
    virtual bool present(dw3::Gpu& gpu, Capture* capture) = 0;
    // Called once with the machine's GPU before the game starts, so a presenter can install its rasterizer.
    virtual void attach(dw3::Gpu&) {}
};

enum class RendererChoice { Auto, Gpu, Software };

// Creates the window and the requested presenter for it. The GPU path needs a Vulkan-capable window; Auto tries
// it first and falls back to a plain window with the software path. `log` receives diagnostics. Returns null if
// no path works.
std::unique_ptr<Presenter> create_presenter(const char* title, int width, int height, RendererChoice choice,
                                            const PresentOptions& options, void (*log)(const std::string&),
                                            SDL_Window*& window);

}  // namespace dw3app
