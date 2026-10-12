#include "dw3/platform.h"

#include <cstdlib>

#if defined(DW3_OS_windows)
#include <windows.h>
#include <shellapi.h>
#endif

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

#if defined(DW3_OS_windows)
namespace {

std::wstring env_wide(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(n);
    return v;
}

}  // namespace
#endif

std::filesystem::path user_data_dir() {
    namespace fs = std::filesystem;
#if defined(DW3_OS_windows)
    if (std::wstring p = env_wide(L"APPDATA"); !p.empty()) return fs::path(p) / L"DW3Recomp";
    if (std::wstring p = env_wide(L"USERPROFILE"); !p.empty()) return fs::path(p) / L"AppData" / L"Roaming" / L"DW3Recomp";
#elif defined(DW3_OS_macos)
    if (const char* p = std::getenv("HOME")) return fs::path(p) / "Library/Application Support/DW3Recomp";
#else
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p) return fs::path(p) / "DW3Recomp";
    if (const char* p = std::getenv("HOME")) return fs::path(p) / ".local/share/DW3Recomp";
#endif
    return fs::current_path() / "userdata";
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string path_to_utf8(const std::filesystem::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::vector<std::string> command_line_utf8(int argc, char** argv) {
    std::vector<std::string> args;
#if defined(DW3_OS_windows)
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; ++i) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
            args.push_back(std::move(a));
        }
        LocalFree(wargv);
        return args;
    }
#endif
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return args;
}

}  // namespace dw3::platform
