#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

#include "controller.h"
#include "fastforward.h"
#include "gameplay_scaling.h"
#include "timestretch.h"

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
    int calls = 0;
    bool result = true;
    bool apply(const Settings&, std::span<const SettingInfo* const> changed) override {
        ++calls;
        for (const SettingInfo* i : changed) applied.emplace_back(i->id);
        return result;
    }
};

}  // namespace

TEST(Settings, DefaultsAreTheOriginalPicture) {
    const Settings s = defaults();
    EXPECT_EQ(s.graphics.preset, kPresetOriginal);
    EXPECT_EQ(bundle_of(s), preset(kPresetOriginal).graphics);
    EXPECT_EQ(s.graphics.internal_res, 1);
    EXPECT_FALSE(s.graphics.true_color);
    EXPECT_EQ(s.graphics.vsync, 1);
    EXPECT_EQ(s.graphics.display_mode, 0);
}

TEST(Settings, MetadataIsCompleteAndConsistent) {
    for (const SettingInfo& i : all_settings()) {
        EXPECT_FALSE(i.label.empty()) << i.id;
        EXPECT_EQ(find_setting(i.id), &i);
        EXPECT_NE(i.id.find(group_name(i.group)), std::string_view::npos) << i.id;
        if (i.type == Type::Choice) EXPECT_FALSE(i.choices.empty()) << i.id;
    }
}

TEST(Settings, TomlRoundTrip) {
    Settings s = defaults();
    EXPECT_TRUE(set(s, "graphics.preset", "remaster"));
    EXPECT_TRUE(set(s, "graphics.internal_res", "6"));
    EXPECT_TRUE(set(s, "graphics.renderer", "software"));
    EXPECT_TRUE(set(s, "graphics.window_width", "1600"));
    EXPECT_TRUE(set(s, "audio.volume", "40"));
    EXPECT_TRUE(set(s, "audio.device", "Speakers \"A\""));
    EXPECT_TRUE(set(s, "system.memcard1", "C:\\cards\\one.mcd"));
    EXPECT_TRUE(set(s, "system.fast_forward_speed", "unlimited"));
    EXPECT_TRUE(set(s, "graphics.comparison_preset", "remaster"));
    LoadReport report;
    const Settings back = settings_from_toml(settings_to_toml(s), report);
    EXPECT_TRUE(report.messages.empty());
    EXPECT_EQ(back, s);
    EXPECT_EQ(back.graphics.preset, kPresetCustom);
    EXPECT_EQ(back.graphics.preset_base, kPresetRemaster);
}

TEST(Settings, FileRoundTripAndMissingFile) {
    const auto dir = std::filesystem::temp_directory_path() / "dw3_settings_test";
    std::filesystem::remove_all(dir);
    const auto path = dir / "sub" / "settings.toml";
    LoadReport r1;
    EXPECT_EQ(load_settings_file(path, r1), defaults());
    EXPECT_FALSE(r1.file_found);
    Settings s = defaults();
    set(s, "audio.mute", "true");
    std::string err;
    ASSERT_TRUE(save_settings_file(path, s, err)) << err;
    LoadReport r2;
    EXPECT_EQ(load_settings_file(path, r2), s);
    EXPECT_TRUE(r2.file_found);
    std::filesystem::remove_all(dir);
}

TEST(Settings, MissingKeysTakeDefaultsAndUnknownKeysAreIgnored) {
    LoadReport report;
    const Settings s = settings_from_toml("[graphics]\ninternal_res = 3\nsome_future_key = 1\n[extra]\nx = 2\n", report);
    EXPECT_TRUE(report.messages.empty());
    EXPECT_EQ(s.graphics.internal_res, 3);
    EXPECT_EQ(s.graphics.window_width, 1280);
    EXPECT_FALSE(s.audio.mute);
}

TEST(Settings, InvalidValuesFallBackWithAMessage) {
    LoadReport report;
    const Settings s = settings_from_toml(
        "[graphics]\ninternal_res = 99\ntexture_filter = \"sharp\"\ntrue_color = \"yes\"\nvsync = false\n"
        "[audio]\nvolume = -5\n",
        report);
    EXPECT_EQ(report.messages.size(), 4u);
    EXPECT_EQ(s.graphics.internal_res, 1);
    EXPECT_EQ(s.graphics.texture_filter, 0);
    EXPECT_FALSE(s.graphics.true_color);
    EXPECT_EQ(s.audio.volume, 100);
    EXPECT_EQ(s.graphics.vsync, 0);  // valid values next to bad ones still load
}

