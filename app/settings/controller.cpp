#include "controller.h"

#include <algorithm>

namespace dw3app {

SettingsController::SettingsController(Settings stored, Settings active, SettingsSink* sink,
                                       std::filesystem::path save_path)
    : stored_(std::move(stored)), active_(std::move(active)), startup_(active_), sink_(sink),
      save_path_(std::move(save_path)) {}

void SettingsController::apply_diff(const Settings& before, ChangeResult& result) {
    std::vector<const SettingInfo*> live;
    for (const SettingInfo* info : diff(before, active_)) {
        auto failed = std::find(failed_live_.begin(), failed_live_.end(), info->id);
        if (failed != failed_live_.end()) failed_live_.erase(failed);
        if (info->is_restart()) result.restart_needed = true;
        else live.push_back(info);
    }
    if (!live.empty() && sink_ && !sink_->apply(active_, live)) {
        for (const SettingInfo* info : live) failed_live_.emplace_back(info->id);
        result.restart_needed = true;
    }
}

ChangeResult SettingsController::set(std::string_view id, std::string_view value) {
    ChangeResult r;
    const SettingInfo* info = find_setting(id);
    if (!info) {
        r.error = "unknown setting " + std::string(id);
        return r;
    }
    Settings next = active_;
    if (!set_text(next, *info, value, r.error)) return r;
    // A user's own change replaces whatever the comparison toggle displaced.
    stash_.reset();
    const Settings before = active_;
    active_ = next;
    std::string ignored;
    set_text(stored_, *info, value, ignored);
    r.ok = true;
    apply_diff(before, r);
    return r;
}

ChangeResult SettingsController::select_preset(int id) {
    return set("graphics.preset", id >= 0 && id < kNamedPresets ? preset(id).name : std::string_view("custom"));
}

std::string SettingsController::describe_preset() const {
    const int p = active_.graphics.preset;
    if (p != kPresetCustom) return std::string(preset_label(p));
    return "Custom (" + std::string(preset_label(active_.graphics.preset_base)) + ")";
}

std::string SettingsController::toggle_comparison() {
    const Settings before = active_;
    ChangeResult ignored;
    if (stash_) {
        // Back to exactly what was displaced.
        active_ = *stash_;
        stash_.reset();
    } else if (active_.graphics.preset == kPresetOriginal) {
        set_text(active_, *find_setting("graphics.preset"), preset(active_.graphics.comparison_preset).name, ignored.error);
    } else {
        stash_ = active_;
        set_text(active_, *find_setting("graphics.preset"), "original", ignored.error);
    }
    apply_diff(before, ignored);
    return describe_preset();
}

std::vector<std::string> SettingsController::pending_restart() const {
    std::vector<std::string> r;
    for (const SettingInfo* info : diff(startup_, active_))
        if (info->is_restart()) r.emplace_back(info->id);
    for (const std::string& id : failed_live_)
        if (std::find(r.begin(), r.end(), id) == r.end()) r.push_back(id);
    return r;
}

bool SettingsController::save(std::string& error) {
    if (save_path_.empty()) return true;
    return save_settings_file(save_path_, stored_, error);
}

}  // namespace dw3app
