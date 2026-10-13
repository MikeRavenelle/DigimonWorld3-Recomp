#include "field_menu.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>

#include "dw3/recomp.h"
#include "game_text.h"
#include "settings_list.h"

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

// Window object methods (through the window's own table) and fields.
constexpr uint32_t kWindowSetText = 0x114;   // (window, string pointer, -1) copies the string into the window
constexpr uint32_t kWindowText = 0x5C;       // pointer to the window's current text
constexpr uint32_t kCursorMoveTo = 0x84;     // (cursor, x, y)
constexpr uint32_t kCursorTargetY = 0x5C;
constexpr uint32_t kCursorX = 176;
constexpr uint32_t kFirstRowY = 49, kRowPitch = 14;

// Entries of the menu (to put the original texts back): locale table lookup and the string id table.
constexpr uint32_t kLocaleTableFunction = 0x80044B58;  // (0xB1) -> table of the status texts
constexpr uint32_t kLocaleTableId = 0xB1;
constexpr uint32_t kRowStringIds = 0x8003EB20;  // 6 ids per variant, 4 bytes each
constexpr uint32_t kRowStringsPerVariant = 6;

// Pad object: per-button shift of the logical bit, and the newly-pressed and auto-repeat masks of port 0.
constexpr uint32_t kPad = 0x8004AF78;
constexpr uint32_t kPadPressed = kPad + 0x48;
constexpr uint32_t kPadRepeat = kPad + 0x4A;
constexpr uint32_t kPadShifts = kPad + 0xA4;
enum Button { kSelect = 0, kUp = 4, kRight = 5, kDown = 6, kLeft = 7, kTriangle = 12, kCircle = 13 };

// Sounds: the game's cue function pointer and the cues the menu itself plays.
constexpr uint32_t kPlaySound = 0x800553F0;
constexpr uint32_t kSoundCursor = 0x8004513E;
constexpr uint32_t kSoundConfirm = 0x8004503C;
constexpr uint32_t kSoundCancel = 0x800450BD;

constexpr uint32_t kScratchFrame = 0x100;  // guest stack taken for a string handed to the game
constexpr uint32_t kStringOffset = 0x20;

class Guest {
public:
    explicit Guest(RecompContext* ctx) : ctx_(ctx) {}
    uint32_t r32(uint32_t a) const { return dw3_read32(ctx_, a); }
    uint32_t r16(uint32_t a) const { return dw3_read16(ctx_, a); }
    uint32_t r8(uint32_t a) const { return dw3_read8(ctx_, a); }
    void w16(uint32_t a, uint32_t v) { dw3_write16(ctx_, a, static_cast<uint16_t>(v)); }
    void w32(uint32_t a, uint32_t v) { dw3_write32(ctx_, a, v); }
    uint32_t call(uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        ctx_->r[4] = a0;
        ctx_->r[5] = a1;
        ctx_->r[6] = a2;
        ctx_->r[7] = a3;
        dw3_call(ctx_, fn);
        return ctx_->r[2];
    }
    // The window's text equals `text` (an encoded, zero-terminated string).
    bool shows(uint32_t window, const std::vector<uint8_t>& text) const {
        const uint32_t at = r32(window + kWindowText);
        if (at == 0) return false;
        for (std::size_t i = 0; i < text.size(); ++i)
            if (r8(at + static_cast<uint32_t>(i)) != text[i]) return false;
        return true;
    }
    void show(uint32_t window, const std::vector<uint8_t>& text) {
        if (shows(window, text)) return;
        // The game copies the string while it is called; a frame below the stack pointer keeps it clear of the
        // callee's own frames.
        const uint32_t sp = ctx_->r[29];
        const uint32_t frame = sp - kScratchFrame;
        for (std::size_t i = 0; i < text.size() && i < kScratchFrame - kStringOffset; ++i)
            dw3_write8(ctx_, frame + kStringOffset + static_cast<uint32_t>(i), text[i]);
        ctx_->r[29] = frame;
        call(r32(window + kWindowSetText), window, frame + kStringOffset, 0xFFFFFFFFu);
        ctx_->r[29] = sp;
    }
    void play(uint32_t cue) { call(r32(kPlaySound), cue); }
    bool pressed(Button b, bool with_repeat) const {
        const uint32_t shift = r8(kPadShifts + b);
        uint32_t mask = r16(kPadPressed);
        if (with_repeat) mask |= r16(kPadRepeat);
        return (mask >> shift) & 1u;
    }
    void consume_input() {
        w16(kPadPressed, 0);
        w16(kPadRepeat, 0);
    }

private:
    RecompContext* ctx_;
};