TEST(Settings, UnparsableFileGivesDefaults) {
    LoadReport report;
    EXPECT_EQ(settings_from_toml("[graphics\nbroken = =", report), defaults());
    EXPECT_EQ(report.messages.size(), 1u);
}

TEST(Settings, SetTextRejectsBadInput) {
    Settings s = defaults();
    std::string err;
    EXPECT_FALSE(set_text(s, *find_setting("graphics.internal_res"), "0", err));
    EXPECT_FALSE(set_text(s, *find_setting("graphics.internal_res"), "four", err));
    EXPECT_FALSE(set_text(s, *find_setting("graphics.scale_mode"), "zoom", err));
    EXPECT_FALSE(set_text(s, *find_setting("audio.mute"), "maybe", err));
    EXPECT_EQ(s, defaults());
}

TEST(Presets, EachPresetSetsItsBundle) {
    for (int p = 0; p < kNamedPresets; ++p) {
        Settings s = defaults();
        ASSERT_TRUE(set(s, "graphics.preset", preset(p).name));
        EXPECT_EQ(bundle_of(s), preset(p).graphics);
        EXPECT_EQ(s.graphics.preset, p);
    }
    EXPECT_EQ(preset(kPresetOriginal).graphics, (GraphicsBundle{1, false, 0, false, 0, 0, 0, 0}));
    EXPECT_GT(preset(kPresetEnhanced).graphics.internal_res, 1);
    EXPECT_GE(preset(kPresetRemaster).graphics.internal_res, preset(kPresetEnhanced).graphics.internal_res);
}

TEST(Presets, MembersAreAllLiveSettings) {
    for (std::string_view id : preset_member_ids()) {
        const SettingInfo* i = find_setting(id);
        ASSERT_NE(i, nullptr) << id;
        EXPECT_EQ(i->apply, Apply::Live) << id;
        EXPECT_EQ(i->group, Group::Graphics) << id;
    }
}

TEST(Presets, TweakingBecomesCustomAndKeepsTheBase) {
    Settings s = defaults();
    set(s, "graphics.preset", "enhanced");
    set(s, "graphics.internal_res", "2");
    EXPECT_EQ(s.graphics.preset, kPresetCustom);
    EXPECT_EQ(s.graphics.preset_base, kPresetEnhanced);
    set(s, "graphics.internal_res", "4");  // back onto the bundle
    EXPECT_EQ(s.graphics.preset, kPresetEnhanced);
}

TEST(Presets, FileCanNamePresetAndOverrideMembers) {
    LoadReport report;
    const Settings s = settings_from_toml("[graphics]\npreset = \"remaster\"\ninternal_res = 2\n", report);
    EXPECT_EQ(s.graphics.internal_res, 2);
    EXPECT_TRUE(s.graphics.true_color);
    EXPECT_EQ(s.graphics.preset, kPresetCustom);
    EXPECT_EQ(s.graphics.preset_base, kPresetRemaster);
}

TEST(Controller, FlagsOverrideTheFileButAreNotStored) {
    LoadReport report;
    Settings stored = settings_from_toml("[graphics]\ninternal_res = 2\n", report);
    Settings active = stored;
    set(active, "graphics.internal_res", "8");  // as a command-line flag would
    set(active, "audio.mute", "true");
    RecordingSink sink;
    const auto path = std::filesystem::temp_directory_path() / "dw3_controller_test.toml";
    SettingsController c(stored, active, &sink, path);
    EXPECT_EQ(c.active().graphics.internal_res, 8);
    EXPECT_EQ(c.stored().graphics.internal_res, 2);
    // A user change to one setting leaves the flag-set values of others out of the saved file.
    ASSERT_TRUE(c.set("graphics.true_color", "true").ok);
    std::string err;
    ASSERT_TRUE(c.save(err)) << err;
    LoadReport r2;
    const Settings saved = load_settings_file(path, r2);
    EXPECT_EQ(saved.graphics.internal_res, 2);
    EXPECT_FALSE(saved.audio.mute);
    EXPECT_TRUE(saved.graphics.true_color);
    std::filesystem::remove(path);
}

