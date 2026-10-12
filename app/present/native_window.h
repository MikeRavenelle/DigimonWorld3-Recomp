// Bridges SDL windows to the GPU renderer, which draws through a Vulkan surface that SDL creates (on Windows straight to
// the window handle, on macOS into the window's CAMetalLayer).
#pragma once

#include <string>
#include <vector>

#include "dw3/hw_renderer.h"

struct SDL_Window;

namespace dw3app {

// Describes `window` for HwRenderer. On Linux it must have been created with SDL_WINDOW_VULKAN, on macOS with
// SDL_WINDOW_METAL. The second function lists the Vulkan instance extensions SDL needs for its surface (none on
// Windows and macOS). release_window frees what describe_window created (the Metal view on macOS) and must be called
// before the window is destroyed.
dw3::NativeWindow describe_window(SDL_Window* window);
void release_window(dw3::NativeWindow& native);
std::vector<std::string> required_instance_extensions();

}  // namespace dw3app
