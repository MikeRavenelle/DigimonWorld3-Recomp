#include "dw3/platform.h"

#include <cstdlib>

namespace dw3::platform {

std::string_view arch_name(Arch a) {
    switch (a) {
        case Arch::X86_64: return "x86_64";
        case Arch::Arm64: return "arm64";
    }
    return "?";
}

std::string_view os_name(Os o) {
    switch (o) {
        case Os::Linux: return "linux";
        case Os::MacOS: return "macos";
        case Os::Windows: return "windows";
    }
    return "?";
}

std::filesystem::path user_data_dir() {
    namespace fs = std::filesystem;
#if defined(DW3_OS_windows)
    if (const char* p = std::getenv("APPDATA")) return fs::path(p) / "DW3Recomp";
#elif defined(DW3_OS_macos)
    if (const char* p = std::getenv("HOME")) return fs::path(p) / "Library/Application Support/DW3Recomp";
#else
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p) return fs::path(p) / "DW3Recomp";
    if (const char* p = std::getenv("HOME")) return fs::path(p) / ".local/share/DW3Recomp";
#endif
    return fs::current_path() / "userdata";
}

}  // namespace dw3::platform
