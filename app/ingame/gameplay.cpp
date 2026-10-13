#include "gameplay.h"

#include "dw3/recomp.h"

namespace dw3app::ingame {
namespace {

// FIELDSTG, once per field frame. While the party stands on ground with random encounters it counts a countdown down
// by the area's rate; at zero or below it starts a battle (which sets kBattlePending) and draws a new countdown.
constexpr uint32_t kEncounterTick = 0x80091910;
constexpr uint32_t kBattlePending = 0x80098EB4 + 92;

enum Encounters { kNormal, kHalf, kOff, kDouble };

}  // namespace

struct GameplayHook {
    static inline GameplayHooks* instance = nullptr;

    static void encounter_tick(RecompContext* ctx) {
        GameplayHooks& self = *instance;
        const int mode = self.controller_.active().gameplay.encounters;
        auto original = [&] { self.machine_.lookup_original(kEncounterTick)(ctx); };
        if (mode == kOff) return;
        if (mode == kHalf && (self.machine_.video().frame_count() & 1)) return;
        const uint32_t sp = ctx->r[29], ra = ctx->r[31];
        original();
        if (mode == kDouble && dw3_read32(ctx, kBattlePending) == 0) {
            ctx->r[29] = sp;
            ctx->r[31] = ra;
            original();
        }
    }
};

GameplayHooks::GameplayHooks(dw3::Machine& machine, SettingsController& controller)
    : machine_(machine), controller_(controller) {
    GameplayHook::instance = this;
    machine_.override_function(kEncounterTick, &GameplayHook::encounter_tick);
}

GameplayHooks::~GameplayHooks() {
    machine_.override_function(kEncounterTick, nullptr);
    GameplayHook::instance = nullptr;
}

}  // namespace dw3app::ingame
