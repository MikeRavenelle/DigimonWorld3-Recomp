// Gameplay options that change what the game does: random encounters and the party's invulnerability.
// Each is a host override of one game function (see docs/ARCHITECTURE.md, "Host overrides"); the options are read from
// the controller every time the function runs, so a change takes effect at once.
#pragma once

#include "controller.h"
#include "dw3/machine.h"

namespace dw3app::ingame {

class GameplayHooks {
public:
    // At most one instance per process may exist at a time; the machine and the controller must outlive it.
    GameplayHooks(dw3::Machine& machine, SettingsController& controller);
    ~GameplayHooks();
    GameplayHooks(const GameplayHooks&) = delete;
    GameplayHooks& operator=(const GameplayHooks&) = delete;

private:
    friend struct GameplayHook;
    dw3::Machine& machine_;
    SettingsController& controller_;
};

}  // namespace dw3app::ingame
