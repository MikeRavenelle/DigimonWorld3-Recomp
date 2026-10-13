// What the player's keyboard and gamepad do: one binding (a key and a gamepad button) per console button and per host
// hotkey, written in settings.toml as text such as "Z|South". Key names are SDL's (SDL_GetScancodeName); gamepad
// buttons are named by position (South is the bottom face button, whatever its printed label).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dw3app::input {

enum class Action : uint8_t {
    Up, Down, Left, Right, Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Start, Select,
    FastForward, FastForwardLatch, GameSpeed, Compare,
    Count
};
constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);
constexpr Action action_at(std::size_t i) { return static_cast<Action>(i); }
constexpr std::size_t index_of(Action a) { return static_cast<std::size_t>(a); }
constexpr bool is_console_button(Action a) { return a <= Action::Select; }

enum class PadInput : uint8_t {
    None, South, East, West, North, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DPadUp, DPadDown, DPadLeft, DPadRight, LeftTrigger, RightTrigger,
    Count
};

struct Binding {
    std::string key;  // SDL key name; empty for none
    PadInput pad = PadInput::None;
    bool operator==(const Binding&) const = default;
};

using Bindings = std::array<Binding, kActionCount>;

// "cross": the settings key (controls.cross) and the lower-case name of the action.
std::string_view action_id(Action a);
// "Cross", "Fast Fwd": the label of the settings row.
std::string_view action_label(Action a);
// The PlayStation button bit of a console action (dw3::PadButton); 0 for host hotkeys.
uint16_t console_bit(Action a);

std::string_view pad_input_name(PadInput p);
std::optional<PadInput> pad_input_from_name(std::string_view name);

// "Z|South", "Tab|RightStick", "F9|" (no gamepad button), "|South". Gamepad names are matched without regard to case;
// the key is kept as written. Returns nothing for text that is not a binding.
std::optional<Binding> parse_binding(std::string_view text);
std::string format_binding(const Binding& b);

// The out-of-the-box binding, as text and parsed.
std::string default_binding_text(Action a);
Binding default_binding(Action a);
std::array<std::string, kActionCount> default_binding_texts();
Bindings default_bindings();
// Parses the stored text of every action; an entry that does not parse takes its default.
Bindings bindings_from_text(const std::array<std::string, kActionCount>& text);

// Keys that always do their console job, whatever is bound, so that the menu can never be locked out: the arrows, Return
// (Start), Backspace (Select), Space (Cross) and Escape (Circle). They cannot be bound to another action.
std::optional<Action> reserved_key_action(std::string_view key);
bool key_assignable(Action a, std::string_view key);

// The bindings with a key or a gamepad button assigned to one action; the same key or button is taken away from the
// other actions, so one press never does two things. A reserved key of another action is refused (bindings unchanged).
Bindings with_key(Bindings b, Action a, std::string_view key);
Bindings with_pad(Bindings b, Action a, PadInput pad);

// Console buttons held, as a mask of dw3::PadButton: the bindings plus the reserved keys. `key_down(name)` and
// `pad_down(PadInput)` say what is held.
template <class KeyDown, class PadDown>
uint16_t console_buttons(const Bindings& b, KeyDown&& key_down, PadDown&& pad_down) {
    static constexpr std::string_view kReserved[] = {"Up", "Down", "Left", "Right", "Return", "Backspace", "Space", "Escape"};
    uint16_t mask = 0;
    for (std::size_t i = 0; i < kActionCount; ++i) {
        const Action a = action_at(i);
        if (!is_console_button(a)) continue;
        if ((!b[i].key.empty() && key_down(std::string_view(b[i].key))) ||
            (b[i].pad != PadInput::None && pad_down(b[i].pad)))
            mask |= console_bit(a);
    }
    for (const std::string_view k : kReserved)
        if (key_down(k)) mask |= console_bit(*reserved_key_action(k));
    return mask;
}

}  // namespace dw3app::input
