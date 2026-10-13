#include "bindings.h"

#include <algorithm>
#include <cctype>

#include "dw3/sio.h"

namespace dw3app::input {
namespace {

struct ActionInfo {
    std::string_view id, label, default_text;
    uint16_t bit;
};

constexpr ActionInfo kActions[kActionCount] = {
    {"up", "Up", "Up|DPadUp", dw3::kPadUp},
    {"down", "Down", "Down|DPadDown", dw3::kPadDown},
    {"left", "Left", "Left|DPadLeft", dw3::kPadLeft},
    {"right", "Right", "Right|DPadRight", dw3::kPadRight},
    {"cross", "Cross", "Z|South", dw3::kPadCross},
    {"circle", "Circle", "X|East", dw3::kPadCircle},
    {"square", "Square", "A|West", dw3::kPadSquare},
    {"triangle", "Triangle", "S|North", dw3::kPadTriangle},
    {"l1", "L1", "Q|LeftShoulder", dw3::kPadL1},
    {"r1", "R1", "W|RightShoulder", dw3::kPadR1},
    {"l2", "L2", "1|LeftTrigger", dw3::kPadL2},
    {"r2", "R2", "2|RightTrigger", dw3::kPadR2},
    {"l3", "L3", "|", dw3::kPadL3},
    {"r3", "R3", "|", dw3::kPadR3},
    {"start", "Start", "Return|Start", dw3::kPadStart},
    {"select", "Select", "Backspace|Back", dw3::kPadSelect},
    {"fast_forward", "Fast Fwd", "Tab|RightStick", 0},
    {"fast_forward_latch", "FF Latch", "F10|LeftStick", 0},
    {"game_speed", "Speed", "F4|Guide", 0},
    {"compare", "Compare", "F9|", 0},
};

constexpr std::string_view kPadNames[] = {"", "South", "East", "West", "North", "Back", "Guide", "Start", "LeftStick",
                                          "RightStick", "LeftShoulder", "RightShoulder", "DPadUp", "DPadDown",
                                          "DPadLeft", "DPadRight", "LeftTrigger", "RightTrigger"};
static_assert(std::size(kPadNames) == static_cast<std::size_t>(PadInput::Count));

struct Reserved {
    std::string_view key;
    Action action;
};
constexpr Reserved kReserved[] = {
    {"Up", Action::Up},       {"Down", Action::Down},         {"Left", Action::Left},
    {"Right", Action::Right}, {"Return", Action::Start},      {"Backspace", Action::Select},
    {"Space", Action::Cross}, {"Escape", Action::Circle},
};

bool equal_nocase(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}  // namespace

std::string_view action_id(Action a) { return kActions[index_of(a)].id; }
std::string_view action_label(Action a) { return kActions[index_of(a)].label; }
uint16_t console_bit(Action a) { return kActions[index_of(a)].bit; }

std::string_view pad_input_name(PadInput p) { return kPadNames[static_cast<std::size_t>(p)]; }

std::optional<PadInput> pad_input_from_name(std::string_view name) {
    name = trimmed(name);
    if (name.empty()) return PadInput::None;
    for (std::size_t i = 1; i < std::size(kPadNames); ++i)
        if (equal_nocase(kPadNames[i], name)) return static_cast<PadInput>(i);
    return std::nullopt;
}

std::optional<Binding> parse_binding(std::string_view text) {
    const auto bar = text.find('|');
    if (bar == std::string_view::npos) return std::nullopt;
    const auto pad = pad_input_from_name(text.substr(bar + 1));
    if (!pad) return std::nullopt;
    return Binding{std::string(trimmed(text.substr(0, bar))), *pad};
}

std::string format_binding(const Binding& b) { return b.key + "|" + std::string(pad_input_name(b.pad)); }

std::string default_binding_text(Action a) { return std::string(kActions[index_of(a)].default_text); }

Binding default_binding(Action a) { return *parse_binding(kActions[index_of(a)].default_text); }

std::array<std::string, kActionCount> default_binding_texts() {
    std::array<std::string, kActionCount> t;
    for (std::size_t i = 0; i < kActionCount; ++i) t[i] = default_binding_text(action_at(i));
    return t;
}

Bindings default_bindings() {
    Bindings b;
    for (std::size_t i = 0; i < kActionCount; ++i) b[i] = default_binding(action_at(i));
    return b;
}

Bindings bindings_from_text(const std::array<std::string, kActionCount>& text) {
    Bindings b;
    for (std::size_t i = 0; i < kActionCount; ++i) b[i] = parse_binding(text[i]).value_or(default_binding(action_at(i)));
    return b;
}

std::optional<Action> reserved_key_action(std::string_view key) {
    for (const Reserved& r : kReserved)
        if (equal_nocase(r.key, key)) return r.action;
    return std::nullopt;
}

bool key_assignable(Action a, std::string_view key) {
    const auto owner = reserved_key_action(key);
    return !owner || *owner == a;
}

Bindings with_key(Bindings b, Action a, std::string_view key) {
    if (!key_assignable(a, key)) return b;
    if (!key.empty())
        for (Binding& other : b)
            if (equal_nocase(other.key, key)) other.key.clear();
    b[index_of(a)].key = std::string(key);
    return b;
}

Bindings with_pad(Bindings b, Action a, PadInput pad) {
    if (pad != PadInput::None)
        for (Binding& other : b)
            if (other.pad == pad) other.pad = PadInput::None;
    b[index_of(a)].pad = pad;
    return b;
}

}  // namespace dw3app::input
