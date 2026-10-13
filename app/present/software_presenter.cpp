#include "software_presenter.h"

#include <SDL3/SDL.h>

#include <cstring>

namespace dw3app {

std::unique_ptr<SoftwarePresenter> SoftwarePresenter::create(SDL_Window* window, const PresentOptions& options,
                                                             std::string& error) {
    std::unique_ptr<SoftwarePresenter> p(new SoftwarePresenter());
    p->options_ = options;
    p->renderer_ = SDL_CreateRenderer(window, nullptr);
    if (!p->renderer_) {
        error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        return nullptr;
    }
    p->adaptive_vsync_ = SDL_SetRenderVSync(p->renderer_, SDL_RENDERER_VSYNC_ADAPTIVE);
    p->set_vsync(options.vsync);
    return p;
}

SoftwarePresenter::~SoftwarePresenter() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (osd_texture_) SDL_DestroyTexture(osd_texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
}

void SoftwarePresenter::set_vsync(VsyncMode mode) {
    options_.vsync = mode;
    const int interval = mode == VsyncMode::Off ? 0 : mode == VsyncMode::Adaptive && adaptive_vsync_ ? SDL_RENDERER_VSYNC_ADAPTIVE : 1;
    SDL_SetRenderVSync(renderer_, interval);
}

bool SoftwarePresenter::update_options(const PresentOptions& options, dw3::Gpu&) {
    // The CPU rasterizer has no internal resolution, true colour or texture filtering: only the output side applies.
    options_.scale = options.scale;
    options_.linear = options.linear;
    if (options.vsync != options_.vsync) set_vsync(options.vsync);
    return true;
}

// Blends the notification over the window; called after the capture is read so it never ends up in one.
void SoftwarePresenter::draw_osd() {
    int ww = 0, wh = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &ww, &wh);
    float opacity = 1.0f;
    const OsdImage* img = osd_.frame(ww, wh, SDL_GetTicks(), opacity);
    if (!img) return;
    if (!osd_texture_ || osd_serial_ != osd_.serial()) {
        if (osd_texture_) SDL_DestroyTexture(osd_texture_);
        osd_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img->w, img->h);
        if (!osd_texture_) return;
        SDL_UpdateTexture(osd_texture_, nullptr, img->rgba.data(), img->w * 4);
        SDL_SetTextureBlendMode(osd_texture_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(osd_texture_, SDL_SCALEMODE_NEAREST);
        osd_serial_ = osd_.serial();
    }
    SDL_SetTextureAlphaModFloat(osd_texture_, opacity);
    const SDL_FRect dst = {static_cast<float>(osd_.x(ww)), static_cast<float>(osd_.margin()),
                           static_cast<float>(img->w), static_cast<float>(img->h)};
    SDL_RenderTexture(renderer_, osd_texture_, nullptr, &dst);
}

bool SoftwarePresenter::present(dw3::Gpu& gpu, Capture* capture) {
    struct {
        const uint32_t* rgba = nullptr;
        int w = 0, h = 0;
    } frame;
    if (gpu.snapshot_display(pixels_, frame.w, frame.h)) frame.rgba = pixels_.data();
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (frame.rgba && frame.w > 0 && frame.h > 0) {
        if (!texture_ || frame.w != tex_w_ || frame.h != tex_h_) {
            if (texture_) SDL_DestroyTexture(texture_);
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, frame.w,
                                         frame.h);
            if (!texture_) return false;
            tex_w_ = frame.w;
            tex_h_ = frame.h;
        }
        SDL_SetTextureScaleMode(texture_, options_.linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_UpdateTexture(texture_, nullptr, frame.rgba, frame.w * 4);
        int ww = 0, wh = 0;
        SDL_GetCurrentRenderOutputSize(renderer_, &ww, &wh);
        dw3::PresentRect r = dw3::compute_present_rect(frame.w, frame.h, ww, wh, options_.scale);
        SDL_FRect dst = {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                         static_cast<float>(r.h)};
        SDL_RenderTexture(renderer_, texture_, nullptr, &dst);
    }
    if (capture) {
        capture->rgba.clear();
        capture->w = capture->h = 0;
        if (SDL_Surface* s = SDL_RenderReadPixels(renderer_, nullptr)) {
            if (SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888)) {
                capture->w = c->w;
                capture->h = c->h;
                capture->rgba.resize(static_cast<std::size_t>(c->w) * static_cast<std::size_t>(c->h));
                for (int y = 0; y < c->h; ++y)
                    std::memcpy(&capture->rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(c->w)],
                                static_cast<const uint8_t*>(c->pixels) + static_cast<std::size_t>(y) * c->pitch,
                                static_cast<std::size_t>(c->w) * 4);
                SDL_DestroySurface(c);
            }
            SDL_DestroySurface(s);
        }
    }
    draw_osd();
    SDL_RenderPresent(renderer_);
    return true;
}

}  // namespace dw3app
