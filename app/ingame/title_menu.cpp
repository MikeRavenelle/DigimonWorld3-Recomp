#include "title_menu.h"

namespace dw3app::ingame {
namespace {

// ---- The title menu (STDWTITL, game mode 0xE; addresses in the USA executable) --------------------------------------

constexpr uint32_t kGameMode = 0x800554D0, kTitleMode = 0xE;
constexpr uint32_t kMenuTask = 0x80084B68;  // per-frame update of the Start / Continue menu task; a0 is the task
// The overlay's code overlaps other mode overlays at this address: its first branches tell it apart.
constexpr uint32_t kMenuTaskSignatureAt = 0x80084B88, kMenuTaskSignature = 0x10650017;

// Task fields.
constexpr uint32_t kState = 0x0C, kSubState = 0x10;
constexpr uint32_t kStateChoosing = 2, kSubStateWaiting = 1;  // the menu waits for up, down or cross
constexpr uint32_t kScreenMarker = 0x74;  // unused by the task: the flag that the settings screen is open
constexpr uint32_t kScreenCursor = 0x78;  // ... and its cursor
constexpr uint32_t kScreenOpen = 0x53455453;

// The game's task runner (MAIN): runs the children of a group, once per frame from the root. Everything drawn into
// the title's one layer lands behind what was drawn before it, so the screen is drawn first, before the title's own
// tasks.
constexpr uint32_t kRunChildren = 0x80017ECC;
constexpr uint32_t kRunObject = 0x80017DDC;  // (object) runs one object and its children; it swaps stacks

// Window, cursor and fade objects of MAIN (the same the field menu builds its list from).
constexpr uint32_t kCreateWindow = 0x8001AAB4;  // (layer, style, x, y)
constexpr uint32_t kCreateCursor = 0x8001AFE0;  // (layer, kind, x, y)
constexpr uint32_t kCreateFade = 0x80012070;    // (layer): a full-screen quad that darkens what is behind it
constexpr uint32_t kWindowTick = 0x8001A890, kObjectCleanup = 0x800142E0;
constexpr uint32_t kObjectState = 0x0C, kObjectTick = 0x48, kObjectCleanupAt = 0x4C;
constexpr uint32_t kFadeLevel = 0x5C, kFadeHeld = 2;  // the fade stays at its level in state 2; level 0xFF00 is black
constexpr uint32_t kFadeDimmed = 0x8800;
constexpr uint32_t kLayer = 0x1000, kStyleText = 1;  // the only layer the title has
constexpr uint32_t kWindowShow = 0x148;  // (window, 1) lays the box out
constexpr uint32_t kCursorShow = 0x80;  // (cursor, 1) shows it; 0 hides it
constexpr uint32_t kHeaderX = 152, kHeaderY = 19, kRowX = 189;
constexpr uint32_t kHintX = 100, kHintY = 8;
constexpr uint32_t kRows = 5;

}  // namespace

struct TitleHook {
    static TitleMenuSettings* instance;

    static bool is_title(const Guest& g) { return g.r32(kGameMode) == kTitleMode; }

    static bool is_menu(const Guest& g) {
        return is_title(g) && g.r32(kMenuTaskSignatureAt) == kMenuTaskSignature;
    }

    static bool waiting(const Guest& g, uint32_t task) {
        return g.r32(task + kState) == kStateChoosing && g.r32(task + kSubState) == kSubStateWaiting;
    }

    static bool alive(const Guest& g, uint32_t object, uint32_t tick) {
        return object != 0 && g.r32(object + kObjectTick) == tick && g.r32(object + kObjectCleanupAt) == kObjectCleanup;
    }

    static uint32_t make_window(Guest& g, uint32_t x, uint32_t y) {
        const uint32_t window = g.call(kCreateWindow, kLayer, kStyleText, x, y);
        g.call(g.r32(window + kWindowShow), window, 1);
        return window;
    }

    static void build_windows(Guest& g, TitleMenuSettings& self) {
        ScreenWindows& w = self.windows_;
        w.header = make_window(g, kHeaderX, kHeaderY);
        w.row_count = kRows;
        for (uint32_t i = 0; i < kRows; ++i) w.rows[i] = make_window(g, kRowX, kFirstRowY + kRowPitch * i);
        w.cursor = g.call(kCreateCursor, kLayer, 0, kCursorX, kFirstRowY);
        g.call(g.r32(w.cursor + kCursorShow), w.cursor, 1);
        self.backdrop_ = g.call(kCreateFade, kLayer);
        g.w32(self.backdrop_ + kObjectState, kFadeHeld);
        g.w32(self.backdrop_ + kFadeLevel, kFadeDimmed);
    }

