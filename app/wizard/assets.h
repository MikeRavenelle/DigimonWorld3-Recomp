// What the optional HD background step downloads: the upscaler for this platform and the converted model files.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dw3/platform.h"

namespace dw3setup {

struct Asset {
    std::string file_name;   // as saved in the download folder
    std::string url;
    std::string sha256;      // lower-case hex; empty while not yet known
    uint64_t size_bytes = 0; // 0 when not known
    bool is_archive = false; // a .zip that is unpacked into <user data>/tools
};

// realesrgan-ncnn-vulkan v0.2.0 (MIT) from the official release; nullopt for a platform it is not built for.
std::optional<Asset> upscaler_asset(dw3::platform::Os os, dw3::platform::Arch arch);
// The converted Real-ESRGAN general-x4v3 model (BSD-3), published with this project's releases.
std::vector<Asset> model_assets();

// The model name texpack is told to use (file names are <name>.param and <name>.bin).
inline constexpr const char* kModelName = "RealESRGAN_General_x4_v3";

struct DownloadPlan {
    std::vector<Asset> assets;  // upscaler first
    std::string unsupported;    // set when this platform has no upscaler build
};
DownloadPlan plan_for(dw3::platform::Os os, dw3::platform::Arch arch);

// Empty when every asset can be fetched and verified; otherwise what is missing (an unset hash or placeholder URL).
std::string plan_problem(const DownloadPlan& plan);
bool is_placeholder_url(const std::string& url);

// Sum of the known sizes; `all_known` is false when some asset has no recorded size.
uint64_t total_download_bytes(const DownloadPlan& plan, bool& all_known);
std::string format_bytes(uint64_t bytes);

// Where things live under <user data>.
std::filesystem::path tools_dir(const std::filesystem::path& user_data);
std::filesystem::path models_dir(const std::filesystem::path& user_data);
std::filesystem::path upscaler_executable(const std::filesystem::path& user_data, dw3::platform::Os os);
std::filesystem::path downloads_dir(const std::filesystem::path& user_data);
std::filesystem::path texture_pack_dir(const std::filesystem::path& user_data, const std::string& pack_name);
inline constexpr const char* kPackName = "disc-hd";

}  // namespace dw3setup
