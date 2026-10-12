#pragma once

// Platform / architecture abstraction. All OS- or ISA-specific behavior goes behind these.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dw3::platform {

enum class Arch { X86_64, Arm64 };
enum class Os { Linux, MacOS, Windows };

constexpr Arch host_arch() {
#if defined(DW3_ARCH_x86_64)
    return Arch::X86_64;
#elif defined(DW3_ARCH_arm64)
    return Arch::Arm64;
#else
#error "unknown arch"
#endif
}

constexpr Os host_os() {
#if defined(DW3_OS_linux)
    return Os::Linux;
#elif defined(DW3_OS_macos)
    return Os::MacOS;
#elif defined(DW3_OS_windows)
    return Os::Windows;
#else
#error "unknown os"
#endif
}

std::string_view arch_name(Arch a);
std::string_view os_name(Os o);

// Per-user writable directory for saves, memory cards, config and logs.
//   Linux    $XDG_DATA_HOME/DW3Recomp or ~/.local/share/DW3Recomp
//   macOS    ~/Library/Application Support/DW3Recomp
//   Windows  %APPDATA%\DW3Recomp
std::filesystem::path user_data_dir();

// Strings that cross module boundaries (command line, configuration, log messages, SDL) are UTF-8. These convert
// them to and from paths without going through the Windows ANSI code page, so names with accents or other
// non-ASCII characters work.
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& p);

// The process arguments as UTF-8. On Windows main() receives them in the ANSI code page, so they are read again
// from the wide command line there; elsewhere they are argv unchanged.
std::vector<std::string> command_line_utf8(int argc, char** argv);

}  // namespace dw3::platform
