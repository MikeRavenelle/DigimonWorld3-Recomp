// The host side of port 1: reads the keyboard and the chosen gamepad into console pad bits through the player's
// bindings, follows gamepads being plugged in and out, runs the hotkey bindings and captures new bindings for the
// settings screen. Scripted presses (audit runs) are added on top.
#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "controller.h"
#include "capture.h"
#include "controls_host.h"
#include "dw3/sio.h"

namespace dw3app::input {

class HostInput final : public dw3::PadSource, public ControlsHost {
public:
    struct Press {
        uint16_t button;
        int64_t from, to;
    };
    struct Hotkeys {
        std::function<void()> toggle_fast_forward, cycle_game_speed, compare_presets;
    };

    HostInput(SettingsController& settings, std::function<void(const std::string&)> notify);
    ~HostInput() override;

    Hotkeys hotkeys;                             // what the hotkey bindings do; set by the application
    const std::vector<Press>* script = nullptr;  // scripted presses by frame
    int64_t frame = 0;

    // Console pad (port 1).
    bool connected(int port) override { return port == 0; }
    uint16_t buttons(int port) override;

    // Once per frame: hotkeys, the capture, and the capture's timeout.
    void update();
    // Gamepad events (plugged in, removed) and the capture's key and button presses. Returns true when the event
    // was used by the capture.
    bool handle_event(const SDL_Event& event);
    // A change to a controls setting: re-reads the bindings and re-selects the pad.
    void settings_changed();
    // Fast-forward key or button is held.
    bool fast_forward_held();

    // ControlsHost
    PromptStyle prompt_style() const override;
    bool pad_connected() const override { return gamepad_ != nullptr; }
    void begin_capture(Action action) override;
    std::optional<Action> capturing() const override { return capture_.active(); }

private:
    bool key_down(std::string_view name);
    bool pad_down(PadInput input) const;
    bool action_down(Action action);
    void select_pad();
    void bind(const Bindings& next);
    bool anything_held();

    SettingsController& settings_;
    std::function<void(const std::string&)> notify_;
    Bindings bindings_;
    SDL_Gamepad* gamepad_ = nullptr;
    std::optional<SDL_JoystickID> gamepad_id_;
    std::unordered_map<std::string, SDL_Scancode> scancodes_;
    BindingCapture capture_;
    bool was_down_[kActionCount] = {};
    bool announce_ = false;  // gamepads plugged in or out are announced once the program runs
};

}  // namespace dw3app::input
