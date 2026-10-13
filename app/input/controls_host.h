// What the in-game settings screen needs from the host's input: the style of button names, whether a pad is
// plugged in (for the hint line), and the "press a key or button" capture used by the binding rows.
#pragma once

#include <optional>

#include "bindings.h"
#include "glyphs.h"

namespace dw3app::input {

class ControlsHost {
public:
    virtual ~ControlsHost() = default;
    virtual PromptStyle prompt_style() const = 0;  // never Auto
    virtual bool pad_connected() const = 0;
    // Waits for the next key or gamepad button and binds it to `action` (Escape or a timeout cancels). While it
    // waits, the game's own pad input must be ignored.
    virtual void begin_capture(Action action) = 0;
    // The action being captured; stays set until every key and button has been released again.
    virtual std::optional<Action> capturing() const = 0;
};

}  // namespace dw3app::input
