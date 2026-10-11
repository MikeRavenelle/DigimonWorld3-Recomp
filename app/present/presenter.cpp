#include "presenter.h"

#include <SDL3/SDL.h>

#include "hw_presenter.h"
#include "software_presenter.h"

namespace dw3app {

void Presenter::notify(std::string text) { osd_.show(std::move(text), SDL_GetTicks()); }

std::unique_ptr<Presenter> create_presenter(const char* title, int width, int height, RendererChoice choice,
                                            const PresentOptions& options, void (*log)(const std::string&),
                                            SDL_Window*& window) {
    std::string error;
    window = nullptr;
    if (choice != RendererChoice::Software) {
        window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
        if (window) {
            if (auto gpu = HwPresenter::create(window, options, error)) return gpu;
            SDL_DestroyWindow(window);
            window = nullptr;
        } else {
            error = std::string("no Vulkan-capable window: ") + SDL_GetError();
        }
        log("GPU renderer unavailable: " + error);
        if (choice == RendererChoice::Gpu) return nullptr;
        log("falling back to the software presentation path");
    }
    window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
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