    // The hint under the logo: the way to the settings, in the game's window like every other text.
    static void show_hint(Guest& g, TitleMenuSettings& self) {
        if (!alive(g, self.hint_, kWindowTick)) self.hint_ = make_window(g, kHintX, kHintY);
        g.show(self.hint_, text_of({select_name_of(self.model_), ": SETTINGS"}));
    }

    // The objects belong to no parent that would run them, so the screen runs them itself.
    static void run_object(Guest& g, uint32_t object) { g.call(kRunObject, object); }

    static void run_screen(Guest& g, const TitleMenuSettings& self) {
        const ScreenWindows& w = self.windows_;
        run_object(g, w.header);
        for (uint32_t i = 0; i < w.row_count; ++i) run_object(g, w.rows[i]);
        run_object(g, w.cursor);
        run_object(g, self.backdrop_);
    }

    // First thing each frame: draw what the menu task asked for in the frame before.
    static void run_children(RecompContext* ctx) {
        TitleMenuSettings* self = instance;
        auto& machine = *static_cast<dw3::Machine*>(ctx->machine);
        Guest g(ctx);
        const uint64_t frame = machine.video().frame_count();
        if (self && frame != self->frame_) {
            self->frame_ = frame;
            if (!is_title(g)) {
                self->windows_ = {};
                self->backdrop_ = self->hint_ = self->task_ = 0;
            } else if (self->task_ && g.r32(self->task_ + kObjectTick) == kMenuTask) {
                const uint32_t a0 = ctx->r[4], sp = ctx->r[29], ra = ctx->r[31];
                if (g.r32(self->task_ + kScreenMarker) == kScreenOpen) run_screen(g, *self);
                else if (waiting(g, self->task_)) run_object(g, self->hint_);
                ctx->r[4] = a0;
                ctx->r[29] = sp;
                ctx->r[31] = ra;
            }
        }
        machine.lookup_original(kRunChildren)(ctx);
    }

    static void menu_task(RecompContext* ctx) {
        TitleMenuSettings* self = instance;
        auto& machine = *static_cast<dw3::Machine*>(ctx->machine);
        Guest g(ctx);
        if (!self || !is_menu(g)) return machine.lookup_original(kMenuTask)(ctx);
        const uint32_t task = ctx->r[4];
        const uint32_t sp = ctx->r[29], ra = ctx->r[31];
        const auto restore = [&] {
            ctx->r[4] = task;
            ctx->r[29] = sp;
            ctx->r[31] = ra;
        };
        if (g.r32(task + kScreenMarker) == kScreenOpen) {
            // A loaded save state can come back with the screen open and its objects gone.
            if (!alive(g, self->windows_.header, kWindowTick)) build_windows(g, *self);
            self->task_ = task;
            uint32_t selected = g.r32(task + kScreenCursor);
            const bool close = step_screen(g, self->model_, selected);
            g.w32(task + kScreenCursor, selected);
            if (close) g.w32(task + kScreenMarker, 0);
            else draw_screen(g, self->windows_, selected, self->model_);
            restore();
            return;
        }
        machine.lookup_original(kMenuTask)(ctx);
        if (!waiting(g, task)) return;
        self->task_ = task;
        show_hint(g, *self);
        if (g.pressed(kSelect, false)) {
            if (!alive(g, self->windows_.header, kWindowTick)) build_windows(g, *self);
            g.w32(task + kScreenCursor, 0);
            g.w32(task + kScreenMarker, kScreenOpen);
            g.play(kSoundConfirm);
            draw_screen(g, self->windows_, 0, self->model_);
        }
        restore();
    }
};

TitleMenuSettings* TitleHook::instance = nullptr;

TitleMenuSettings::TitleMenuSettings(dw3::Machine& machine, ScreenModel& model) : machine_(machine), model_(model) {
    TitleHook::instance = this;
    machine_.override_function(kMenuTask, &TitleHook::menu_task);
    machine_.override_function(kRunChildren, &TitleHook::run_children);
}

TitleMenuSettings::~TitleMenuSettings() {
    machine_.override_function(kMenuTask, nullptr);
    machine_.override_function(kRunChildren, nullptr);
    TitleHook::instance = nullptr;
}

}  // namespace dw3app::ingame
