#include "settings_screen.h"

#include <algorithm>

#include "game_text.h"

namespace dw3app::ingame {
namespace {

// Pad object: per-button shift of the logical bit, and the newly-pressed and auto-repeat masks of port 0.
constexpr uint32_t kPad = 0x8004AF78;
constexpr uint32_t kPadPressed = kPad + 0x48;
constexpr uint32_t kPadRepeat = kPad + 0x4A;
constexpr uint32_t kPadShifts = kPad + 0xA4;

}  // namespace

bool Guest::shows(uint32_t window, const std::vector<uint8_t>& text) const {
    const uint32_t at = r32(window + kWindowText);
    if (at == 0) return false;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (r8(at + static_cast<uint32_t>(i)) != text[i]) return false;
    return true;
}

void Guest::show(uint32_t window, const std::vector<uint8_t>& text) {
    if (shows(window, text)) return;
    // The game copies the string while it is called; a frame below the stack pointer keeps it clear of the callee's
    // own frames.
    const uint32_t sp = ctx_->r[29];
    const uint32_t frame = sp - kScratchFrame;
    for (std::size_t i = 0; i < text.size() && i < kScratchFrame - kStringOffset; ++i)
        dw3_write8(ctx_, frame + kStringOffset + static_cast<uint32_t>(i), text[i]);
    ctx_->r[29] = frame;
    call(r32(window + kWindowSetText), window, frame + kStringOffset, 0xFFFFFFFFu);
    ctx_->r[29] = sp;
}

bool Guest::pressed(Button b, bool with_repeat) const {
    const uint32_t shift = r8(kPadShifts + b);
    uint32_t mask = r16(kPadPressed);
    if (with_repeat) mask |= r16(kPadRepeat);
    return (mask >> shift) & 1u;
}

void Guest::consume_input() {
    w16(kPadPressed, 0);
    w16(kPadRepeat, 0);
}

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

Settings view_of(const ScreenModel& model) {
    Settings view = model.controller.active();
    for (const ScreenChange& c : model.pending) {
        std::string ignored;
        set_text(view, *find_setting(c.id), c.value, ignored);
    }
    return view;
}

input::PromptStyle style_of(const ScreenModel& model) {
    return model.controls ? model.controls->prompt_style() : input::PromptStyle::Xbox;
}

RowContext context_of(const ScreenModel& model) {
    RowContext context;
    context.style = style_of(model);
    if (model.controls) context.capturing = model.controls->capturing();
    context.monitors = model.monitors;
    context.adaptive_vsync = model.adaptive_vsync;
    context.hdr_available = model.hdr_available;
    return context;
}

std::string select_name_of(const ScreenModel& model) {
    const auto bindings = input::bindings_from_text(model.controller.active().controls.bindings);
    return input::select_hint(bindings[input::index_of(input::Action::Select)], style_of(model),
                              model.controls && model.controls->pad_connected());
}

namespace {

void move_cursor(Guest& g, uint32_t cursor, uint32_t x, uint32_t row) {
    const uint32_t y = kFirstRowY + kRowPitch * row;
    if (g.r32(cursor + kCursorTargetY) != y) g.call(g.r32(cursor + kCursorMoveTo), cursor, x, y);
}

}  // namespace

void draw_screen(Guest& g, const ScreenWindows& windows, uint32_t selected, const ScreenModel& model) {
    const auto& list = entries();
    const uint32_t count = static_cast<uint32_t>(list.size());
    const uint32_t rows = windows.row_count;
    const RowContext context = context_of(model);
    const Settings view = view_of(model);
    const uint32_t top = std::min(selected > 2 ? selected - 2 : 0u, count > rows ? count - rows : 0u);
    const auto [page_first, page_count] = page_range(selected);
    g.show(windows.header,
           context.capturing ? text_of({"Press key - Esc cancels"})
                             : text_of({page_name(list[selected].page), "  ", std::to_string(selected - page_first + 1),
                                        " of ", std::to_string(page_count)}));
    for (uint32_t i = 0; i < rows; ++i) {
        const uint32_t at = top + i;
        g.show(windows.rows[i], at < count ? text_of({row_text(view, list[at], context)}) : text_of({""}));
    }
    move_cursor(g, windows.cursor, windows.cursor_x, selected - top);
}

bool step_screen(Guest& g, ScreenModel& model, uint32_t& selected) {
    const auto& list = entries();
    const uint32_t count = static_cast<uint32_t>(list.size());
    selected = std::min(selected, count - 1);
    Settings view = view_of(model);

    // While a key or button is being captured the pad belongs to the capture.
    const bool idle = model.controls && model.controls->capturing();
    const bool up = !idle && g.pressed(kUp, true), down = !idle && g.pressed(kDown, true);
    const bool left = !idle && g.pressed(kLeft, true), right = !idle && g.pressed(kRight, true);
    const bool page_back = !idle && g.pressed(kL1, false), page_forward = !idle && g.pressed(kR1, false);
    const bool confirm = !idle && g.pressed(kConfirm, false);
    const bool close = !idle && (g.pressed(kCancel, false) || g.pressed(kMenuClose, false));
    g.consume_input();  // the menu's own handler then sees an idle pad

    if (page_back != page_forward) {
        selected = static_cast<uint32_t>(jump_page(selected, page_forward ? 1 : -1));
        g.play(kSoundCursor);
    } else if (up != down) {
        selected = up ? (selected + count - 1) % count : (selected + 1) % count;
        g.play(kSoundCursor);
    } else if (confirm && list[selected].kind == Kind::Binding) {
        if (model.controls) model.controls->begin_capture(list[selected].action);
        g.play(kSoundConfirm);
    } else if (confirm && list[selected].kind == Kind::Reset) {
        for (const auto& [id, value] : controls_reset_changes()) model.pending.push_back({id, value});
        g.play(kSoundConfirm);
    } else if (left != right) {
        if (list[selected].kind == Kind::Resolution) {
            const auto changes = resolution_changes(view, context_of(model), right ? 1 : -1);
            for (const auto& [id, value] : changes) {
                model.pending.push_back({id, value});
                std::string ignored;
                set_text(view, *find_setting(id), value, ignored);
            }
            if (!changes.empty()) g.play(kSoundCursor);
        }
        const std::string value = stepped_value(view, list[selected], right ? 1 : -1, context_of(model));
        if (!value.empty()) {
            model.pending.push_back({std::string(list[selected].info->id), value});
            std::string ignored;
            set_text(view, *list[selected].info, value, ignored);
            g.play(kSoundCursor);
        }
    }
    if (close) g.play(kSoundCancel);
    return close;
}

}  // namespace dw3app::ingame