std::vector<uint8_t> text_of(std::initializer_list<std::string_view> parts) {
    std::vector<uint8_t> out;
    for (const std::string_view p : parts) {
        std::vector<uint8_t> bytes = encode_game_text(p);
        bytes.pop_back();
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    out.push_back(0);
    return out;
}

// The header's hint, in place of the menu's own: the triangle glyph, "Close", then the way to the settings.
std::vector<uint8_t> header_hint() {
    std::vector<uint8_t> out = {1, 0x34, 1, 1};
    const std::vector<uint8_t> rest = text_of({"Close SELECT: SETTINGS"});
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

}  // namespace

struct MenuHook {
    static FieldMenuSettings* instance;

    static Settings view_of(const FieldMenuSettings& self) {
        Settings view = self.controller_.active();
        for (const FieldMenuSettings::Change& c : self.pending_) {
            std::string ignored;
            set_text(view, *find_setting(c.id), c.value, ignored);
        }
        return view;
    }

    static void move_cursor(Guest& g, uint32_t cursor, uint32_t row) {
        const uint32_t y = kFirstRowY + kRowPitch * row;
        if (g.r32(cursor + kCursorTargetY) != y) g.call(g.r32(cursor + kCursorMoveTo), cursor, kCursorX, y);
    }

    static void draw_screen(Guest& g, uint32_t windows, uint32_t rows, uint32_t selected, const Settings& view) {
        const auto& list = entries();
        const uint32_t count = static_cast<uint32_t>(list.size());
        const uint32_t top = std::min(selected > 2 ? selected - 2 : 0u, count > rows ? count - rows : 0u);
        g.show(g.r32(windows + kWindowHeader),
               text_of({"Settings  ", std::to_string(selected + 1), " of ", std::to_string(count)}));
        for (uint32_t i = 0; i < rows; ++i) {
            const uint32_t at = top + i;
            g.show(g.r32(windows + kWindowRow0 + 4 * i),
                   at < count ? text_of({row_text(view, list[at])}) : text_of({""}));
        }
        move_cursor(g, g.r32(windows + kWindowCursor), selected - top);
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
        move_cursor(g, g.r32(windows + kWindowCursor), g.r32(task + kCursorRow));
    }

    static void tick_screen(FieldMenuSettings& self, RecompContext* ctx, uint32_t task, uint32_t windows) {
        Guest g(ctx);
        const auto& list = entries();
        const uint32_t count = static_cast<uint32_t>(list.size());
        const uint32_t rows = g.r32(task + kRowCount);
        uint32_t selected = std::min(g.r32(task + kScreenCursor), count - 1);
        Settings view = view_of(self);

        const bool up = g.pressed(kUp, true), down = g.pressed(kDown, true);
        const bool left = g.pressed(kLeft, true), right = g.pressed(kRight, true);
        const bool close = g.pressed(kCircle, false) || g.pressed(kTriangle, false);
        g.consume_input();  // the menu's own handler then sees an idle pad and only draws

        if (up != down) {
            selected = up ? (selected + count - 1) % count : (selected + 1) % count;
            g.play(kSoundCursor);
        } else if (left != right) {
            const std::string value = stepped_value(view, list[selected], right ? 1 : -1);
            if (!value.empty()) {
                self.pending_.push_back({std::string(list[selected].info->id), value});
                std::string ignored;
                set_text(view, *list[selected].info, value, ignored);
                g.play(kSoundCursor);
            }
        }
        g.w32(task + kScreenCursor, selected);
        if (close) {
            g.w32(task + kScreenMarker, 0);
            g.play(kSoundCancel);
            restore_menu(g, windows, task, rows);
        }
        ctx->r[4] = task;
        ctx->r[5] = windows;
        self.machine_.lookup_original(kListTaskFunction)(ctx);
        if (!close) draw_screen(g, windows, rows, selected, view);
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
        if (header) g.show(header, header_hint());
        if (g.pressed(kSelect, false) && g.r32(task + kRowCount) > 0) {
            g.w32(task + kScreenCursor, 0);
            g.w32(task + kScreenMarker, kScreenOpen);
            g.play(kSoundConfirm);
            draw_screen(g, windows, g.r32(task + kRowCount), 0, view_of(*self));
        }
    }
};

FieldMenuSettings* MenuHook::instance = nullptr;

FieldMenuSettings::FieldMenuSettings(dw3::Machine& machine, SettingsController& controller)
    : machine_(machine), controller_(controller) {
    MenuHook::instance = this;
    machine_.override_function(kListTaskFunction, &MenuHook::list_task);
}

FieldMenuSettings::~FieldMenuSettings() {
    machine_.override_function(kListTaskFunction, nullptr);
    MenuHook::instance = nullptr;
}

void FieldMenuSettings::apply_pending() {
    if (pending_.empty()) return;
    for (const Change& c : pending_) controller_.set(c.id, c.value);
    pending_.clear();
    std::string error;
    controller_.save(error);
}

}  // namespace dw3app::ingame
