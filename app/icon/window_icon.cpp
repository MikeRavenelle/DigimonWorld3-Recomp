#include "window_icon.h"

#include <SDL3/SDL.h>

#include "window_icon_data.h"

namespace dw3app {

void set_window_icon(SDL_Window* window) {
    constexpr int kSize = 128;
    static_assert(dw3::icon::window_icon_128_size == kSize * kSize * 4, "window-icon-128.rgba must be 128x128 RGBA");
    // The surface only borrows the embedded pixels; SDL copies them into the window's icon.
    SDL_Surface* surface = SDL_CreateSurfaceFrom(kSize, kSize, SDL_PIXELFORMAT_RGBA32, const_cast<uint8_t*>(dw3::icon::window_icon_128), kSize * 4);
    if (!surface) return;
    SDL_SetWindowIcon(window, surface);
    SDL_DestroySurface(surface);
}

}  // namespace dw3app