TEST(Controller, LiveSettingsApplyAndRestartOnesAreMarked) {
    RecordingSink sink;
    SettingsController c(defaults(), defaults(), &sink, {});
    ChangeResult r = c.set("graphics.true_color", "true");
    EXPECT_TRUE(r.ok);
    EXPECT_FALSE(r.restart_needed);
    EXPECT_NE(std::find(sink.applied.begin(), sink.applied.end(), "graphics.true_color"), sink.applied.end());
    sink.applied.clear();
    r = c.set("graphics.renderer", "software");
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.restart_needed);
    EXPECT_TRUE(sink.applied.empty());
    EXPECT_EQ(c.pending_restart(), std::vector<std::string>{"graphics.renderer"});
    c.set("graphics.renderer", "auto");
    EXPECT_TRUE(c.pending_restart().empty());
    EXPECT_FALSE(c.set("graphics.internal_res", "40").ok);
    EXPECT_FALSE(c.set("nonsense", "1").ok);
}

TEST(Controller, FailedLiveApplyCountsAsRestartNeeded) {
    RecordingSink sink;
    sink.result = false;
    SettingsController c(defaults(), defaults(), &sink, {});
    EXPECT_TRUE(c.set("graphics.internal_res", "8").restart_needed);
    EXPECT_FALSE(c.pending_restart().empty());  // the setting and the preset derived from it
}

TEST(Controller, PresetChangesApplyAsOneBatch) {
    RecordingSink sink;
    SettingsController c(defaults(), defaults(), &sink, {});
    EXPECT_TRUE(c.select_preset(kPresetEnhanced).ok);
    EXPECT_EQ(sink.calls, 1);
    EXPECT_GE(sink.applied.size(), 4u);
    EXPECT_EQ(c.active().graphics.preset, kPresetEnhanced);
}

TEST(Controller, ComparisonToggleRestoresExactlyThePreviousSettings) {
    RecordingSink sink;
    Settings s = defaults();
    set(s, "graphics.preset", "remaster");
    set(s, "graphics.internal_res", "5");
    set(s, "graphics.scale_mode", "integer");
    set(s, "graphics.window_width", "1000");
    set(s, "audio.volume", "30");
    SettingsController c(s, s, &sink, {});
    EXPECT_EQ(c.toggle_comparison(), "Original");
    EXPECT_EQ(c.active().graphics.preset, kPresetOriginal);
    EXPECT_EQ(c.active().graphics.scale_mode, s.graphics.scale_mode);  // display preferences are not part of a preset
    EXPECT_EQ(c.stored(), s);                                          // the file's view is untouched
    EXPECT_EQ(c.toggle_comparison(), "Custom (Remaster)");
    EXPECT_EQ(c.active(), s);
    EXPECT_TRUE(c.pending_restart().empty());
}

TEST(Controller, ComparisonFromOriginalGoesToTheChosenPreset) {
    RecordingSink sink;
    Settings s = defaults();
    set(s, "graphics.comparison_preset", "remaster");
    SettingsController c(s, s, &sink, {});
    EXPECT_EQ(c.toggle_comparison(), "Remaster");
    EXPECT_EQ(bundle_of(c.active()), preset(kPresetRemaster).graphics);
    EXPECT_EQ(c.toggle_comparison(), "Original");
    EXPECT_EQ(c.toggle_comparison(), "Remaster");
}

TEST(Controller, ComparisonNeverTouchesRestartSettings) {
    RecordingSink sink;
    Settings s = defaults();
    set(s, "graphics.preset", "enhanced");
    SettingsController c(s, s, &sink, {});
    c.toggle_comparison();
    for (const std::string& id : sink.applied) EXPECT_EQ(find_setting(id)->apply, Apply::Live) << id;
    EXPECT_TRUE(c.pending_restart().empty());
}

