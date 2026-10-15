#pragma once

struct SDL_Window;

namespace dw3app {

// Gives the window the project icon (taskbar, title bar and Alt-Tab on X11 and Windows; the Dock on an unbundled macOS run).
void set_window_icon(SDL_Window* window);

}  // namespace dw3app
