#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

#include "game_text.h"
#include "settings_list.h"

using namespace dw3app;
using namespace dw3app::ingame;

namespace {

Settings defaults() {
    LoadReport report;
    return settings_from_toml("", report);
}

const Entry& binding_entry(input::Action a) {
    for (const Entry& e : entries())
        if (e.kind == Kind::Binding && e.action == a) return e;
    ADD_FAILURE() << "no binding row";
    return entries().front();
}

std::size_t index_of_first(Page p) {
    const auto& list = entries();
    return static_cast<std::size_t>(std::find_if(list.begin(), list.end(), [&](const Entry& e) { return e.page == p; }) - list.begin());
}

}  // namespace

TEST(IngameControls, TheSlashHasAGlyph) {
    EXPECT_EQ(encode_game_text("Z/A"), (std::vector<uint8_t>{0x27, 1, 0x0F, 0x0E, 0}));
}

TEST(IngameControls, PagesRunInOrderAndAreContiguous) {
    const auto& list = entries();
    EXPECT_EQ(list.front().page, Page::Graphics);
    EXPECT_EQ(list.back().page, Page::Gameplay);
    for (std::size_t i = 1; i < list.size(); ++i) EXPECT_LE(list[i - 1].page, list[i].page) << i;
    EXPECT_LT(index_of_first(Page::Audio), index_of_first(Page::Controls));
    EXPECT_LT(index_of_first(Page::Controls), index_of_first(Page::Gameplay));
}

TEST(IngameControls, PageNames) {
    EXPECT_EQ(page_name(Page::Graphics), "Graphics");
    EXPECT_EQ(page_name(Page::Controls), "Controls");
    EXPECT_EQ(page_name(Page::Gameplay), "Gameplay");
}

TEST(IngameControls, ControlsPageListsSettingsThenBindingsThenReset) {
    const auto [first, count] = page_range(index_of_first(Page::Controls));
    const auto& list = entries();
    EXPECT_EQ(list[first].info->id, "controls.pad");
    std::size_t bindings = 0;
    for (std::size_t i = first; i < first + count; ++i) bindings += list[i].kind == Kind::Binding;
    EXPECT_EQ(bindings, input::kActionCount);
    EXPECT_EQ(list[first + count - 1].kind, Kind::Reset);
    for (std::size_t i = first; i < first + count; ++i)
        if (list[i].kind == Kind::Binding) {
            EXPECT_GE(i, first + 4) << "settings come before the bindings";
            break;
        }
}

TEST(IngameControls, JumpingGoesToThePageStartAndWraps) {
    const std::size_t graphics = index_of_first(Page::Graphics), audio = index_of_first(Page::Audio);
    EXPECT_EQ(jump_page(graphics, 1), audio);
    EXPECT_EQ(jump_page(graphics + 3, 1), audio);
    EXPECT_EQ(jump_page(audio, -1), graphics);
    EXPECT_EQ(jump_page(audio + 1, -1), graphics);  // from inside a page: the previous page
}

TEST(IngameControls, JumpingWrapsAtBothEnds) {
    const std::size_t gameplay = index_of_first(Page::Gameplay);
    EXPECT_EQ(jump_page(gameplay, 1), 0u);
    EXPECT_EQ(jump_page(0, -1), gameplay);
}

TEST(IngameControls, PageRangesTileTheList) {
    std::size_t at = 0, total = 0;
    while (at < entries().size()) {
        const auto [first, count] = page_range(at);
        EXPECT_EQ(first, at);
        total += count;
        at = first + count;
    }
    EXPECT_EQ(total, entries().size());
}

TEST(IngameControls, BindingRowShowsKeyAndButton) {
    const Settings s = defaults();
    EXPECT_EQ(row_text(s, binding_entry(input::Action::Cross)), "Cross: Z/A");
    EXPECT_EQ(row_text(s, binding_entry(input::Action::Start)), "Start: Enter/Menu");
    EXPECT_EQ(row_text(s, binding_entry(input::Action::Compare)), "Compare: F9");
    EXPECT_EQ(row_text(s, binding_entry(input::Action::L3)), "L3: None");
}

