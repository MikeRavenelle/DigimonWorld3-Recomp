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
        if (e.kind == Kind::Setting && e.info->id == id) return e;
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
    for (const Entry& e : entries())
        if (e.kind == Kind::Setting) listed.insert(e.info->id);
    EXPECT_EQ(listed, expected);
}

TEST(IngameList, PresetComesFirstAndNothingNeedsARestart) {
    ASSERT_FALSE(entries().empty());
    EXPECT_EQ(entries().front().info->id, "graphics.preset");
    for (const Entry& e : entries()) {
        if (e.kind != Kind::Setting) continue;
        EXPECT_FALSE(e.info->is_restart()) << e.info->id;
        EXPECT_FALSE(e.info->hidden) << e.info->id;
    }
}

TEST(IngameList, EveryRowFitsTheMenuWindow) {
    // The window holds about 20 characters of the game's proportional font.
    Settings s = defaults();
    for (const Entry& e : entries()) {
        EXPECT_LE(row_text(s, e).size(), 20u) << e.label;
        EXPECT_LE(e.label.size(), 14u) << e.label;
        if (e.kind == Kind::Setting && e.info->type == Type::Choice) {
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

// ---- Display rows ----------------------------------------------------------------------------------------------

namespace {

MonitorList two_monitors() {
    MonitorList list;
    MonitorInfo a{"DP-1", {1920, 1080}, {1920, 1080, 14400}, {{1920, 1080, 14400}, {1920, 1080, 6000}, {1280, 720, 6000}, {640, 480, 6000}}};
    MonitorInfo b{"LG ULTRAGEAR 27GL850", {2560, 1440}, {2560, 1440, 6000}, {{2560, 1440, 6000}}};
    list.monitors = {a, b};
    return list;
}

const Entry& resolution_entry() {
    for (const Entry& e : entries())
        if (e.kind == Kind::Resolution) return e;
    ADD_FAILURE() << "no resolution row";
    return entries().front();
}

void change(Settings& s, const std::vector<std::pair<std::string, std::string>>& changes) {
    for (const auto& [id, value] : changes) {
        std::string error;
        ASSERT_TRUE(set_text(s, *find_setting(id), value, error)) << error;
    }
}

}  // namespace

TEST(IngameDisplay, RowsFollowTheGraphicsPageInOrder) {
    std::vector<std::string> labels;
    for (const Entry& e : entries())
        if (e.page == Page::Graphics) labels.push_back(e.label);
    const auto at = [&](const std::string& l) { return std::find(labels.begin(), labels.end(), l) - labels.begin(); };
    EXPECT_LT(at("Scaling"), at("Display"));
    EXPECT_EQ(at("Monitor"), at("Display") + 1);
    EXPECT_EQ(at("Res"), at("Monitor") + 1);
    EXPECT_EQ(at("VSync"), at("Res") + 1);
    EXPECT_LT(at("VSync"), at("Pacing"));
    EXPECT_LT(at("Pacing"), at("Show FPS"));
    EXPECT_LT(at("Show FPS"), at("Gamma"));
    EXPECT_LT(at("Gamma"), at("Bright"));
    EXPECT_EQ(std::count(labels.begin(), labels.end(), "Win Width"), 0);
    EXPECT_EQ(std::count(labels.begin(), labels.end(), "Fullscreen"), 0);
}

TEST(IngameDisplay, MonitorRowShowsAutoANameThatFitsOrItsNumber) {
    const MonitorList list = two_monitors();
    RowContext context;
    context.monitors = &list;
    Settings s = defaults();
    const Entry& monitor = entry("graphics.monitor");
    EXPECT_EQ(value_label(s, monitor, context), "Auto");
    apply(s, monitor, "1");
    EXPECT_EQ(value_label(s, monitor, context), "DP-1");
    apply(s, monitor, "2");
    EXPECT_EQ(value_label(s, monitor, context), "2");  // the name is too long for the row
    EXPECT_LE(row_text(s, monitor, context).size(), 20u);
    EXPECT_EQ(value_label(s, monitor), "2");           // no monitor list: the number
}

TEST(IngameDisplay, MonitorRowStopsAtTheLastMonitor) {
    const MonitorList list = two_monitors();
    RowContext context;
    context.monitors = &list;
    Settings s = defaults();
    const Entry& monitor = entry("graphics.monitor");
    EXPECT_EQ(stepped_value(s, monitor, 1, context), "1");
    apply(s, monitor, "2");
    EXPECT_EQ(stepped_value(s, monitor, 1, context), "");
    EXPECT_EQ(stepped_value(s, monitor, -1, context), "1");
    apply(s, monitor, "0");
    EXPECT_EQ(stepped_value(s, monitor, -1, context), "");
}

TEST(IngameDisplay, WindowedResolutionStepsThroughSizesThatFit) {
    const MonitorList list = two_monitors();
    RowContext context;
    context.monitors = &list;
    Settings s = defaults();
    apply(s, entry("graphics.monitor"), "1");
    EXPECT_EQ(value_label(s, resolution_entry(), context), "1280x960");
    const auto up = resolution_changes(s, context, 1);
    ASSERT_EQ(up.size(), 2u);
    EXPECT_EQ(up[0].first, "graphics.window_width");
    EXPECT_EQ(up[0].second, "1440");
    EXPECT_EQ(up[1].second, "900");
    change(s, up);
    EXPECT_EQ(value_label(s, resolution_entry(), context), "1440x900");
    const auto down = resolution_changes(s, context, -1);
    ASSERT_EQ(down.size(), 2u);
    EXPECT_EQ(down[0].second, "1280");
}

TEST(IngameDisplay, WindowedResolutionStopsAtBothEnds) {
    MonitorList list;
    list.monitors = {{"A", {1024, 768}, {1024, 768, 6000}, {}}};
    RowContext context;
    context.monitors = &list;
    Settings s = defaults();
    change(s, {{"graphics.window_width", "1024"}, {"graphics.window_height", "768"}});
    EXPECT_TRUE(resolution_changes(s, context, 1).empty());
    change(s, {{"graphics.window_width", "640"}, {"graphics.window_height", "480"}});
    EXPECT_TRUE(resolution_changes(s, context, -1).empty());
}

TEST(IngameDisplay, BorderlessUsesTheDesktopAndDoesNotStep) {
    const MonitorList list = two_monitors();
    RowContext context;
    context.monitors = &list;
    Settings s = defaults();
    apply(s, entry("graphics.display_mode"), "borderless");
    EXPECT_EQ(value_label(s, resolution_entry(), context), "Desktop");
    EXPECT_TRUE(resolution_changes(s, context, 1).empty());
}

TEST(IngameDisplay, ExclusiveResolutionListsTheMonitorsOwnModes) {
    const MonitorList list = two_monitors();
    RowContext context;
    context.monitors = &list;
    Settings s = defaults();
    apply(s, entry("graphics.display_mode"), "exclusive");
    apply(s, entry("graphics.monitor"), "1");
    EXPECT_EQ(value_label(s, resolution_entry(), context), "1920x1080 144Hz");  // the desktop mode until one is chosen
    EXPECT_EQ(row_text(s, resolution_entry(), context), "1920x1080 144Hz");  // too wide for a label
    EXPECT_TRUE(resolution_changes(s, context, 1).empty());                   // already the last mode
    const auto down = resolution_changes(s, context, -1);
    ASSERT_EQ(down.size(), 3u);
    EXPECT_EQ(down[0].second, "1920");
    EXPECT_EQ(down[1].second, "1080");
    EXPECT_EQ(down[2].second, "6000");
    change(s, down);
    EXPECT_EQ(value_label(s, resolution_entry(), context), "1920x1080 60Hz");
}

TEST(IngameDisplay, ExclusiveWithoutMonitorInformationReadsDesktop) {
    Settings s = defaults();
    apply(s, entry("graphics.display_mode"), "exclusive");
    EXPECT_EQ(value_label(s, resolution_entry()), "Desktop");
    EXPECT_TRUE(resolution_changes(s, {}, 1).empty());
}

TEST(IngameDisplay, AdaptiveVsyncIsOnlyOfferedWhereItWorks) {
    Settings s = defaults();
    const Entry& vsync = entry("graphics.vsync");
    EXPECT_EQ(stepped_value(s, vsync, 1), "off");  // on wraps past the missing adaptive
    RowContext adaptive;
    adaptive.adaptive_vsync = true;
    EXPECT_EQ(stepped_value(s, vsync, 1, adaptive), "adaptive");
    apply(s, vsync, "adaptive");
    EXPECT_EQ(value_label(s, vsync), "Adaptive");
    EXPECT_EQ(stepped_value(s, vsync, 1), "on");  // a saved adaptive on a path without it steps back to the list
}

TEST(IngameDisplay, GammaAndBrightnessReadAsValuesAndStep) {
    Settings s = defaults();
    const Entry& gamma = entry("graphics.gamma");
    const Entry& bright = entry("graphics.brightness");
    EXPECT_EQ(row_text(s, gamma), "Gamma: 1.00");
    EXPECT_EQ(stepped_value(s, gamma, 1), "105");
    apply(s, gamma, "105");
    EXPECT_EQ(value_label(s, gamma), "1.05");
    apply(s, gamma, "80");
    EXPECT_EQ(value_label(s, gamma), "0.80");
    EXPECT_EQ(stepped_value(s, gamma, -1), "");
    apply(s, gamma, "140");
    EXPECT_EQ(stepped_value(s, gamma, 1), "");
    EXPECT_EQ(value_label(s, bright), "0");
    apply(s, bright, "10");
    EXPECT_EQ(value_label(s, bright), "+10");
    apply(s, bright, "-20");
    EXPECT_EQ(value_label(s, bright), "-20");
}

TEST(IngameDisplay, EveryDisplayRowFitsWhateverTheChoice) {
    const MonitorList list = two_monitors();
    RowContext context;
    context.monitors = &list;
    context.adaptive_vsync = true;
    Settings s = defaults();
    for (const char* mode : {"windowed", "borderless", "exclusive"}) {
        apply(s, entry("graphics.display_mode"), mode);
        for (const char* monitor : {"0", "1", "2"}) {
            apply(s, entry("graphics.monitor"), monitor);
            for (const Entry& e : entries())
                if (e.page == Page::Graphics) EXPECT_LE(row_text(s, e, context).size(), 20u) << e.label << " " << mode << " " << monitor;
        }
    }
}

TEST(IngameList, HdrRowsReadNotAvailableWithoutAnHdrDisplay) {
    Settings s = defaults();
    RowContext none;
    for (const char* id : {"graphics.hdr", "graphics.hdr_peak_nits", "graphics.hdr_paper_white"}) {
        EXPECT_EQ(value_label(s, entry(id), none), "N/A") << id;
        EXPECT_EQ(stepped_value(s, entry(id), 1, none), "") << id;
        EXPECT_EQ(stepped_value(s, entry(id), -1, none), "") << id;
    }
    EXPECT_EQ(row_text(s, entry("graphics.hdr"), none), "HDR: N/A");
}

TEST(IngameList, HdrRowsWorkOnAnHdrDisplay) {
    Settings s = defaults();
    RowContext hdr;
    hdr.hdr_available = true;
    EXPECT_EQ(row_text(s, entry("graphics.hdr"), hdr), "HDR: Off");
    EXPECT_EQ(stepped_value(s, entry("graphics.hdr"), 1, hdr), "true");
    EXPECT_EQ(row_text(s, entry("graphics.hdr_peak_nits"), hdr), "HDR Peak: 1000");
    EXPECT_EQ(stepped_value(s, entry("graphics.hdr_peak_nits"), 1, hdr), "1050");
    EXPECT_EQ(stepped_value(s, entry("graphics.hdr_paper_white"), -1, hdr), "193");
    apply(s, entry("graphics.hdr_peak_nits"), "1500");
    EXPECT_EQ(stepped_value(s, entry("graphics.hdr_peak_nits"), 1, hdr), "");  // at the limit
    apply(s, entry("graphics.hdr_paper_white"), "300");
    EXPECT_EQ(stepped_value(s, entry("graphics.hdr_paper_white"), 1, hdr), "");
}

TEST(IngameList, WidescreenRowCyclesTheAspects) {
    Settings s = defaults();
    const Entry& e = entry("graphics.widescreen");
    EXPECT_EQ(row_text(s, e), "Widescreen: Off");
    apply(s, e, stepped_value(s, e, 1));
    EXPECT_EQ(row_text(s, e), "Widescreen: 16:9");
    apply(s, e, stepped_value(s, e, 1));
    EXPECT_EQ(value_label(s, e), "16:10");
    apply(s, e, "21:9");
    apply(s, e, stepped_value(s, e, 1));
    EXPECT_EQ(value_label(s, e), "Off");
}
