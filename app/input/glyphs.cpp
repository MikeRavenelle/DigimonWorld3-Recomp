#include "glyphs.h"

#include <algorithm>
#include <cstddef>

namespace dw3app::input {
namespace {

constexpr std::size_t kInputCount = static_cast<std::size_t>(PadInput::Count);

// Indexed by PadInput.
constexpr std::string_view kXbox[kInputCount] = {"None", "A", "B", "X", "Y", "View", "Xbox", "Menu", "LS", "RS", "LB", "RB",
                                                 "Up", "Down", "Left", "Right", "LT", "RT"};
constexpr std::string_view kPlayStation[kInputCount] = {"None", "Cross", "Circ", "Sqr", "Tri", "Share", "PS", "Start", "L3", "R3", "L1", "R1",
                                                        "Up", "Down", "Left", "Right", "L2", "R2"};
constexpr std::string_view kNintendo[kInputCount] = {"None", "B", "A", "Y", "X", "Minus", "Home", "Plus", "LS", "RS", "L", "R",
                                                     "Up", "Down", "Left", "Right", "ZL", "ZR"};

struct KeyAbbreviation {
    std::string_view name, label;
};
constexpr KeyAbbreviation kKeyAbbreviations[] = {
    {"Backspace", "Bksp"}, {"Return", "Enter"}, {"Escape", "Esc"}, {"Left Shift", "LShift"}, {"Right Shift", "RShift"},
    {"Left Ctrl", "LCtrl"}, {"Right Ctrl", "RCtrl"}, {"Left Alt", "LAlt"}, {"Right Alt", "RAlt"}, {"PageUp", "PgUp"},
    {"PageDown", "PgDn"}, {"Delete", "Del"}, {"Insert", "Ins"}, {"CapsLock", "Caps"}, {"Keypad Enter", "KpEnt"},
};
constexpr std::size_t kMaxKeyLabel = 6;

}  // namespace

PromptStyle resolve_style(PromptStyle chosen, PadFamily connected) {
    if (chosen != PromptStyle::Auto) return chosen;
    switch (connected) {
        case PadFamily::PlayStation: return PromptStyle::PlayStation;
        case PadFamily::Nintendo: return PromptStyle::Nintendo;
        default: return PromptStyle::Xbox;
    }
}

std::string_view pad_label(PadInput p, PromptStyle style) {
    const std::size_t i = static_cast<std::size_t>(p);
    switch (style) {
        case PromptStyle::PlayStation: return kPlayStation[i];
        case PromptStyle::Nintendo: return kNintendo[i];
        default: return kXbox[i];
    }
}

std::string key_label(std::string_view name) {
    for (const KeyAbbreviation& k : kKeyAbbreviations)
        if (k.name == name) return std::string(k.label);
    return std::string(name.substr(0, kMaxKeyLabel));
}

std::string binding_label(const Binding& b, PromptStyle style) {
    const bool has_pad = b.pad != PadInput::None;
    if (b.key.empty() && !has_pad) return "None";
    std::string text = key_label(b.key);
    // The arrow keys and the D-pad share their names: "Right" says it all, "Right/Right" runs out of the row.
    if (has_pad && text == pad_label(b.pad, style)) return text;
    if (has_pad) text += (b.key.empty() ? "" : "/") + std::string(pad_label(b.pad, style));
    return text;
}

std::string select_hint(const Binding& select, PromptStyle style, bool pad_connected) {
    if (pad_connected && select.pad != PadInput::None) return std::string(pad_label(select.pad, style));
    return "SELECT";
}

std::string close_hint(const Binding& close, PromptStyle style, bool pad_connected) {
    if (!pad_connected || style == PromptStyle::PlayStation || close.pad == PadInput::None) return "";
    return std::string(pad_label(close.pad, style));
}

}  // namespace dw3app::input