TEST(Controller, UserChangeDuringComparisonDropsTheStash) {
    RecordingSink sink;
    Settings s = defaults();
    set(s, "graphics.preset", "enhanced");
    SettingsController c(s, s, &sink, {});
    c.toggle_comparison();
    ASSERT_TRUE(c.set("graphics.internal_res", "2").ok);
    EXPECT_FALSE(c.showing_original_for_comparison());
    c.toggle_comparison();  // now a plain switch to Original from the tweaked state
    EXPECT_EQ(c.active().graphics.preset, kPresetOriginal);
}

TEST(FastForward, Multipliers) {
    EXPECT_EQ(fast_forward_multiplier(0), 2.0);
    EXPECT_EQ(fast_forward_multiplier(1), 4.0);
    EXPECT_EQ(fast_forward_multiplier(2), 0.0);
}

TEST(FastForward, HoldAndLatchCombine) {
    FastForwardInput in;
    EXPECT_FALSE(in.active());
    in.set_held(true);
    EXPECT_TRUE(in.active());
    in.set_held(false);
    EXPECT_FALSE(in.active());
    in.toggle_latch();
    EXPECT_TRUE(in.active());
    in.set_held(true);
    in.toggle_latch();
    EXPECT_TRUE(in.active());  // still held
    in.set_held(false);
    EXPECT_FALSE(in.active());
}

TEST(Pacer, RealTimeWaitsForEmulatedTime) {
    Pacer p;
    p.set_speed(1.0, 0, 0);
    const int64_t frame = static_cast<int64_t>(Pacer::kCpuHz / 60.0);
    // One frame of emulated time, 1 ms of wall time used: wait for the rest of the 16.67 ms.
    const uint64_t wait = p.delay_ns(1000000, frame);
    EXPECT_NEAR(static_cast<double>(wait), 1e9 / 60.0 - 1e6, 2e3);
    EXPECT_EQ(p.delay_ns(20000000, frame), 0u);  // already late
}

TEST(Pacer, SpeedDividesTheWait) {
    Pacer p;
    p.set_speed(4.0, 0, 0);
    const int64_t frame = static_cast<int64_t>(Pacer::kCpuHz / 60.0);
    EXPECT_NEAR(static_cast<double>(p.delay_ns(0, frame)), 1e9 / 60.0 / 4.0, 2e3);
}

TEST(Pacer, UnlimitedNeverWaits) {
    Pacer p;
    p.set_speed(0.0, 0, 0);
    EXPECT_EQ(p.delay_ns(0, 100000000), 0u);
}

TEST(Pacer, ChangingSpeedReanchorsWithoutABurstOrStall) {
    Pacer p;
    p.set_speed(1.0, 0, 0);
    const int64_t second = static_cast<int64_t>(Pacer::kCpuHz);
    // After 10 emulated seconds run unlimited in 1 s of wall time, back to real time: no sleep to "catch up".
    p.set_speed(0.0, 0, 0);
    p.set_speed(1.0, 1000000000, 10 * second);
    EXPECT_EQ(p.delay_ns(1000000000, 10 * second), 0u);
    EXPECT_NEAR(static_cast<double>(p.delay_ns(1000000000, 11 * second)), 1e9, 1e4);
}

TEST(Pacer, LongStallDoesNotCauseASprint) {
    Pacer p;
    p.set_speed(1.0, 0, 0);
    const int64_t second = static_cast<int64_t>(Pacer::kCpuHz);
    EXPECT_EQ(p.delay_ns(5000000000ull, second), 0u);  // 4 s late: the anchor moves up
    EXPECT_GT(p.delay_ns(5000000000ull, 2 * second), 900000000u);
}

TEST(Presets, PostProcessingPerPreset) {
    const auto& original = preset(kPresetOriginal).graphics;
    EXPECT_EQ(original.antialiasing, 0);
    EXPECT_EQ(original.sharpening, 0);
    EXPECT_EQ(original.color_grade, 0);
    EXPECT_EQ(original.output_filter, 0);
    const auto& remaster = preset(kPresetRemaster).graphics;
    EXPECT_GT(remaster.sharpening, preset(kPresetEnhanced).graphics.sharpening);
    EXPECT_EQ(remaster.color_grade, 1);
    EXPECT_EQ(remaster.antialiasing, 0);
}

