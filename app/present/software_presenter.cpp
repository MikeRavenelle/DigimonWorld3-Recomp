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
    p->set_vsync(options.vsync);
    return p;
}

SoftwarePresenter::~SoftwarePresenter() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
}

void SoftwarePresenter::set_vsync(bool on) {
    options_.vsync = on;
    SDL_SetRenderVSync(renderer_, on ? 1 : 0);
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
    SDL_RenderPresent(renderer_);
    return true;
}

}  // namespace dw3app
