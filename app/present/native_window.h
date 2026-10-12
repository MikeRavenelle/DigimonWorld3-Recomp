// Bridges SDL windows to the GPU renderer, which draws through a Vulkan surface that SDL creates (or, on Windows, straight to the window handle).
#pragma once

#include <string>
#include <vector>

#include "dw3/hw_renderer.h"

struct SDL_Window;

namespace dw3app {

// Describes `window` for HwRenderer. Outside Windows it must have been created with SDL_WINDOW_VULKAN; the second
// function lists the Vulkan instance extensions SDL needs for its surface (none on Windows).
dw3::NativeWindow describe_window(SDL_Window* window);
std::vector<std::string> required_instance_extensions();

}  // namespace dw3app