TEST(Presets, CrtIsNotPartOfAnyPreset) {
    for (std::string_view id : preset_member_ids()) EXPECT_NE(id, "graphics.crt");
    Settings s = defaults();
    EXPECT_EQ(s.graphics.crt, 0);
    ASSERT_TRUE(set(s, "graphics.crt", "crt"));
    EXPECT_EQ(s.graphics.preset, kPresetOriginal);  // switching the look on leaves the preset alone
    ASSERT_TRUE(set(s, "graphics.preset", "remaster"));
    EXPECT_EQ(s.graphics.crt, 2);  // and so does choosing a preset
}

TEST(Settings, PostProcessingRoundTripsThroughToml) {
    Settings s = defaults();
    ASSERT_TRUE(set(s, "graphics.output_filter", "fsr"));
    ASSERT_TRUE(set(s, "graphics.antialiasing", "smaa"));
    ASSERT_TRUE(set(s, "graphics.sharpening", "high"));
    ASSERT_TRUE(set(s, "graphics.color_grade", "warm"));
    LoadReport report;
    EXPECT_EQ(settings_from_toml(settings_to_toml(s), report), s);
    EXPECT_FALSE(set(s, "graphics.sharpening", "extreme"));
}

TEST(GameplaySettings, DefaultsAreTheOriginalGame) {
    const Settings s = defaults();
    EXPECT_EQ(get_text(s, *find_setting("gameplay.encounters")), "normal");
    EXPECT_EQ(get_text(s, *find_setting("gameplay.game_speed")), "1x");
    EXPECT_EQ(get_text(s, *find_setting("gameplay.god_mode")), "false");
    EXPECT_EQ(get_text(s, *find_setting("gameplay.xp_multiplier")), "1x");
    EXPECT_EQ(get_text(s, *find_setting("gameplay.text_speed")), "normal");
}

TEST(GameplaySettings, ValuesRoundTripThroughToml) {
    Settings s = defaults();
    EXPECT_TRUE(set(s, "gameplay.encounters", "off"));
    EXPECT_TRUE(set(s, "gameplay.game_speed", "3x"));
    EXPECT_TRUE(set(s, "gameplay.god_mode", "true"));
    EXPECT_TRUE(set(s, "gameplay.xp_multiplier", "5x"));
    EXPECT_TRUE(set(s, "gameplay.text_speed", "instant"));
    EXPECT_FALSE(set(s, "gameplay.text_speed", "slow"));
    EXPECT_FALSE(set(s, "gameplay.game_speed", "9x"));
    EXPECT_FALSE(set(s, "gameplay.xp_multiplier", "4x"));
    LoadReport report;
    EXPECT_EQ(settings_from_toml(settings_to_toml(s), report), s);
}

TEST(Experience, MultiplierFollowsTheChoice) {
    EXPECT_EQ(xp_multiplier(0), 1);
    EXPECT_EQ(xp_multiplier(1), 2);
    EXPECT_EQ(xp_multiplier(2), 3);
    EXPECT_EQ(xp_multiplier(3), 5);
    EXPECT_EQ(xp_multiplier(4), 10);
    EXPECT_EQ(xp_multiplier(-1), 1);
    EXPECT_EQ(xp_multiplier(9), 10);
}

TEST(Experience, AwardScalesAndStopsAtTheFieldMaximum) {
    EXPECT_EQ(scaled_experience(6, 1), 6u);
    EXPECT_EQ(scaled_experience(6, 2), 12u);
    EXPECT_EQ(scaled_experience(0, 10), 0u);
    EXPECT_EQ(scaled_experience(100000, 10), kMaxExperience);
    EXPECT_EQ(scaled_experience(0xFFFFFFFFu, 10), kMaxExperience);
    EXPECT_EQ(scaled_experience(6, 0), 6u);
}

TEST(TextSpeed, ChoiceMapsToSpeed) {
    EXPECT_EQ(text_speed(0), TextSpeed::Normal);
    EXPECT_EQ(text_speed(1), TextSpeed::Fast);
    EXPECT_EQ(text_speed(2), TextSpeed::Instant);
    EXPECT_EQ(text_speed(7), TextSpeed::Normal);
}

