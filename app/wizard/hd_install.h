// The optional HD background step end to end: download the upscaler and model files with curl, verify their SHA-256,
// unpack them under <user data>/tools, then build the texture pack from the disc. Processes and the pack generator are
// injected so that the sequence can be tested; run() blocks and is meant for a worker thread.
#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "assets.h"
#include "dw3/platform.h"
#include "wizard_state.h"

namespace dw3setup {

struct HdConfig {
    std::filesystem::path user_data;
    std::filesystem::path disc;
    DownloadPlan plan;
    dw3::platform::Os os = dw3::platform::host_os();
    // Runs a program and returns its exit code (see process_runner.h for the negative codes).
    std::function<int(const std::vector<std::string>&, const std::atomic<bool>& cancel)> run;
    // Builds the texture pack from the disc; reports (done, total) images. Returns false with an error, also on cancel.
    std::function<bool(const std::filesystem::path& upscaler, const std::filesystem::path& models, const std::filesystem::path& pack_dir,
                       const std::function<void(std::size_t, std::size_t)>& progress, const std::atomic<bool>& cancel, std::string& error)>
        generate;
};

struct HdSnapshot {
    HdPhase phase = HdPhase::Preparing;
    std::string status;         // one line: what is happening now
    double fraction = -1.0;     // 0..1 of the current phase, or -1 when unknown
};

struct HdResult {
    bool ok = false;
    std::string error;
    bool manual_download = false;  // curl is missing or failed: the player can fetch the files by hand into downloads_dir
};

class HdInstaller {
public:
    explicit HdInstaller(HdConfig config) : config_(std::move(config)) {}
    HdResult run();
    void cancel() { cancel_ = true; }
    HdSnapshot snapshot() const;

private:
    bool downloaded_ok(const Asset& asset, const std::filesystem::path& file) const;
    HdResult download_all();
    HdResult unpack_all();
    void set_status(HdPhase phase, std::string status, double fraction = -1.0);
    HdResult fail(std::string message, bool manual = false) const;

    HdConfig config_;
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    HdSnapshot snapshot_;
    std::filesystem::path part_file_;  // the download in progress
    uint64_t part_expected_ = 0;
};

}  // namespace dw3setup
