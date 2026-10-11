// Bridges SDL windows to the GPU renderer, which draws through a Vulkan surface that SDL creates.
#pragma once

#include <string>
#include <vector>

#include "dw3/hw_renderer.h"

struct SDL_Window;

namespace dw3app {

// Describes `window` (created with SDL_WINDOW_VULKAN) for HwRenderer and lists the instance extensions SDL needs
// for its surface.
dw3::NativeWindow describe_window(SDL_Window* window);
std::vector<std::string> required_instance_extensions();

}  // namespace dw3app
