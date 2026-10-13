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

// STFGTREP (the battle results screen, game mode 0x14): each party member's row finalises its experience share in
// the method at kFinaliseShare (a0 = row; the share is at +96, the done flag at +100, the share is returned). The
// results screen prints that share and the grant adds it to the member's total (capped at 999999), so scaling it
// there, once, keeps the printed figure equal to the amount granted. The overlay's code overlaps other mode overlays
// at this address, so the override only acts in the results mode and when the function holds the instructions of this
// overlay.
constexpr uint32_t kFinaliseShare = 0x80084170;
constexpr uint32_t kFinaliseShareSignature = 0x8e032744, kFinaliseShareSignatureAt = 0x800841AC;
constexpr uint32_t kShare = 96, kShareFinalised = 100;
constexpr uint32_t kGameMode = 0x800554D0, kResultsMode = 0x14;

// Message windows (field dialogue, menus, battle text), MAIN, once per frame; a0 is the window. State +12 is 1 while the
// text prints. It prints with a delay: the halfword at +170 is the frames between characters and +168 counts them; each
// time it is passed the halfword at +164 (the position printed so far, from the start at +166) goes up by one, and the
// byte at +193 enables the typewriter while the byte at +195 is set once the box is full. kMeasureBox is the function
// that, for a window in state 1, stores the position of the end of the box in +164 (what pressing a button does).
constexpr uint32_t kTextWindowTick = 0x8001A890, kMeasureBox = 0x80019C2C;
constexpr uint32_t kWindowState = 12, kWindowPrinted = 164, kWindowTimer = 168, kWindowDelay = 170;
constexpr uint32_t kWindowTypewriter = 193, kWindowBoxFull = 195;
constexpr uint32_t kWindowPrinting = 1;

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
    static void finalise_share(RecompContext* ctx) {
        GameplayHooks& self = *instance;
        const uint32_t row = ctx->r[4];
        const bool scalable = dw3_read32(ctx, kGameMode) == kResultsMode &&
                              dw3_read32(ctx, kFinaliseShareSignatureAt) == kFinaliseShareSignature &&
                              dw3_read32(ctx, row + kShareFinalised) == 0;
        self.machine_.lookup_original(kFinaliseShare)(ctx);
        if (!scalable) return;
        const uint32_t share = scaled_experience(dw3_read32(ctx, row + kShare), xp_multiplier(self.controller_.active().gameplay.xp_multiplier));
        dw3_write32(ctx, row + kShare, share);
        ctx->r[2] = share;
    }
    static void text_window_tick(RecompContext* ctx) {
        GameplayHooks& self = *instance;
        const TextSpeed speed = text_speed(self.controller_.active().gameplay.text_speed);
        const uint32_t window = ctx->r[4];
        const bool typing = dw3_read32(ctx, window + kWindowState) == kWindowPrinting && dw3_read8(ctx, window + kWindowTypewriter) != 0 &&
                            static_cast<int16_t>(dw3_read16(ctx, window + kWindowDelay)) > 0 && dw3_read8(ctx, window + kWindowBoxFull) == 0;
        if (speed != TextSpeed::Normal && typing) {
            const uint32_t sp = ctx->r[29], ra = ctx->r[31];
            const uint16_t shown = dw3_read16(ctx, window + kWindowPrinted);
            dw3_call(ctx, kMeasureBox);
            const uint16_t page_end = dw3_read16(ctx, window + kWindowPrinted);
            ctx->r[4] = window;
            ctx->r[29] = sp;
            ctx->r[31] = ra;
            dw3_write16(ctx, window + kWindowPrinted, static_cast<uint16_t>(text_position_before_tick(speed, shown, page_end)));
            // The window counts a character when the timer passes the delay; leave it just short for instant (its box is
            // already printed) and make it pass for fast.
            dw3_write16(ctx, window + kWindowTimer, speed == TextSpeed::Fast ? dw3_read16(ctx, window + kWindowDelay) : 0);
        }
        self.machine_.lookup_original(kTextWindowTick)(ctx);
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
    machine_.override_function(kFinaliseShare, &GameplayHook::finalise_share);
    machine_.override_function(kTextWindowTick, &GameplayHook::text_window_tick);
}

GameplayHooks::~GameplayHooks() {
    machine_.override_function(kEncounterTick, nullptr);
    machine_.override_function(kAttackTask, nullptr);
    machine_.override_function(kFinaliseShare, nullptr);
    machine_.override_function(kTextWindowTick, nullptr);
    GameplayHook::instance = nullptr;
}

}  // namespace dw3app::ingame
