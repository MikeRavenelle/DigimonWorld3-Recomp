// Changing settings while the program runs. Every runtime change, whether from a hotkey today or a settings screen
// later, goes through SettingsController::set: it validates, records the change, applies it live where the setting
// allows and otherwise remembers that a restart is needed.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "presets.h"
#include "settings.h"

namespace dw3app {

// Where live changes land. The application implements it on top of the presenter, the audio output and the window.
class SettingsSink {
public:
    virtual ~SettingsSink() = default;
    // Applies the changed live settings together (so that a preset costs one renderer rebuild, not five). `now`
    // is the full configuration after the change. Returns false if something could not be applied; the controller
    // then reports a restart as needed for those settings.
    virtual bool apply(const Settings& now, std::span<const SettingInfo* const> changed) = 0;
};

struct ChangeResult {
    bool ok = false;
    std::string error;
    bool restart_needed = false;  // the change only takes effect after a restart
};

class SettingsController {
public:
    // `stored` is what settings.toml holds (defaults plus the file); `active` is what runs (plus command-line flags).
    // `save_path` empty means nothing is ever written.
    SettingsController(Settings stored, Settings active, SettingsSink* sink, std::filesystem::path save_path);

    const Settings& active() const { return active_; }
    const Settings& stored() const { return stored_; }

    // A user's change: applied now and kept in the stored configuration (call save() to write it).
    ChangeResult set(std::string_view id, std::string_view value);
    ChangeResult select_preset(int id);

    // Flips between Original and the user's chosen comparison preset for a side-by-side look. Not stored. From any
    // configuration other than Original it switches to Original and remembers that configuration, and the next
    // call restores it exactly. Returns the text for a toast.
    std::string toggle_comparison();
    bool showing_original_for_comparison() const { return stash_.has_value(); }

    // Settings whose changes are waiting for a restart.
    std::vector<std::string> pending_restart() const;
    bool save(std::string& error);

private:
    void apply_diff(const Settings& before, ChangeResult& result);
    std::string describe_preset() const;

    Settings stored_, active_, startup_;
    SettingsSink* sink_;
    std::filesystem::path save_path_;
    std::optional<Settings> stash_;  // the configuration the comparison toggle displaced
    std::vector<std::string> failed_live_;
};

}  // namespace dw3app
