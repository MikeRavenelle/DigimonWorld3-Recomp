// A settings screen inside the game's START menu in the field. While the menu's list is open, SELECT turns the list
// into the settings list: the same windows, cursor, font and sounds, with one row per setting. Up and down move the
// cursor, left and right change the value (applied at once and saved), L1 and R1 jump between pages (Graphics, Audio,
// Controls, Gameplay), cross on a binding row waits for the next key or button, circle or triangle returns to the menu.
//
// The screen keeps no state of its own: whether it is open and where the cursor is live in the menu task's memory,
// so a save state taken while it is open loads back into it.
#pragma once

#include <string>
#include <vector>

#include "controller.h"
#include "controls_host.h"
#include "display.h"
#include "dw3/machine.h"

namespace dw3app::ingame {

class FieldMenuSettings {
public:
    // Hooks the menu list of `machine`. At most one instance per process may exist at a time; the machine and the
    // controller and `controls` (without it the binding rows cannot be changed) must outlive it.
    FieldMenuSettings(dw3::Machine& machine, SettingsController& controller, input::ControlsHost* controls = nullptr);
    ~FieldMenuSettings();
    FieldMenuSettings(const FieldMenuSettings&) = delete;
    FieldMenuSettings& operator=(const FieldMenuSettings&) = delete;

    // Applies and saves the changes made on the screen since the last call. Call it once per frame from the frame
    // hook, where the presenter may be reconfigured safely (the game code that asks for the change runs mid-frame).
    void apply_pending();

    // What the monitor and resolution rows choose from, and whether vertical sync can be adaptive. The list must
    // outlive the screen or be replaced before it is freed; null leaves those rows without choices.
    void set_display_context(const MonitorList* monitors, bool adaptive_vsync) {
        monitors_ = monitors;
        adaptive_vsync_ = adaptive_vsync;
    }

private:
    friend struct MenuHook;
    struct Change {
        std::string id, value;
    };
    dw3::Machine& machine_;
    SettingsController& controller_;
    input::ControlsHost* controls_;
    std::vector<Change> pending_;
    const MonitorList* monitors_ = nullptr;
    bool adaptive_vsync_ = false;
};

}  // namespace dw3app::ingame
