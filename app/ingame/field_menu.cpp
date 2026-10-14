#include "field_menu.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>

#include "dw3/recomp.h"
#include "game_text.h"
#include "settings_screen.h"
#include "title_menu.h"

namespace dw3app::ingame {
namespace {

// ---- The START menu in the field (addresses in the USA executable) -------------------------------------------------

constexpr uint32_t kListTaskFunction = 0x800126FC;  // per-frame update of the menu's list; reached through the task's method table

// Task fields (the task is allocated at run time; the list function gets it as its first argument).
constexpr uint32_t kState = 0x0C;      // 1 while the menu runs
constexpr uint32_t kSubState = 0x10;   // 3 while the list waits for input
constexpr uint32_t kCursorRow = 0x58;  // selected entry of the menu
constexpr uint32_t kRowCount = 0x5C;   // entries shown: 5, or 6 when CARD FOLDERS exists
constexpr uint32_t kRowVariant = 0x60; // 0 or 1: which row of the string table the entries come from
constexpr uint32_t kScreenCursor = 0x68;  // unused by the menu while it waits for input: the settings cursor
constexpr uint32_t kScreenMarker = 0x6C;  // ... and the flag that the settings screen is open
constexpr uint32_t kScreenOpen = 0x53455453;
constexpr uint32_t kStateRunning = 1;
constexpr uint32_t kSubStateWaiting = 3;

// The windows the list task keeps in the array it is called with (a1): the header, five or six rows, the cursor.
constexpr uint32_t kWindowHeader = 0 * 4;
constexpr uint32_t kWindowRow0 = 3 * 4;
constexpr uint32_t kWindowCursor = 9 * 4;

// Entries of the menu (to put the original texts back): locale table lookup and the string id table.
constexpr uint32_t kLocaleTableFunction = 0x80044B58;  // (0xB1) -> table of the status texts
constexpr uint32_t kLocaleTableId = 0xB1;
constexpr uint32_t kRowStringIds = 0x8003EB20;  // 6 ids per variant, 4 bytes each
constexpr uint32_t kRowStringsPerVariant = 6;

// The header's hint, in place of the menu's own: the close button (the game's triangle glyph, or the pad's name for
// it), "Close", then the way to the settings, all named in one prompt style.
std::vector<uint8_t> header_hint(const std::string& close_name, const std::string& select_name) {
    std::vector<uint8_t> out = close_name.empty() ? std::vector<uint8_t>{1, 0x34, 1, 1} : text_of({close_name, ": "});
    if (!out.empty() && out.back() == 0) out.pop_back();
    const std::vector<uint8_t> rest = text_of({"Close  ", select_name, ": SETTINGS"});
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

}  // namespace

struct MenuHook {
    static FieldMenuSettings* instance;

    static ScreenWindows windows_of(Guest& g, uint32_t windows, uint32_t rows) {
        ScreenWindows out;
        out.header = g.r32(windows + kWindowHeader);
        out.cursor = g.r32(windows + kWindowCursor);
        out.row_count = std::min(rows, kMaxRows);
        for (uint32_t i = 0; i < out.row_count; ++i) out.rows[i] = g.r32(windows + kWindowRow0 + 4 * i);
        return out;
    }

    // The menu's own texts back in the rows and the cursor on the entry that was selected.
    static void restore_menu(Guest& g, uint32_t windows, uint32_t task, uint32_t rows) {
        const uint32_t table = g.call(g.r32(kLocaleTableFunction), kLocaleTableId);
        const uint32_t variant = g.r32(task + kRowVariant);
        for (uint32_t i = 0; i < rows; ++i) {
            const uint32_t window = g.r32(windows + kWindowRow0 + 4 * i);
            const uint32_t id = g.r32(kRowStringIds + variant * 4 * kRowStringsPerVariant + 4 * i);
            g.call(g.r32(window + kWindowSetText), window, table, id);
        }
        const uint32_t cursor = g.r32(windows + kWindowCursor);
        const uint32_t y = kFirstRowY + kRowPitch * g.r32(task + kCursorRow);
        if (g.r32(cursor + kCursorTargetY) != y) g.call(g.r32(cursor + kCursorMoveTo), cursor, kCursorX, y);
    }

    static void tick_screen(FieldMenuSettings& self, RecompContext* ctx, uint32_t task, uint32_t windows) {
        Guest g(ctx);
        const uint32_t rows = g.r32(task + kRowCount);
        uint32_t selected = g.r32(task + kScreenCursor);
        const bool close = step_screen(g, self.model_, selected);
        g.w32(task + kScreenCursor, selected);
        if (close) {
            g.w32(task + kScreenMarker, 0);
            restore_menu(g, windows, task, rows);
        }
        ctx->r[4] = task;
        ctx->r[5] = windows;
        self.machine_.lookup_original(kListTaskFunction)(ctx);
        if (!close) {
            ScreenWindows screen = windows_of(g, windows, rows);
            draw_screen(g, screen, selected, self.model_);
        }
    }

    static void list_task(RecompContext* ctx) {
        FieldMenuSettings* self = instance;
        const uint32_t task = ctx->r[4], windows = ctx->r[5];
        if (!self) {
            static_cast<dw3::Machine*>(ctx->machine)->lookup_original(kListTaskFunction)(ctx);
            return;
        }
        Guest g(ctx);
        const bool open = g.r32(task + kState) == kStateRunning && g.r32(task + kSubState) == kSubStateWaiting &&
                          g.r32(task + kScreenMarker) == kScreenOpen;
        if (open) {
            tick_screen(*self, ctx, task, windows);
            return;
        }
        self->machine_.lookup_original(kListTaskFunction)(ctx);
        if (g.r32(task + kState) != kStateRunning || g.r32(task + kSubState) != kSubStateWaiting) return;
        const uint32_t header = g.r32(windows + kWindowHeader);
        if (header) g.show(header, header_hint(close_name_of(self->model_), select_name_of(self->model_)));
        if (g.pressed(kSelect, false) && g.r32(task + kRowCount) > 0) {
            g.w32(task + kScreenCursor, 0);
            g.w32(task + kScreenMarker, kScreenOpen);
            g.play(kSoundConfirm);
            ScreenWindows screen = windows_of(g, windows, g.r32(task + kRowCount));
            draw_screen(g, screen, 0, self->model_);
        }
    }
};

FieldMenuSettings* MenuHook::instance = nullptr;

FieldMenuSettings::FieldMenuSettings(dw3::Machine& machine, SettingsController& controller, input::ControlsHost* controls)
    : machine_(machine), model_{controller, controls}, title_(machine, model_) {
    MenuHook::instance = this;
    machine_.override_function(kListTaskFunction, &MenuHook::list_task);
}

FieldMenuSettings::~FieldMenuSettings() {
    machine_.override_function(kListTaskFunction, nullptr);
    MenuHook::instance = nullptr;
}

void FieldMenuSettings::apply_pending() {
    if (model_.pending.empty()) return;
    for (const ScreenChange& c : model_.pending) model_.controller.set(c.id, c.value);
    model_.pending.clear();
    std::string error;
    model_.controller.save(error);
}

}  // namespace dw3app::ingame
