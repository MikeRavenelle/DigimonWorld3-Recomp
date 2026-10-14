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
    hw.texture_pack = options.texture_pack;
    hw.stage_library = options.stage_library;
    hw.seamless_inside = options.seamless_bg;
    hw.wide_frame = dw3::make_wide_frame(options.widescreen);
    hw.hdr = options.post.hdr.mode != dw3::HdrMode::Off;
    p->api_ = api;
    p->renderer_ = dw3::HwRenderer::create(&native, required_instance_extensions(), error, options.internal_res, hw);
    if (!p->renderer_) return nullptr;
    p->name_ = api == dw3::GraphicsApi::D3D12 ? "GPU (plume/Direct3D 12)"
               : api == dw3::GraphicsApi::Metal ? "GPU (plume/Metal)"
                                                : "GPU (plume/Vulkan)";
    p->renderer_->set_vsync(options.vsync != VsyncMode::Off);
    SDL_Log("GPU renderer: %s, device %s", p->name_.c_str(), p->renderer_->device_name().c_str());
    if (hw.hdr) SDL_Log(p->renderer_->hdr_active() ? "HDR output on" : "HDR output is not available on this display or driver; staying SDR");
    return p;
#else
    (void)window;
    (void)api;
    (void)options;
    error = "built without the GPU renderer";
    return nullptr;
#endif
}

void HwPresenter::set_vsync(VsyncMode mode) {
    options_.vsync = mode;
    renderer_->set_vsync(mode != VsyncMode::Off);
}

bool HwPresenter::update_options(const PresentOptions& options, dw3::Gpu& gpu) {
    bool ok = true;
    if (options.internal_res != options_.internal_res || options.true_color != options_.true_color ||
        options.texture_filter != options_.texture_filter || options.precise_geometry != options_.precise_geometry ||
        options.texture_pack != options_.texture_pack || options.stage_library != options_.stage_library ||
        options.widescreen != options_.widescreen || options.seamless_bg != options_.seamless_bg ||
        (options.post.hdr.mode != dw3::HdrMode::Off) != (options_.post.hdr.mode != dw3::HdrMode::Off)) {
        dw3::HwOptions hw;
        hw.true_color = options.true_color;
        hw.filter = static_cast<dw3::TextureFilter>(options.texture_filter);
        hw.precise_geometry = options.precise_geometry;
        hw.texture_pack = options.texture_pack;
        hw.stage_library = options.stage_library;
        hw.seamless_inside = options.seamless_bg;
        hw.wide_frame = dw3::make_wide_frame(options.widescreen);
        hw.hdr = options.post.hdr.mode != dw3::HdrMode::Off;
        std::string error;
        if (renderer_->reconfigure(options.internal_res, hw, gpu.vram(), error)) {
            options_.internal_res = renderer_->internal_scale();
            options_.true_color = options.true_color;
            options_.texture_filter = options.texture_filter;
            options_.precise_geometry = options.precise_geometry;
            options_.texture_pack = options.texture_pack;
            options_.stage_library = options.stage_library;
            options_.seamless_bg = options.seamless_bg;
            options_.widescreen = options.widescreen;
            options_.post.hdr = options.post.hdr;
            if (hw.hdr && !renderer_->hdr_active()) SDL_Log("HDR output is not available on this display or driver; staying SDR");
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

bool HwPresenter::supports_hdr() const { return true; }

void HwPresenter::set_wide_mode(dw3::WideMode mode) { renderer_->set_wide_mode(mode); }

void HwPresenter::attach(dw3::Gpu& gpu) { gpu.set_rasterizer(renderer_.get()); }

bool HwPresenter::set_texture_dump(const std::string& directory) {
    renderer_->set_texture_dump(directory);
    return true;
}

bool HwPresenter::write_texture_dump(std::string& error) { return renderer_->write_texture_dump(error); }

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
        overlay.x = osd_.x(ww);
        overlay.y = osd_.margin();
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