TEST(TextSpeed, NormalLeavesTheWindowAlone) {
    EXPECT_EQ(text_position_before_tick(TextSpeed::Normal, 5, 55), 5u);
}

TEST(TextSpeed, FastAdvancesAndStopsShortOfTheBoxEnd) {
    EXPECT_EQ(text_position_before_tick(TextSpeed::Fast, 5, 55), 5u + kFastTextChars - 1);
    EXPECT_EQ(text_position_before_tick(TextSpeed::Fast, 52, 55), 53u);
    EXPECT_EQ(text_position_before_tick(TextSpeed::Fast, 54, 55), 54u);
    EXPECT_EQ(text_position_before_tick(TextSpeed::Fast, 0, 0), 0u);
    EXPECT_EQ(text_position_before_tick(TextSpeed::Fast, 60, 55), 60u);
}

TEST(TextSpeed, InstantPrintsTheWholeBox) {
    EXPECT_EQ(text_position_before_tick(TextSpeed::Instant, 5, 55), 55u);
    EXPECT_EQ(text_position_before_tick(TextSpeed::Instant, 60, 55), 60u);
}

TEST(GameSpeed, MultiplierFollowsTheChoice) {
    EXPECT_EQ(game_speed_multiplier(0), 1.0);
    EXPECT_EQ(game_speed_multiplier(1), 2.0);
    EXPECT_EQ(game_speed_multiplier(3), 4.0);
}

namespace {

std::vector<int16_t> sine(std::size_t frames, double hz) {
    std::vector<int16_t> v(frames * 2);
    for (std::size_t i = 0; i < frames; ++i)
        v[i * 2] = v[i * 2 + 1] = static_cast<int16_t>(12000 * std::sin(2 * 3.14159265358979 * hz * static_cast<double>(i) / 44100.0));
    return v;
}

// Rising zero crossings of the left channel per second of output.
double crossings_per_second(const std::vector<int16_t>& v, std::size_t skip_frames) {
    int count = 0;
    for (std::size_t i = skip_frames + 1; i < v.size() / 2; ++i)
        if (v[(i - 1) * 2] < 0 && v[i * 2] >= 0) ++count;
    return count * 44100.0 / static_cast<double>(v.size() / 2 - skip_frames);
}

}  // namespace

class TimeStretchSpeed : public testing::TestWithParam<double> {};

TEST_P(TimeStretchSpeed, ShortensByTheSpeedAndKeepsThePitch) {
    const double speed = GetParam();
    TimeStretch stretch;
    stretch.set_speed(speed);
    const std::vector<int16_t> in = sine(44100 * 2, 440.0);
    std::vector<int16_t> out;
    for (std::size_t at = 0; at < in.size() / 2; at += 735) stretch.process(&in[at * 2], std::min<std::size_t>(735, in.size() / 2 - at), out);
    const double ratio = static_cast<double>(in.size()) / static_cast<double>(out.size());
    EXPECT_NEAR(ratio, speed, speed * 0.1);
    EXPECT_NEAR(crossings_per_second(out, 4000), 440.0, 15.0);
}

INSTANTIATE_TEST_SUITE_P(Speeds, TimeStretchSpeed, testing::Values(2.0, 3.0, 4.0));

TEST(TimeStretch, NormalSpeedPassesThroughUnchanged) {
    TimeStretch stretch;
    const std::vector<int16_t> in = sine(1000, 300.0);
    std::vector<int16_t> out;
    stretch.process(in.data(), 1000, out);
    EXPECT_EQ(out, in);
}

TEST(Settings, HdBackgroundsDefaultOffAndLive) {
    const Settings s = defaults();
    EXPECT_FALSE(s.graphics.hd_backgrounds);
    EXPECT_TRUE(s.graphics.texture_pack.empty());
    const SettingInfo* hd = find_setting("graphics.hd_backgrounds");
    ASSERT_NE(hd, nullptr);
    EXPECT_EQ(hd->type, Type::Bool);
    EXPECT_EQ(hd->label, "HD BG");
    EXPECT_FALSE(hd->is_restart());
    EXPECT_FALSE(hd->hidden);
}

