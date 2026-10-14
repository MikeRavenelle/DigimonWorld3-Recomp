#include "presenter.h"

#include <SDL3/SDL.h>

#include <vector>

#include "hw_presenter.h"
#include "software_presenter.h"

namespace dw3app {

void Presenter::notify(std::string text) { osd_.show(std::move(text), SDL_GetTicks()); }

void Presenter::set_status(std::string text) { osd_.set_status(std::move(text)); }

std::unique_ptr<Presenter> create_presenter(const char* title, int width, int height, RendererChoice choice,
                                            const PresentOptions& options, void (*log)(const std::string&),
                                            SDL_Window*& window) {
    std::string error;
    window = nullptr;
    std::vector<dw3::GraphicsApi> apis;
    const bool software_only = choice == RendererChoice::Software;
    if (choice == RendererChoice::Vulkan) apis = {dw3::GraphicsApi::Vulkan};
    else if (choice == RendererChoice::D3D12) apis = {dw3::GraphicsApi::D3D12};
    else if (choice == RendererChoice::Metal) apis = {dw3::GraphicsApi::Metal};
    else if (!software_only) {
#if defined(_WIN32)
        apis = {dw3::GraphicsApi::D3D12, dw3::GraphicsApi::Vulkan};
#elif defined(__APPLE__)
        apis = {dw3::GraphicsApi::Metal};
#else
        apis = {dw3::GraphicsApi::Vulkan};
#endif
    }
    for (const dw3::GraphicsApi api : apis) {
        // A window that has had a swap chain of one API cannot get one of another, so every attempt has its own.
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
#if defined(__APPLE__)
        flags |= SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#elif !defined(_WIN32)
        flags |= SDL_WINDOW_VULKAN;
#endif
        window = SDL_CreateWindow(title, width, height, flags);
        if (window) {
            if (auto gpu = HwPresenter::create(window, api, options, error)) return gpu;
            SDL_DestroyWindow(window);
            window = nullptr;
        } else {
            error = std::string("window creation failed: ") + SDL_GetError();
        }
        const char* api_name = api == dw3::GraphicsApi::D3D12 ? "Direct3D 12" : api == dw3::GraphicsApi::Metal ? "Metal" : "Vulkan";
        log(std::string(api_name) + " renderer unavailable: " + error);
    }
    if (choice != RendererChoice::Auto && choice != RendererChoice::Software) return nullptr;
    if (!software_only) log("falling back to the software presentation path");
    SDL_WindowFlags software_flags = SDL_WINDOW_RESIZABLE;
#if defined(__APPLE__)
    software_flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    window = SDL_CreateWindow(title, width, height, software_flags);
    if (!window) {
        log(std::string("window creation failed: ") + SDL_GetError());
        return nullptr;
    }
    auto sw = SoftwarePresenter::create(window, options, error);
    if (!sw) {
        log(error);
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    return sw;
}

}  // namespace dw3app
