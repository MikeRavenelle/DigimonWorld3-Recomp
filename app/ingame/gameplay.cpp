#include "gameplay.h"

#include "dw3/recomp.h"
#include "gameplay_scaling.h"

namespace dw3app::ingame {
namespace {

// FIELDSTG, once per field frame. While the party stands on ground with random encounters it counts a countdown down
// by the area's rate; at zero or below it starts a battle (which sets kBattlePending) and draws a new countdown.
constexpr uint32_t kEncounterTick = 0x80091910;
constexpr uint32_t kBattlePending = 0x80098EB4 + 92;

// FIGHTSTG attack task (one per attack, run every frame). +80 is the side of the attacker.
constexpr uint32_t kAttackTask = 0x8008C0BC;
constexpr uint32_t kAttackerSide = 80;
// Combatants: 32-byte records, three per side, the party's first at kRecords and the enemies' 96 bytes after;
// kActive holds, per side, the index of the member being acted on. The attacker's side is 0 for the party.
constexpr uint32_t kRecords = 0x800A31F8, kActive = 0x800A31F0;
constexpr uint32_t kRecordSize = 32, kSideStride = 96, kHp = 8;
constexpr uint32_t kPartySide = 0, kEnemySide = 1;
constexpr uint16_t kLift = 0x4000;

// STFGTREP (the battle results screen, game mode 0x14): gives one party member experience and runs the level-ups it
// causes. a0 is the member's index, a1 the amount; the member's total (at +24 of its record) is capped at 999999.
// The overlay's code overlaps other mode overlays at this address, so the override only acts in the results mode and
// when the function's first instructions are the ones of this overlay.
constexpr uint32_t kGrantExperience = 0x80085870;
constexpr uint32_t kGrantExperienceSignature = 0x8c42ac2c, kGrantExperienceSignatureAt = 0x80085888;
constexpr uint32_t kGameMode = 0x800554D0, kResultsMode = 0x14;

enum Encounters { kNormal, kHalf, kOff, kDouble };

}  // namespace

struct GameplayHook {
    static inline GameplayHooks* instance = nullptr;

    // The battle's attack task runs once per frame; in the frame that applies a hit it subtracts the damage from the
    // target's HP and, at zero or below, starts the defeat. God mode keeps the target's HP out of that arithmetic's
    // reach: a party member is lifted far above any damage for the duration of the call and put back afterwards, and
    // an enemy is lowered to 1 HP so that any hit that lands defeats it (a miss leaves it at the HP it had).
    static void attack_task(RecompContext* ctx) {
        GameplayHooks& self = *instance;
        const auto original = [&] { self.machine_.lookup_original(kAttackTask)(ctx); };
        if (!self.controller_.active().gameplay.god_mode) return original();
        const uint32_t task = ctx->r[4];
        const uint32_t target_side = dw3_read8(ctx, task + kAttackerSide) == kPartySide ? kEnemySide : kPartySide;
        const uint32_t record = kRecords + kSideStride * target_side + kRecordSize * dw3_read32(ctx, kActive + 4 * target_side);
        const uint16_t before = dw3_read16(ctx, record + kHp);
        if (before == 0 || before > 0x3FFF) return original();
        dw3_write16(ctx, record + kHp, target_side == kPartySide ? before + kLift : 1);
        original();
        const uint16_t after = dw3_read16(ctx, record + kHp);
        const bool defeated = target_side == kEnemySide && static_cast<int16_t>(after) <= 0;
        if (!defeated) dw3_write16(ctx, record + kHp, before);
    }
    static void grant_experience(RecompContext* ctx) {
        GameplayHooks& self = *instance;
        const bool in_results = dw3_read32(ctx, kGameMode) == kResultsMode &&
                                dw3_read32(ctx, kGrantExperienceSignatureAt) == kGrantExperienceSignature;
        if (in_results) ctx->r[5] = scaled_experience(ctx->r[5], xp_multiplier(self.controller_.active().gameplay.xp_multiplier));
        self.machine_.lookup_original(kGrantExperience)(ctx);
    }
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
    machine_.override_function(kAttackTask, &GameplayHook::attack_task);
    machine_.override_function(kGrantExperience, &GameplayHook::grant_experience);
}

GameplayHooks::~GameplayHooks() {
    machine_.override_function(kEncounterTick, nullptr);
    machine_.override_function(kAttackTask, nullptr);
    machine_.override_function(kGrantExperience, nullptr);
    GameplayHook::instance = nullptr;
}

}  // namespace dw3app::ingame
