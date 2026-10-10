#pragma once

// Platform / architecture abstraction. All OS- or ISA-specific behavior goes behind these.

#include <cstdint>
#include <filesystem>
#include <string_view>

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
std::filesystem::path user_data_dir();

}  // namespace dw3::platform