TEST(IngameControls, BindingRowFollowsThePromptStyle) {
    const Settings s = defaults();
    RowContext ps;
    ps.style = input::PromptStyle::PlayStation;
    EXPECT_EQ(row_text(s, binding_entry(input::Action::Cross), ps), "Cross: Z/Cross");
    RowContext nintendo;
    nintendo.style = input::PromptStyle::Nintendo;
    EXPECT_EQ(row_text(s, binding_entry(input::Action::Cross), nintendo), "Cross: Z/B");
}

TEST(IngameControls, ARowWaitingForAKeySaysSo) {
    const Settings s = defaults();
    RowContext waiting;
    waiting.capturing = input::Action::Circle;
    EXPECT_EQ(row_text(s, binding_entry(input::Action::Circle), waiting), "Circle: Press key");
    EXPECT_EQ(row_text(s, binding_entry(input::Action::Cross), waiting), "Cross: Z/A");
}

TEST(IngameControls, RowsShowTheStoredBinding) {
    Settings s = defaults();
    std::string error;
    ASSERT_TRUE(set_text(s, *find_setting("controls.triangle"), "Left Shift|RightShoulder", error)) << error;
    EXPECT_EQ(row_text(s, binding_entry(input::Action::Triangle)), "Triangle: LShift/RB");
}

TEST(IngameControls, EveryBindingRowFitsWithTheLongestNames) {
    Settings s = defaults();
    std::string error;
    for (const input::PromptStyle style : {input::PromptStyle::Xbox, input::PromptStyle::PlayStation, input::PromptStyle::Nintendo}) {
        RowContext context;
        context.style = style;
        for (const Entry& e : entries()) {
            if (e.kind != Kind::Binding) continue;
            ASSERT_TRUE(set_text(s, *find_setting("controls." + std::string(input::action_id(e.action))),
                                 "Keypad Multiply|RightShoulder", error)) << error;
            EXPECT_LE(row_text(s, e, context).size(), 22u) << e.label;
        }
    }
}

TEST(IngameControls, BindingAndResetRowsDoNotStep) {
    const Settings s = defaults();
    EXPECT_EQ(stepped_value(s, binding_entry(input::Action::Cross), 1), "");
    for (const Entry& e : entries())
        if (e.kind == Kind::Reset) EXPECT_EQ(stepped_value(s, e, -1), "");
}

TEST(IngameControls, ResetRowReadsAsItsLabel) {
    const Settings s = defaults();
    for (const Entry& e : entries())
        if (e.kind == Kind::Reset) EXPECT_EQ(row_text(s, e), "Reset Controls");
}

TEST(IngameControls, ResetRestoresEveryControlSetting) {
    Settings s = defaults();
    std::string error;
    ASSERT_TRUE(set_text(s, *find_setting("controls.cross"), "C|North", error));
    ASSERT_TRUE(set_text(s, *find_setting("controls.deadzone"), "45", error));
    ASSERT_TRUE(set_text(s, *find_setting("controls.pad"), "3", error));
    ASSERT_TRUE(set_text(s, *find_setting("audio.volume"), "30", error));
    for (const auto& [id, value] : controls_reset_changes()) ASSERT_TRUE(set_text(s, *find_setting(id), value, error)) << error;
    Settings expected = defaults();
    ASSERT_TRUE(set_text(expected, *find_setting("audio.volume"), "30", error));
    EXPECT_EQ(s, expected);  // the audio setting is not a control and stays
}

TEST(IngameControls, ControlSettingsStepLikeOtherRows) {
    Settings s = defaults();
    const auto find = [](std::string_view id) -> const Entry& {
        for (const Entry& e : entries())
            if (e.kind == Kind::Setting && e.info->id == id) return e;
        ADD_FAILURE() << id;
        return entries().front();
    };
    EXPECT_EQ(stepped_value(s, find("controls.deadzone"), 1), "30");
    EXPECT_EQ(stepped_value(s, find("controls.deadzone"), -1), "20");
    EXPECT_EQ(stepped_value(s, find("controls.pad"), 1), "1");
    EXPECT_EQ(stepped_value(s, find("controls.pad"), -1), "4");
    EXPECT_EQ(row_text(s, find("controls.prompts")), "Prompts: Auto");
    std::string error;
    ASSERT_TRUE(set_text(s, *find_setting("controls.prompts"), "playstation", error));
    EXPECT_EQ(row_text(s, find("controls.prompts")), "Prompts: PlayStation");
}
