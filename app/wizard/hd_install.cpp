#include "hd_install.h"

#include <algorithm>
#include <system_error>

#include "commands.h"
#include "process_runner.h"
#include "sha256.h"
#include "zip_extract.h"

namespace dw3setup {

namespace fs = std::filesystem;

HdSnapshot HdInstaller::snapshot() const {
    std::lock_guard lock(mutex_);
    HdSnapshot s = snapshot_;
    if (s.phase == HdPhase::Downloading && !part_file_.empty() && part_expected_ > 0) {
        std::error_code ec;
        const auto size = fs::file_size(part_file_, ec);
        if (!ec) s.fraction = std::min(1.0, static_cast<double>(size) / static_cast<double>(part_expected_));
    }
    return s;
}

void HdInstaller::set_status(HdPhase phase, std::string status, double fraction) {
    std::lock_guard lock(mutex_);
    snapshot_.phase = phase;
    snapshot_.status = std::move(status);
    snapshot_.fraction = fraction;
}

HdResult HdInstaller::fail(std::string message, bool manual) const {
    HdResult r;
    r.error = std::move(message);
    r.manual_download = manual;
    return r;
}

bool HdInstaller::downloaded_ok(const Asset& asset, const fs::path& file) const {
    std::string hash;
    return fs::is_regular_file(file) && sha256_file(file, hash) && hash == asset.sha256;
}

HdResult HdInstaller::download_all() {
    const fs::path dir = downloads_dir(config_.user_data);
    std::error_code ec;
    fs::create_directories(dir, ec);
    int curl_state = 0;  // 0 unknown, 1 present, -1 missing
    for (const Asset& asset : config_.plan.assets) {
        if (cancel_) return fail("Cancelled.");
        const fs::path file = dir / asset.file_name;
        if (downloaded_ok(asset, file)) continue;
        if (curl_state == 0) {
            const int rc = config_.run(curl_version_command(), cancel_);
            curl_state = rc == 0 ? 1 : -1;
            if (rc == kProcessCancelled) return fail("Cancelled.");
        }
        if (curl_state < 0)
            return fail("curl was not found on this computer, so the files cannot be downloaded automatically.", true);
        const fs::path part = dir / (asset.file_name + ".part");
        fs::remove(part, ec);
        {
            std::lock_guard lock(mutex_);
            part_file_ = part;
            part_expected_ = asset.size_bytes;
        }
        set_status(HdPhase::Downloading, "Downloading " + asset.file_name);
        const int rc = config_.run(curl_download_command(asset.url, part), cancel_);
        {
            std::lock_guard lock(mutex_);
            part_file_.clear();
        }
        if (rc == kProcessCancelled) {
            fs::remove(part, ec);
            return fail("Cancelled.");
        }
        if (rc != 0) {
            fs::remove(part, ec);
            return fail("Downloading " + asset.file_name + " failed: " + describe_curl_exit(rc) + ".", true);
        }
        set_status(HdPhase::Downloading, "Verifying " + asset.file_name);
        std::string hash;
        if (!sha256_file(part, hash)) return fail("Cannot read the downloaded file " + asset.file_name + ".");
        if (hash != asset.sha256) {
            fs::remove(part, ec);
            return fail(asset.file_name + " does not match its expected SHA-256 (expected " + asset.sha256 + ", got " + hash +
                        "). The file was deleted; it may have been changed or damaged in transit.");
        }
        fs::rename(part, file, ec);
        if (ec) return fail("Cannot save " + asset.file_name + ": " + ec.message());
    }
    return {true, {}, false};
}

HdResult HdInstaller::unpack_all() {
    const fs::path dir = downloads_dir(config_.user_data);
    std::error_code ec;
    fs::create_directories(models_dir(config_.user_data), ec);
    for (const Asset& asset : config_.plan.assets) {
        set_status(HdPhase::Unpacking, "Unpacking " + asset.file_name);
        const fs::path file = dir / asset.file_name;
        if (asset.is_archive) {
            std::string error;
            if (!extract_zip(file, tools_dir(config_.user_data), error)) return fail("Cannot unpack " + asset.file_name + ": " + error);
        } else {
            fs::copy_file(file, models_dir(config_.user_data) / asset.file_name, fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("Cannot install " + asset.file_name + ": " + ec.message());
        }
    }
    const fs::path exe = upscaler_executable(config_.user_data, config_.os);
    if (!fs::exists(exe)) return fail("The upscaler archive did not contain " + exe.filename().string() + ".");
    fs::permissions(exe, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    return {true, {}, false};
}

HdResult HdInstaller::run() {
    set_status(HdPhase::Preparing, "Checking the download list");
    if (const std::string problem = plan_problem(config_.plan); !problem.empty()) return fail(problem);
    HdResult r = download_all();
    if (!r.ok) return r;
    if (cancel_) return fail("Cancelled.");
    r = unpack_all();
    if (!r.ok) return r;
    if (cancel_) return fail("Cancelled.");

    set_status(HdPhase::Generating, "Preparing the field maps of your disc");
    std::string error;
    auto progress = [&](std::size_t done, std::size_t total) {
        set_status(HdPhase::Generating, "Upscaling backgrounds: " + std::to_string(done) + " of " + std::to_string(total),
                   total ? static_cast<double>(done) / static_cast<double>(total) : -1.0);
    };
    if (!config_.generate(upscaler_executable(config_.user_data, config_.os), models_dir(config_.user_data),
                          texture_pack_dir(config_.user_data, kPackName), progress, cancel_, error))
        return fail(cancel_ ? "Cancelled." : error);
    return {true, {}, false};
}

}  // namespace dw3setup
