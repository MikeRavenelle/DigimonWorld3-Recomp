#include "native_window.h"

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#elif !defined(_WIN32)
#include <SDL3/SDL_vulkan.h>
#endif

namespace dw3app {

#ifdef _WIN32

// On Windows both backends present straight to the window handle.
dw3::NativeWindow describe_window(SDL_Window* window) {
    dw3::NativeWindow w;
    w.user = window;
    w.hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    return w;
}

void release_window(dw3::NativeWindow&) {}

std::vector<std::string> required_instance_extensions() { return {}; }

#elif defined(__APPLE__)

// Metal draws into the window's CAMetalLayer, which SDL provides through a view it creates for the window.
dw3::NativeWindow describe_window(SDL_Window* window) {
    dw3::NativeWindow w;
    w.cocoa_window = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    SDL_MetalView view = SDL_Metal_CreateView(window);
    w.user = view;
    w.metal_layer = view ? SDL_Metal_GetLayer(view) : nullptr;
    return w;
}

void release_window(dw3::NativeWindow& native) {
    if (native.user) SDL_Metal_DestroyView(static_cast<SDL_MetalView>(native.user));
    native.user = nullptr;
    native.metal_layer = nullptr;
}

std::vector<std::string> required_instance_extensions() { return {}; }

#else

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

void release_window(dw3::NativeWindow&) {}

std::vector<std::string> required_instance_extensions() {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    std::vector<std::string> out;
    for (Uint32 i = 0; names && i < count; ++i) out.emplace_back(names[i]);
    return out;
}

#endif

}  // namespace dw3app
