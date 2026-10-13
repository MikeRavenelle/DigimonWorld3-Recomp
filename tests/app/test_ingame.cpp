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

const Entry& entry(std::string_view id) {
    for (const Entry& e : entries())
        if (e.info->id == id) return e;
    ADD_FAILURE() << "no entry " << id;
    return entries().front();
}

void apply(Settings& s, const Entry& e, std::string_view value) {
    std::string error;
    ASSERT_TRUE(set_text(s, *e.info, value, error)) << error;
}

}  // namespace

TEST(IngameList, ListsExactlyTheLiveVisibleNonTextSettings) {
    std::set<std::string_view> expected;
    for (const SettingInfo& info : all_settings())
        if (!info.hidden && info.apply == Apply::Live && info.type != Type::String) expected.insert(info.id);
    std::set<std::string_view> listed;
    for (const Entry& e : entries()) listed.insert(e.info->id);
    EXPECT_EQ(listed, expected);
}

TEST(IngameList, PresetComesFirstAndNothingNeedsARestart) {
    ASSERT_FALSE(entries().empty());
    EXPECT_EQ(entries().front().info->id, "graphics.preset");
    for (const Entry& e : entries()) {
        EXPECT_FALSE(e.info->is_restart()) << e.info->id;
        EXPECT_FALSE(e.info->hidden) << e.info->id;
    }
}

TEST(IngameList, EveryRowFitsTheMenuWindow) {
    // The window holds about 20 characters of the game's proportional font.
    Settings s = defaults();
    for (const Entry& e : entries()) {
        EXPECT_LE(row_text(s, e).size(), 20u) << e.info->id;
        EXPECT_LE(e.label.size(), 12u) << e.info->id;
        if (e.info->type == Type::Choice) {
            for (const std::string_view name : e.info->choices) {
                apply(s, e, name);
                EXPECT_LE(row_text(s, e).size(), 20u) << e.info->id << " " << name;
            }
        }
    }
}

TEST(IngameList, RowsReadLabelAndValue) {
    Settings s = defaults();
    EXPECT_EQ(row_text(s, entry("graphics.preset")), "Preset: Original");
    EXPECT_EQ(row_text(s, entry("graphics.true_color")), "True Color: Off");
    EXPECT_EQ(row_text(s, entry("graphics.internal_res")), "3D Scale: 1X");
    EXPECT_EQ(row_text(s, entry("graphics.texture_filter")), "Texture: Nearest");
    apply(s, entry("graphics.texture_filter"), "xbr");
    EXPECT_EQ(value_label(s, entry("graphics.texture_filter")), "xBR");
    EXPECT_EQ(value_label(s, entry("audio.volume")), "100");
    apply(s, entry("system.fast_forward_speed"), "unlimited");
    EXPECT_EQ(value_label(s, entry("system.fast_forward_speed")), "Max");
    apply(s, entry("system.fast_forward_speed"), "4x");
    EXPECT_EQ(value_label(s, entry("system.fast_forward_speed")), "4X");
}

TEST(IngameList, SwitchesFlipEitherWay) {
    Settings s = defaults();
    const Entry& e = entry("graphics.true_color");
    EXPECT_EQ(stepped_value(s, e, 1), "true");
    EXPECT_EQ(stepped_value(s, e, -1), "true");
    apply(s, e, "true");
    EXPECT_EQ(stepped_value(s, e, 1), "false");
}

TEST(IngameList, NumbersStopAtTheirLimits) {
    Settings s = defaults();
    const Entry& scale = entry("graphics.internal_res");
    EXPECT_EQ(stepped_value(s, scale, -1), "");  // already 1
    EXPECT_EQ(stepped_value(s, scale, 1), "2");
    apply(s, scale, "8");
    EXPECT_EQ(stepped_value(s, scale, 1), "");
    EXPECT_EQ(stepped_value(s, scale, -1), "7");

    const Entry& volume = entry("audio.volume");
    EXPECT_EQ(stepped_value(s, volume, 1), "");
    EXPECT_EQ(stepped_value(s, volume, -1), "90");
    apply(s, volume, "5");
    EXPECT_EQ(stepped_value(s, volume, -1), "0");
}

TEST(IngameList, ChoicesWrapAround) {
    Settings s = defaults();
    const Entry& filter = entry("graphics.scale_mode");
    EXPECT_EQ(stepped_value(s, filter, 1), "integer");
    EXPECT_EQ(stepped_value(s, filter, -1), "stretch");
    apply(s, filter, "stretch");
    EXPECT_EQ(stepped_value(s, filter, 1), "fit");
}

TEST(IngameList, ComparisonPresetOffersOnlyEnhancedAndRemaster) {
    Settings s = defaults();
    const Entry& e = entry("graphics.comparison_preset");
    EXPECT_EQ(value_label(s, e), "Enhanced");
    EXPECT_EQ(stepped_value(s, e, 1), "remaster");
    apply(s, e, "remaster");
    EXPECT_EQ(stepped_value(s, e, 1), "enhanced");
    EXPECT_EQ(stepped_value(s, e, -1), "enhanced");
}

TEST(IngameList, PresetCyclesThroughTheNamedPresetsAndNeverSelectsCustom) {
    Settings s = defaults();
    const Entry& e = entry("graphics.preset");
    EXPECT_EQ(stepped_value(s, e, 1), "enhanced");
    EXPECT_EQ(stepped_value(s, e, -1), "remaster");
    apply(s, e, "remaster");
    EXPECT_EQ(stepped_value(s, e, 1), "original");
    // Changing an option away from a preset makes it Custom; stepping then goes on from the preset it was based on.
    apply(s, e, "enhanced");
    apply(s, entry("graphics.internal_res"), "7");
    ASSERT_EQ(value_label(s, e), "Custom");
    EXPECT_EQ(stepped_value(s, e, 1), "remaster");
    EXPECT_EQ(stepped_value(s, e, -1), "original");
}

TEST(IngameText, EncodesDigitsAndLettersAsTheFontCodes) {
    const std::vector<uint8_t> text = encode_game_text("Items 10");
    const std::vector<uint8_t> expected = {0x16, 0x28 + 19, 0x28 + 4, 0x28 + 12, 0x28 + 18, 1, 1, 5, 4, 0};
    // I t e m s <space> 1 0 + terminator
    EXPECT_EQ(text, expected);
}

TEST(IngameText, SymbolsUseTheTwoByteRange) {
    EXPECT_EQ(encode_game_text(":"), (std::vector<uint8_t>{1, 7, 0}));
    EXPECT_EQ(encode_game_text("."), (std::vector<uint8_t>{1, 5, 0}));
    EXPECT_EQ(encode_game_text("-"), (std::vector<uint8_t>{1, 0x1A, 0}));
    EXPECT_EQ(encode_game_text("~"), (std::vector<uint8_t>{1, 1, 0}));  // unknown characters become spaces
    EXPECT_EQ(encode_game_text(""), (std::vector<uint8_t>{0}));
}
