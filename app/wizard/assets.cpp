#include "assets.h"

#include <cstdio>

namespace dw3setup {

namespace {

using dw3::platform::Arch;
using dw3::platform::Os;

constexpr const char* kUpscalerReleaseUrl = "https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases/download/v0.2.0/";

// Owner: upload the converted model files (tools/models/convert_realesrgan.py prints their hashes) to a release of this
// project, then set the base URL and the four values below.
constexpr const char* kModelReleaseUrl = "https://example.invalid/PLACEHOLDER-model-release/";
constexpr const char* kModelParamSha256 = "";
constexpr const char* kModelBinSha256 = "";

Asset upscaler_zip(const char* platform_name, const char* sha256, uint64_t size) {
    Asset a;
    a.file_name = std::string("realesrgan-ncnn-vulkan-v0.2.0-") + platform_name + ".zip";
    a.url = std::string(kUpscalerReleaseUrl) + a.file_name;
    a.sha256 = sha256;
    a.size_bytes = size;
    a.is_archive = true;
    return a;
}

}  // namespace

std::optional<Asset> upscaler_asset(Os os, Arch arch) {
    switch (os) {
        case Os::Windows:  // the x64 build also runs on Windows on Arm
            return upscaler_zip("windows", "1bbbdb12d470af80b035c773682e144c6c2f6ece9210832a289af0a48ce3fa9a", 2159606);
        case Os::MacOS:
            return upscaler_zip("macos", "f1cc31f13398126ddec25a186b9e50c7ab3e4f2bc011d0222331b43ce2037d3b", 8532236);
        case Os::Linux:
            if (arch != Arch::X86_64) return std::nullopt;
            return upscaler_zip("ubuntu", "d0e8e1cf954f5cde11be4745dd912cc3774bef36f71c5b1cb8f74c4112b6e919", 3654741);
    }
    return std::nullopt;
}

std::vector<Asset> model_assets() {
    Asset param;
    param.file_name = std::string(kModelName) + ".param";
    param.url = std::string(kModelReleaseUrl) + param.file_name;
    param.sha256 = kModelParamSha256;
    Asset bin = param;
    bin.file_name = std::string(kModelName) + ".bin";
    bin.url = std::string(kModelReleaseUrl) + bin.file_name;
    bin.sha256 = kModelBinSha256;
    return {param, bin};
}

DownloadPlan plan_for(Os os, Arch arch) {
    DownloadPlan plan;
    if (auto up = upscaler_asset(os, arch)) plan.assets.push_back(*up);
    else plan.unsupported = "There is no build of the upscaler for this platform.";
    for (const Asset& m : model_assets()) plan.assets.push_back(m);
    return plan;
}

bool is_placeholder_url(const std::string& url) { return url.find("PLACEHOLDER") != std::string::npos || url.find(".invalid/") != std::string::npos; }

std::string plan_problem(const DownloadPlan& plan) {
    if (!plan.unsupported.empty()) return plan.unsupported;
    for (const Asset& a : plan.assets) {
        if (is_placeholder_url(a.url)) return a.file_name + ": the download address has not been set in this build.";
        if (a.sha256.size() != 64) return a.file_name + ": the expected SHA-256 has not been set in this build, so it cannot be verified.";
    }
    return {};
}

uint64_t total_download_bytes(const DownloadPlan& plan, bool& all_known) {
    uint64_t total = 0;
    all_known = true;
    for (const Asset& a : plan.assets) {
        if (a.size_bytes == 0) all_known = false;
        total += a.size_bytes;
    }
    return total;
}

std::string format_bytes(uint64_t bytes) {
    char text[32];
    if (bytes >= 1024 * 1024) std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else std::snprintf(text, sizeof text, "%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    return text;
}

std::filesystem::path tools_dir(const std::filesystem::path& user_data) { return user_data / "tools"; }
std::filesystem::path models_dir(const std::filesystem::path& user_data) { return tools_dir(user_data) / "models"; }
std::filesystem::path downloads_dir(const std::filesystem::path& user_data) { return user_data / "downloads"; }
std::filesystem::path texture_pack_dir(const std::filesystem::path& user_data, const std::string& pack_name) {
    return user_data / "texture-packs" / pack_name;
}
std::filesystem::path upscaler_executable(const std::filesystem::path& user_data, Os os) {
    return tools_dir(user_data) / (os == Os::Windows ? "realesrgan-ncnn-vulkan.exe" : "realesrgan-ncnn-vulkan");
}

}  // namespace dw3setup
