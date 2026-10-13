// The first-run setup window: Dear ImGui drawn through SDL_Renderer in a window of its own. It runs before the game
// window exists and shares nothing with the GPU renderer. Settings that matter while playing are not here; they are in
// the game's own settings screen.
#pragma once
#include <filesystem>
#include <string>

#include "wizard_state.h"

namespace dw3setup {

struct WizardLaunch {
    std::string initial_disc;          // pre-filled path, such as a stored one that no longer works
    std::filesystem::path user_data;   // where tools, downloads and texture packs go
    std::string screenshot_dir;        // development aid: walk through the steps unattended, saving a picture of each
};

// Blocks until the player finishes or closes the window. outcome.completed is false if the window was closed early or
// no window could be opened.
WizardOutcome run_setup_wizard(const WizardLaunch& launch);

}  // namespace dw3setup
