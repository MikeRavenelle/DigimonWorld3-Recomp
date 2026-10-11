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
    p->options_ = options;
    p->renderer_ = dw3::HwRenderer::create(&native, required_instance_extensions(), error);
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
    const bool ok = renderer_->present(gpu.display(), options_.scale, options_.linear, capture ? &capture->rgba : nullptr, &w, &h);
    if (capture) {
        capture->w = w;
        capture->h = h;
    }
    return ok;
}

}  // namespace dw3app
