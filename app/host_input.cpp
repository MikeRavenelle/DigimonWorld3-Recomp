#include "host_input.h"

#include "analog.h"

namespace dw3app::input {
namespace {

struct ButtonPair {
    SDL_GamepadButton sdl;
    PadInput input;
};
constexpr ButtonPair kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PadInput::South},
    {SDL_GAMEPAD_BUTTON_EAST, PadInput::East},
    {SDL_GAMEPAD_BUTTON_WEST, PadInput::West},
    {SDL_GAMEPAD_BUTTON_NORTH, PadInput::North},
    {SDL_GAMEPAD_BUTTON_BACK, PadInput::Back},
    {SDL_GAMEPAD_BUTTON_GUIDE, PadInput::Guide},
    {SDL_GAMEPAD_BUTTON_START, PadInput::Start},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, PadInput::LeftStick},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PadInput::RightStick},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PadInput::LeftShoulder},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PadInput::RightShoulder},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, PadInput::DPadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PadInput::DPadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PadInput::DPadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PadInput::DPadRight},
};

constexpr int16_t kCaptureTriggerLevel = 16000;

PadFamily family_of(SDL_GamepadType type) {
    switch (type) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return PadFamily::PlayStation;
        case SDL_GAMEPAD_TYPE_XBOX360:
        case SDL_GAMEPAD_TYPE_XBOXONE: return PadFamily::Xbox;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return PadFamily::Nintendo;
        default: return PadFamily::Unknown;
    }
}

}  // namespace

HostInput::HostInput(SettingsController& settings, std::function<void(const std::string&)> notify)
    : settings_(settings), notify_(std::move(notify)) {
    settings_changed();
}

HostInput::~HostInput() {
    if (gamepad_) SDL_CloseGamepad(gamepad_);
}

bool HostInput::key_down(std::string_view name) {
    auto found = scancodes_.find(std::string(name));
    if (found == scancodes_.end()) found = scancodes_.emplace(std::string(name), SDL_GetScancodeFromName(std::string(name).c_str())).first;
    return found->second != SDL_SCANCODE_UNKNOWN && SDL_GetKeyboardState(nullptr)[found->second];
}

bool HostInput::pad_down(PadInput input) const {
    if (!gamepad_) return false;
    if (input == PadInput::LeftTrigger) return trigger_pressed(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    if (input == PadInput::RightTrigger) return trigger_pressed(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    for (const ButtonPair& b : kButtons)
        if (b.input == input) return SDL_GetGamepadButton(gamepad_, b.sdl);
    return false;
}

bool HostInput::action_down(Action action) {
    const Binding& b = bindings_[index_of(action)];
    return (!b.key.empty() && key_down(b.key)) || (b.pad != PadInput::None && pad_down(b.pad));
}

uint16_t HostInput::buttons(int port) {
    if (port != 0) return 0;
    uint16_t mask = console_buttons(
        bindings_, [&](std::string_view key) { return key_down(key); }, [&](PadInput pad) { return pad_down(pad); });
    const Settings::Controls& controls = settings_.active().controls;
    if (gamepad_ && controls.left_stick)
        mask |= stick_dpad_bits(SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX),
                                SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY), controls.deadzone);
    if (script)
        for (const Press& p : *script)
            if (frame >= p.from && frame < p.to) mask |= p.button;
    return mask;
}

bool HostInput::fast_forward_held() { return !capture_.active() && action_down(Action::FastForward); }

PromptStyle HostInput::prompt_style() const {
    const PadFamily family = gamepad_ ? family_of(SDL_GetGamepadType(gamepad_)) : PadFamily::Unknown;
    return resolve_style(static_cast<PromptStyle>(settings_.active().controls.prompts), family);
}

void HostInput::settings_changed() {
    bindings_ = bindings_from_text(settings_.active().controls.bindings);
    select_pad();
}

void HostInput::select_pad() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    const std::vector<SDL_JoystickID> connected(ids, ids + (ids ? count : 0));
    SDL_free(ids);
    const auto pick = choose_pad<SDL_JoystickID>(connected, settings_.active().controls.pad, gamepad_id_);
    if (pick == gamepad_id_ && (gamepad_ != nullptr) == pick.has_value()) return;
    if (gamepad_) SDL_CloseGamepad(gamepad_);
    gamepad_ = pick ? SDL_OpenGamepad(*pick) : nullptr;
    gamepad_id_ = gamepad_ ? pick : std::nullopt;
    if (announce_) notify_(gamepad_ ? std::string("Gamepad: ") + SDL_GetGamepadName(gamepad_) : std::string("No gamepad"));
    announce_ = true;
}

void HostInput::begin_capture(Action action) { capture_.begin(action); }

void HostInput::bind(const Bindings& next) {
    for (std::size_t i = 0; i < kActionCount; ++i)
        if (next[i] != bindings_[i]) settings_.set("controls." + std::string(action_id(action_at(i))), format_binding(next[i]));
    std::string error;
    settings_.save(error);
}

bool HostInput::handle_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) select_pad();
    if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        if (gamepad_id_ == e.gdevice.which) {
            SDL_CloseGamepad(gamepad_);
            gamepad_ = nullptr;
            gamepad_id_.reset();
        }
        select_pad();
    }
    if (!capture_.active()) return false;
    std::optional<Bindings> bound;
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        bound = capture_.key_pressed(bindings_, SDL_GetScancodeName(e.key.scancode));
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.which == gamepad_id_) {
        for (const ButtonPair& b : kButtons)
            if (b.sdl == e.gbutton.button) bound = capture_.pad_pressed(bindings_, b.input);
    } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.which == gamepad_id_ && e.gaxis.value > kCaptureTriggerLevel) {
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) bound = capture_.pad_pressed(bindings_, PadInput::LeftTrigger);
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) bound = capture_.pad_pressed(bindings_, PadInput::RightTrigger);
    }
    if (bound) bind(*bound);
    return e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
}

bool HostInput::anything_held() {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; i < count; ++i)
        if (keys[i]) return true;
    if (!gamepad_) return false;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (SDL_GetGamepadButton(gamepad_, static_cast<SDL_GamepadButton>(b))) return true;
    return pad_down(PadInput::LeftTrigger) || pad_down(PadInput::RightTrigger);
}

void HostInput::update() {
    if (capture_.active()) {
        capture_.tick(anything_held());
        for (bool& down : was_down_) down = true;  // nothing fires on release of the capture's own keys
        return;
    }
    const auto edge = [&](Action action, const std::function<void()>& fire) {
        const bool down = action_down(action);
        if (down && !was_down_[index_of(action)] && fire) fire();
        was_down_[index_of(action)] = down;
    };
    edge(Action::FastForwardLatch, hotkeys.toggle_fast_forward);
    edge(Action::GameSpeed, hotkeys.cycle_game_speed);
    edge(Action::Compare, hotkeys.compare_presets);
}

}  // namespace dw3app::input
