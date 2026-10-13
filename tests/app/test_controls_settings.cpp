#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "controller.h"

using namespace dw3app;

namespace {

Settings defaults() {
    LoadReport report;
    return settings_from_toml("", report);
}

bool set(Settings& s, std::string_view id, std::string_view v) {
    std::string err;
    return set_text(s, *find_setting(id), v, err);
}

struct RecordingSink : SettingsSink {
    std::vector<std::string> applied;
    bool apply(const Settings&, std::span<const SettingInfo* const> changed) override {
        for (const SettingInfo* i : changed) applied.emplace_back(i->id);
        return true;
    }
};

}  // namespace

TEST(ControlsSettings, DefaultsKeepTheOriginalControls) {
    const Settings s = defaults();
    EXPECT_EQ(get_text(s, *find_setting("controls.pad")), "auto");
    EXPECT_EQ(get_text(s, *find_setting("controls.left_stick")), "true");
    EXPECT_EQ(get_text(s, *find_setting("controls.deadzone")), "25");
    EXPECT_EQ(get_text(s, *find_setting("controls.prompts")), "auto");
    EXPECT_EQ(get_text(s, *find_setting("controls.cross")), "Z|South");
    EXPECT_EQ(get_text(s, *find_setting("controls.fast_forward")), "Tab|RightStick");
}

TEST(ControlsSettings, EveryActionHasABindingSetting) {
    for (std::size_t i = 0; i < input::kActionCount; ++i) {
        const SettingInfo* info = find_setting("controls." + std::string(input::action_id(input::action_at(i))));
        ASSERT_NE(info, nullptr) << i;
        EXPECT_EQ(info->type, Type::String);
        EXPECT_TRUE(info->hidden);
    }
}

TEST(ControlsSettings, ValuesAreChecked) {
    Settings s = defaults();
    EXPECT_TRUE(set(s, "controls.pad", "2"));
    EXPECT_FALSE(set(s, "controls.pad", "5"));
    EXPECT_TRUE(set(s, "controls.deadzone", "50"));
    EXPECT_FALSE(set(s, "controls.deadzone", "9"));
    EXPECT_FALSE(set(s, "controls.deadzone", "51"));
    EXPECT_TRUE(set(s, "controls.prompts", "PlayStation"));
    EXPECT_FALSE(set(s, "controls.prompts", "sega"));
    EXPECT_TRUE(set(s, "controls.circle", "Q|North"));
    EXPECT_FALSE(set(s, "controls.circle", "Q"));
    EXPECT_FALSE(set(s, "controls.circle", "Q|Cross"));
    EXPECT_EQ(get_text(s, *find_setting("controls.circle")), "Q|North");
}

TEST(ControlsSettings, BindingsRoundTripThroughTheControlsTable) {
    Settings s = defaults();
    EXPECT_TRUE(set(s, "controls.cross", "Left Shift|DPadDown"));
    EXPECT_TRUE(set(s, "controls.compare", "|"));
    EXPECT_TRUE(set(s, "controls.pad", "1"));
    EXPECT_TRUE(set(s, "controls.left_stick", "false"));
    const std::string text = settings_to_toml(s);
    EXPECT_NE(text.find("[controls]"), std::string::npos);
    EXPECT_NE(text.find("cross = \"Left Shift|DPadDown\""), std::string::npos);
    LoadReport report;
    const Settings back = settings_from_toml(text, report);
    EXPECT_TRUE(report.messages.empty());
    EXPECT_EQ(back, s);
}

TEST(ControlsSettings, ABadBindingInTheFileIsReportedAndDefaulted) {
    LoadReport report;
    const Settings s = settings_from_toml("[controls]\ncross = \"nonsense\"\ncircle = \"C|East\"\n", report);
    EXPECT_EQ(report.messages.size(), 1u);
    EXPECT_EQ(get_text(s, *find_setting("controls.cross")), "Z|South");
    EXPECT_EQ(get_text(s, *find_setting("controls.circle")), "C|East");
}

TEST(ControlsSettings, TheyApplyLiveThroughTheController) {
    RecordingSink sink;
    SettingsController c(defaults(), defaults(), &sink, {});
    EXPECT_TRUE(c.set("controls.deadzone", "40").ok);
    EXPECT_TRUE(c.set("controls.cross", "C|South").ok);
    EXPECT_EQ(sink.applied, (std::vector<std::string>{"controls.deadzone", "controls.cross"}));
    EXPECT_TRUE(c.pending_restart().empty());
}
