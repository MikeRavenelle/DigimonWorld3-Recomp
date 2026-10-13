// How gamepad buttons are named in text the host draws: A/B/X/Y on an Xbox pad, Cross/Circle/... on a PlayStation
// pad, B/A/Y/X on a Nintendo pad. Positions never change (South stays the console's Cross); only the names do.
#pragma once

#include <string>
#include <string_view>

#include "bindings.h"

namespace dw3app::input {

enum class PromptStyle { Auto, Xbox, PlayStation, Nintendo };
// What kind of pad is connected (SDL_GetGamepadType), as far as button names go.
enum class PadFamily { Unknown, Xbox, PlayStation, Nintendo };

// Auto follows the connected pad and falls back to Xbox names when there is none.
PromptStyle resolve_style(PromptStyle chosen, PadFamily connected);

// A short name (at most five characters) of a gamepad button in a style; Auto reads as Xbox.
std::string_view pad_label(PadInput p, PromptStyle style);
// A key name shortened for a menu row: "Backspace" is "Bksp", "Left Shift" is "LShift".
std::string key_label(std::string_view sdl_key_name);

// "Z/A", "Tab/RS", "Z" or "A" when only one half is bound, "None" for neither.
std::string binding_label(const Binding& b, PromptStyle style);

// The name of the way to open the settings screen in the hint line: the gamepad's name for the Select binding when
// a pad is connected, otherwise "SELECT".
std::string select_hint(const Binding& select, PromptStyle style, bool pad_connected);

}  // namespace dw3app::input
