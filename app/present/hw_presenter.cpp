#include "hw_presenter.h"

#include <SDL3/SDL.h>

#include "dw3/hw_renderer.h"
#include "native_window.h"

namespace dw3app {

HwPresenter::HwPresenter() = default;
HwPresenter::~HwPresenter() {
    renderer_.reset();
    release_window(native_);
}

std::unique_ptr<HwPresenter> HwPresenter::create(SDL_Window* window, dw3::GraphicsApi api, const PresentOptions& options, std::string& error) {
#if DW3_HW_RENDERER
    std::unique_ptr<HwPresenter> p(new HwPresenter());
    p->native_ = describe_window(window);
    const dw3::NativeWindow& native = p->native_;
    p->window_ = window;
    p->options_ = options;
    dw3::HwOptions hw;
    hw.api = api;
    hw.true_color = options.true_color;
    hw.filter = static_cast<dw3::TextureFilter>(options.texture_filter);
    hw.precise_geometry = options.precise_geometry;
    p->renderer_ = dw3::HwRenderer::create(&native, required_instance_extensions(), error, options.internal_res, hw);
    if (!p->renderer_) return nullptr;
    p->name_ = api == dw3::GraphicsApi::D3D12 ? "GPU (plume/Direct3D 12)"
               : api == dw3::GraphicsApi::Metal ? "GPU (plume/Metal)"
                                                : "GPU (plume/Vulkan)";
    p->renderer_->set_vsync(options.vsync);
    SDL_Log("GPU renderer: %s, device %s", p->name_.c_str(), p->renderer_->device_name().c_str());
    return p;
#else
    (void)window;
    (void)api;
    (void)options;
    error = "built without the GPU renderer";
    return nullptr;
#endif
}

void HwPresenter::set_vsync(bool on) {
    options_.vsync = on;
    renderer_->set_vsync(on);
}

bool HwPresenter::update_options(const PresentOptions& options, dw3::Gpu& gpu) {
    bool ok = true;
    if (options.internal_res != options_.internal_res || options.true_color != options_.true_color ||
        options.texture_filter != options_.texture_filter || options.precise_geometry != options_.precise_geometry) {
        dw3::HwOptions hw;
        hw.true_color = options.true_color;
        hw.filter = static_cast<dw3::TextureFilter>(options.texture_filter);
        hw.precise_geometry = options.precise_geometry;
        std::string error;
        if (renderer_->reconfigure(options.internal_res, hw, gpu.vram(), error)) {
            options_.internal_res = renderer_->internal_scale();
            options_.true_color = options.true_color;
            options_.texture_filter = options.texture_filter;
            options_.precise_geometry = options.precise_geometry;
        } else {
            SDL_Log("renderer reconfiguration failed: %s", error.c_str());
            ok = false;
        }
    }
    options_.scale = options.scale;
    options_.linear = options.linear;
    options_.post = options.post;
    if (options.vsync != options_.vsync) set_vsync(options.vsync);
    return ok;
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
                                       img ? &overlay : nullptr, &options_.post);
    if (capture) {
        capture->w = w;
        capture->h = h;
    }
    return ok;
}

}  // namespace dw3app
