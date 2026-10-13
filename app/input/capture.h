// Waiting for the player to press the key or gamepad button for a binding. Only the rules live here; the host feeds
// it the presses and the frame ticks.
#pragma once

#include <optional>
#include <string_view>

#include "bindings.h"

namespace dw3app::input {

class BindingCapture {
public:
    static constexpr int kTimeoutFrames = 300;  // five seconds at 60 frames a second

    void begin(Action action) {
        action_ = action;
        released_ = false;
        frames_left_ = kTimeoutFrames;
    }
    // Waiting for a press, or for everything to be let go again after one: the press that bound a button must not
    // also act in the menu.
    std::optional<Action> active() const { return action_; }
    bool waiting() const { return action_ && !released_; }

    // The bindings after a key press: the key goes to the action, taken from any other. Escape cancels, and a key that
    // is reserved for another action is ignored (still waiting). Nothing happens unless waiting.
    std::optional<Bindings> key_pressed(const Bindings& current, std::string_view key_name) {
        if (!waiting() || key_name.empty()) return std::nullopt;
        if (key_name == "Escape") {
            finish();
            return std::nullopt;
        }
        if (!key_assignable(*action_, key_name)) return std::nullopt;
        finish();
        return with_key(current, *action_, key_name);
    }
    std::optional<Bindings> pad_pressed(const Bindings& current, PadInput pad) {
        if (!waiting() || pad == PadInput::None) return std::nullopt;
        finish();
        return with_pad(current, *action_, pad);
    }

    // Once per frame. Ends the capture when it times out or, after a press, when nothing is held any more.
    void tick(bool anything_held) {
        if (!action_) return;
        if (--frames_left_ <= 0 || (released_ && !anything_held)) action_.reset();
    }

private:
    void finish() {
        released_ = true;
        frames_left_ = kTimeoutFrames;
    }

    std::optional<Action> action_;
    bool released_ = false;
    int frames_left_ = 0;
};

}  // namespace dw3app::input