TEST(Settings, TexturePackIsAHiddenStringSetting) {
    const SettingInfo* pack = find_setting("graphics.texture_pack");
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->type, Type::String);
    EXPECT_TRUE(pack->hidden);
    Settings s = defaults();
    ASSERT_TRUE(set(s, "graphics.texture_pack", "general-x4"));
    LoadReport report;
    const Settings back = settings_from_toml(settings_to_toml(s), report);
    EXPECT_EQ(back.graphics.texture_pack, "general-x4");
    EXPECT_EQ(back.graphics.preset, kPresetOriginal) << "the pack name is not part of any preset";
}

TEST(Presets, OnlyRemasterTurnsHdBackgroundsOn) {
    for (int p = 0; p < kNamedPresets; ++p) {
        Settings s = defaults();
        ASSERT_TRUE(set(s, "graphics.preset", preset(p).name));
        EXPECT_EQ(s.graphics.hd_backgrounds, p == kPresetRemaster) << preset(p).name;
    }
    Settings s = defaults();
    ASSERT_TRUE(set(s, "graphics.preset", "remaster"));
    ASSERT_TRUE(set(s, "graphics.hd_backgrounds", "false"));
    EXPECT_EQ(s.graphics.preset, kPresetCustom);
    EXPECT_EQ(s.graphics.preset_base, kPresetRemaster);
}

// ---- Display settings ------------------------------------------------------------------------------------------

namespace {
Settings from(std::string_view toml) {
    LoadReport report;
    const Settings s = settings_from_toml(toml, report);
    EXPECT_TRUE(report.messages.empty()) << (report.messages.empty() ? "" : report.messages.front());
    return s;
}
}  // namespace

TEST(DisplaySettings, OldFullscreenSwitchBecomesTheDisplayMode) {
    EXPECT_EQ(from("[graphics]\nfullscreen = true\n").graphics.display_mode, 1);
    EXPECT_EQ(from("[graphics]\nfullscreen = false\n").graphics.display_mode, 0);
    EXPECT_EQ(from("[graphics]\nwindow_width = 1600\n").graphics.display_mode, 0);
}

TEST(DisplaySettings, ANewDisplayModeBeatsTheOldSwitch) {
    EXPECT_EQ(from("[graphics]\nfullscreen = true\ndisplay_mode = \"windowed\"\n").graphics.display_mode, 0);
    EXPECT_EQ(from("[graphics]\nfullscreen = false\ndisplay_mode = \"exclusive\"\n").graphics.display_mode, 2);
}

TEST(DisplaySettings, OldVsyncSwitchBecomesOffOrOn) {
    EXPECT_EQ(from("[graphics]\nvsync = false\n").graphics.vsync, 0);
    EXPECT_EQ(from("[graphics]\nvsync = true\n").graphics.vsync, 1);
    EXPECT_EQ(from("[graphics]\nvsync = \"adaptive\"\n").graphics.vsync, 2);
}

TEST(DisplaySettings, MigratedFilesAreWrittenInTheNewForm) {
    const std::string text = settings_to_toml(from("[graphics]\nfullscreen = true\nvsync = false\n"));
    EXPECT_NE(text.find("display_mode = \"borderless\""), std::string::npos);
    EXPECT_NE(text.find("vsync = \"off\""), std::string::npos);
    EXPECT_EQ(text.find("fullscreen = "), std::string::npos);
}

TEST(DisplaySettings, EveryDisplaySettingRoundTrips) {
    Settings s = defaults();
    ASSERT_TRUE(set(s, "graphics.display_mode", "exclusive"));
    ASSERT_TRUE(set(s, "graphics.monitor", "2"));
    ASSERT_TRUE(set(s, "graphics.window_width", "1600"));
    ASSERT_TRUE(set(s, "graphics.window_height", "1200"));
    ASSERT_TRUE(set(s, "graphics.fullscreen_width", "1920"));
    ASSERT_TRUE(set(s, "graphics.fullscreen_height", "1080"));
    ASSERT_TRUE(set(s, "graphics.fullscreen_refresh", "14400"));
    ASSERT_TRUE(set(s, "graphics.vsync", "adaptive"));
    ASSERT_TRUE(set(s, "graphics.frame_pacing", "off"));
    ASSERT_TRUE(set(s, "graphics.show_fps", "true"));
    ASSERT_TRUE(set(s, "graphics.gamma", "125"));
    ASSERT_TRUE(set(s, "graphics.brightness", "-15"));
    LoadReport report;
    const Settings back = settings_from_toml(settings_to_toml(s), report);
    EXPECT_TRUE(report.messages.empty());
    EXPECT_EQ(back, s);
}

