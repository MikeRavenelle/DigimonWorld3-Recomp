// DW3Recomp entry point. Currently opens a window and presents frames; the recompiled game
// loop is wired in once the recompiler emits code (see game tasks).
//
// Audit flags (used for automated live testing):
//   --frames N         exit after N frames
//   --screenshot PATH  save a BMP of the last frame before exiting

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "dw3/platform.h"

int main(int argc, char** argv) {
    int max_frames = -1;
    std::string screenshot;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) max_frames = std::atoi(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
    }

    SDL_Log("DW3Recomp starting (os=%s arch=%s)", dw3::platform::os_name(dw3::platform::host_os()).data(),
            dw3::platform::arch_name(dw3::platform::host_arch()).data());

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    // Temporary presentation path until the plume renderer lands.
    if (!SDL_CreateWindowAndRenderer("Digimon World 3 Recomp", 1280, 960, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("window creation failed: %s", SDL_GetError());
        return 1;
    }

    bool running = true;
    int frame = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) running = false;

        SDL_SetRenderDrawColor(renderer, 16, 24, 48, 255);
        SDL_RenderClear(renderer);

        if (max_frames >= 0 && frame + 1 >= max_frames) {
            if (!screenshot.empty()) {
                if (SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr)) {
                    SDL_SaveBMP(s, screenshot.c_str());
                    SDL_DestroySurface(s);
                    SDL_Log("saved screenshot %s", screenshot.c_str());
                }
            }
            running = false;
        }
        SDL_RenderPresent(renderer);
        ++frame;
    }
    SDL_Log("exiting after %d frames", frame);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
