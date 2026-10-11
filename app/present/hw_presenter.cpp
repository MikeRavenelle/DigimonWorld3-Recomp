#include "hw_presenter.h"

#include <SDL3/SDL.h>

#include "dw3/hw_renderer.h"
#include "native_window.h"

namespace dw3app {

HwPresenter::HwPresenter() = default;
HwPresenter::~HwPresenter() = default;

std::unique_ptr<HwPresenter> HwPresenter::create(SDL_Window* window, const PresentOptions& options, std::string& error) {
#if DW3_HW_RENDERER
    const dw3::NativeWindow native = describe_window(window);
    std::unique_ptr<HwPresenter> p(new HwPresenter());
    p->window_ = window;
    p->options_ = options;
    dw3::HwOptions hw;
    hw.true_color = options.true_color;
    hw.filter = static_cast<dw3::TextureFilter>(options.texture_filter);
    hw.precise_geometry = options.precise_geometry;
    p->renderer_ = dw3::HwRenderer::create(&native, required_instance_extensions(), error, options.internal_res, hw);
    if (!p->renderer_) return nullptr;
    p->renderer_->set_vsync(options.vsync);
    SDL_Log("GPU device: %s", p->renderer_->device_name().c_str());
    return p;
#else
    (void)window;
    (void)options;
    error = "built without the GPU renderer";
    return nullptr;
#endif
}

void HwPresenter::set_vsync(bool on) {
    options_.vsync = on;
    renderer_->set_vsync(on);
}

void HwPresenter::attach(dw3::Gpu& gpu) { gpu.set_rasterizer(renderer_.get()); }

bool HwPresenter::present(dw3::Gpu& gpu, Capture* capture) {
    int w = 0, h = 0;
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(window_, &ww, &wh);
    dw3::HwOverlay overlay;
    const OsdImage* img = osd_.frame(ww, wh, SDL_GetTicks(), overlay.opacity);
    if (img) {
        overlay.rgba = img->rgba.data();
        overlay.w = img->w;
        overlay.h = img->h;
        overlay.x = overlay.y = osd_.margin();
        overlay.serial = osd_.serial();
    }
    const bool ok = renderer_->present(gpu.display(), options_.scale, options_.linear, capture ? &capture->rgba : nullptr, &w, &h,
                                       img ? &overlay : nullptr);
    if (capture) {
        capture->w = w;
        capture->h = h;
    }
    return ok;
}

}  // namespace dw3app
