// What the settings screen needs wherever the game shows it (the field START menu, the title menu): reading the pad,
// calling guest functions, putting text into the game's windows, and the page / row logic that follows the pad.
#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "controller.h"
#include "controls_host.h"
#include "display.h"
#include "dw3/recomp.h"
#include "settings_list.h"

namespace dw3app::ingame {

// A setting changed on the screen, applied between frames.
struct ScreenChange {
    std::string id, value;
};

// The state the screens share: where changes go and what the monitor and resolution rows choose from.
struct ScreenModel {
    ScreenModel(SettingsController& settings, input::ControlsHost* host) : controller(settings), controls(host) {}
    SettingsController& controller;
    input::ControlsHost* controls;
    std::vector<ScreenChange> pending;
    const MonitorList* monitors = nullptr;
    bool adaptive_vsync = false;
};

// The game's own button numbering: 12 is the cancel button (the pad's circle), 13 confirm (cross) and 14 the menu's
// close button (triangle).
enum Button { kSelect = 0, kUp = 4, kRight = 5, kDown = 6, kLeft = 7, kL1 = 10, kR1 = 11, kCancel = 12, kConfirm = 13, kMenuClose = 14 };

// Sounds: the game's cue function pointer and the cues the menus themselves play.
constexpr uint32_t kPlaySound = 0x800553F0;
constexpr uint32_t kSoundCursor = 0x8004513E;
constexpr uint32_t kSoundConfirm = 0x8004503C;
constexpr uint32_t kSoundCancel = 0x800450BD;

// Window object methods (through the window's own table) and fields.
constexpr uint32_t kWindowSetText = 0x114;  // (window, string pointer, -1) copies the string into the window
constexpr uint32_t kWindowText = 0x5C;      // pointer to the window's current text
constexpr uint32_t kCursorMoveTo = 0x84;    // (cursor, x, y)
constexpr uint32_t kCursorTargetY = 0x5C;
constexpr uint32_t kCursorX = 176;
constexpr uint32_t kFirstRowY = 49, kRowPitch = 14;
constexpr uint32_t kMaxRows = 6;

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
    bool shows(uint32_t window, const std::vector<uint8_t>& text) const;
    void show(uint32_t window, const std::vector<uint8_t>& text);
    void play(uint32_t cue) { call(r32(kPlaySound), cue); }
    bool pressed(Button b, bool with_repeat) const;
    void consume_input();

private:
    RecompContext* ctx_;
};

// The game's text for the characters of `parts` joined, zero-terminated.
std::vector<uint8_t> text_of(std::initializer_list<std::string_view> parts);

// The windows of one screen: the header, up to kMaxRows rows and the cursor.
struct ScreenWindows {
    uint32_t header = 0;
    std::array<uint32_t, kMaxRows> rows{};
    uint32_t cursor = 0;
    uint32_t row_count = 0;
};

// The settings with the changes made on the screen applied.
Settings view_of(const ScreenModel& model);
input::PromptStyle style_of(const ScreenModel& model);
RowContext context_of(const ScreenModel& model);
// "SELECT" or the name of the button the player bound to it.
std::string select_name_of(const ScreenModel& model);

// Draws the page the row `selected` belongs to into the windows.
void draw_screen(Guest& g, const ScreenWindows& windows, uint32_t selected, const ScreenModel& model);

// Follows the pad for one frame: moves the cursor (`selected`), changes values, waits for key captures. The pad is
// consumed. Returns true when the player closed the screen.
bool step_screen(Guest& g, ScreenModel& model, uint32_t& selected);

}  // namespace dw3app::ingame
