// The settings screen on the title menu (Start / Continue). SELECT there opens the same list as in the field START
// menu, drawn with the game's own windows and cursor; the title menu stands still until the screen is closed.
#pragma once

#include "dw3/machine.h"
#include "settings_screen.h"

namespace dw3app::ingame {

class TitleMenuSettings {
public:
    // Hooks the title menu task of `machine`. The machine and `model` must outlive it.
    TitleMenuSettings(dw3::Machine& machine, ScreenModel& model);
    ~TitleMenuSettings();
    TitleMenuSettings(const TitleMenuSettings&) = delete;
    TitleMenuSettings& operator=(const TitleMenuSettings&) = delete;

private:
    friend struct TitleHook;
    dw3::Machine& machine_;
    ScreenModel& model_;
    ScreenWindows windows_;
    uint32_t backdrop_ = 0;  // guest objects of the open screen
    uint32_t hint_ = 0;
    uint32_t task_ = 0;      // the menu task that has the screen open
    uint64_t frame_ = 0;
};

}  // namespace dw3app::ingame
