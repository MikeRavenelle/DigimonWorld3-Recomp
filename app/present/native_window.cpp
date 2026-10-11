#include "native_window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace dw3app {

namespace {

uint64_t create_surface(void* user, void* instance) {
    VkSurfaceKHR surface = 0;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(user), static_cast<VkInstance>(instance), nullptr, &surface)) {
        SDL_Log("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return 0;
    }
    return reinterpret_cast<uint64_t>(surface);
}

void window_size(void* user, uint32_t* w, uint32_t* h) {
    int iw = 0, ih = 0;
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(user), &iw, &ih);
    *w = static_cast<uint32_t>(iw);
    *h = static_cast<uint32_t>(ih);
}

}  // namespace

dw3::NativeWindow describe_window(SDL_Window* window) {
    dw3::NativeWindow w;
    w.user = window;
    w.create_surface = create_surface;
    w.get_size = window_size;
    return w;
}

std::vector<std::string> required_instance_extensions() {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    std::vector<std::string> out;
    for (Uint32 i = 0; names && i < count; ++i) out.emplace_back(names[i]);
    return out;
}

}  // namespace dw3app