TEST(DisplaySettings, ValuesAreRangeChecked) {
    Settings s = defaults();
    EXPECT_FALSE(set(s, "graphics.gamma", "79"));
    EXPECT_FALSE(set(s, "graphics.gamma", "141"));
    EXPECT_FALSE(set(s, "graphics.brightness", "51"));
    EXPECT_FALSE(set(s, "graphics.monitor", "-1"));
    EXPECT_FALSE(set(s, "graphics.display_mode", "fullscreen"));
    EXPECT_FALSE(set(s, "graphics.vsync", "maybe"));
    EXPECT_TRUE(set(s, "graphics.gamma", "80"));
    EXPECT_TRUE(set(s, "graphics.gamma", "140"));
}

TEST(DisplaySettings, TheOldFullscreenKeyIsNoLongerASetting) {
    EXPECT_EQ(find_setting("graphics.fullscreen"), nullptr);
    EXPECT_NE(find_setting("graphics.display_mode"), nullptr);
}

TEST(DisplaySettings, PresetsLeaveDisplayAndPictureAdjustmentsAlone) {
    for (const std::string_view id : {"graphics.display_mode", "graphics.monitor", "graphics.window_width", "graphics.window_height",
                                      "graphics.fullscreen_width", "graphics.vsync", "graphics.frame_pacing", "graphics.show_fps",
                                      "graphics.gamma", "graphics.brightness"})
        for (const std::string_view member : preset_member_ids()) EXPECT_NE(id, member);
    Settings s = defaults();
    ASSERT_TRUE(set(s, "graphics.display_mode", "borderless"));
    ASSERT_TRUE(set(s, "graphics.gamma", "120"));
    ASSERT_TRUE(set(s, "graphics.brightness", "10"));
    ASSERT_TRUE(set(s, "graphics.vsync", "off"));
    for (int p = 0; p < kNamedPresets; ++p) {
        ASSERT_TRUE(set(s, "graphics.preset", preset(p).name));
        EXPECT_EQ(s.graphics.preset, p);
        EXPECT_EQ(s.graphics.display_mode, 1);
        EXPECT_EQ(s.graphics.gamma, 120);
        EXPECT_EQ(s.graphics.brightness, 10);
        EXPECT_EQ(s.graphics.vsync, 0);
    }
}

TEST(DisplaySettings, ChangingThemKeepsThePreset) {
    Settings s = defaults();
    ASSERT_TRUE(set(s, "graphics.preset", "enhanced"));
    ASSERT_TRUE(set(s, "graphics.gamma", "110"));
    ASSERT_TRUE(set(s, "graphics.show_fps", "true"));
    EXPECT_EQ(s.graphics.preset, kPresetEnhanced);
}

TEST(DisplaySettings, TheComparisonToggleKeepsTheDisplayChoices) {
    RecordingSink sink;
    Settings s = defaults();
    set(s, "graphics.preset", "remaster");
    set(s, "graphics.display_mode", "borderless");
    set(s, "graphics.gamma", "115");
    SettingsController c(s, s, &sink, {});
    c.toggle_comparison();
    EXPECT_EQ(c.active().graphics.display_mode, 1);
    EXPECT_EQ(c.active().graphics.gamma, 115);
}

TEST(DisplaySettings, AllOfThemApplyLive) {
    for (const std::string_view id : {"graphics.display_mode", "graphics.monitor", "graphics.vsync", "graphics.frame_pacing",
                                      "graphics.show_fps", "graphics.gamma", "graphics.brightness"}) {
        const SettingInfo* info = find_setting(id);
        ASSERT_NE(info, nullptr) << id;
        EXPECT_FALSE(info->is_restart()) << id;
        EXPECT_FALSE(info->hidden) << id;
    }
}
