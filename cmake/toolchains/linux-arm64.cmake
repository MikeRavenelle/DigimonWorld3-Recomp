# Cross-compiles for Linux aarch64 from another host with clang, lld and an aarch64 sysroot (glibc, libstdc++ and the
# development packages of SDL3's dependencies). Fedora can create one without an aarch64 machine:
#
#   sudo dnf install -y --forcearch=aarch64 --installroot=<dir> --use-host-config --releasever=<N> \
#       glibc-devel libstdc++-devel gcc SDL3-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel \
#       libXi-devel libXfixes-devel libXScrnSaver-devel libxkbcommon-devel wayland-devel vulkan-loader-devel
#
# Point DW3_ARM64_SYSROOT (CMake cache variable or environment variable) at <dir>;
# ~/.local/share/dw3-sysroot-aarch64 is used when it exists. Tests run under qemu-user (qemu-aarch64 or
# qemu-aarch64-static on the PATH) with the sysroot as the library root.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DW3_ARM64_SYSROOT AND DEFINED ENV{DW3_ARM64_SYSROOT})
    set(DW3_ARM64_SYSROOT "$ENV{DW3_ARM64_SYSROOT}")
elseif(NOT DW3_ARM64_SYSROOT AND DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/.local/share/dw3-sysroot-aarch64/usr/include")
    set(DW3_ARM64_SYSROOT "$ENV{HOME}/.local/share/dw3-sysroot-aarch64")
endif()
set(DW3_ARM64_SYSROOT "${DW3_ARM64_SYSROOT}" CACHE PATH "aarch64 Linux sysroot (see cmake/toolchains/linux-arm64.cmake)")
if(NOT DW3_ARM64_SYSROOT OR NOT EXISTS "${DW3_ARM64_SYSROOT}/usr/include")
    message(FATAL_ERROR "DW3_ARM64_SYSROOT must point at an aarch64 sysroot (see docs/BUILDING.md)")
endif()

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_SYSROOT "${DW3_ARM64_SYSROOT}")

# The GCC runtime (crt files, libgcc, libstdc++ headers) of the sysroot: the newest version installed.
file(GLOB DW3_ARM64_GCC_DIRS "${DW3_ARM64_SYSROOT}/usr/lib/gcc/*/*")
list(SORT DW3_ARM64_GCC_DIRS COMPARE NATURAL)
list(REVERSE DW3_ARM64_GCC_DIRS)
list(GET DW3_ARM64_GCC_DIRS 0 DW3_ARM64_GCC_DIR)
set(DW3_ARM64_FLAGS "--gcc-install-dir=${DW3_ARM64_GCC_DIR}")
set(CMAKE_C_FLAGS_INIT "${DW3_ARM64_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${DW3_ARM64_FLAGS}")
set(DW3_ARM64_LDFLAGS "-fuse-ld=lld -Wl,-rpath-link,${DW3_ARM64_SYSROOT}/usr/lib64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${DW3_ARM64_LDFLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${DW3_ARM64_LDFLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${DW3_ARM64_LDFLAGS}")

set(CMAKE_FIND_ROOT_PATH "${DW3_ARM64_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# CTest runs the test executables under qemu-user.
if(NOT DEFINED CMAKE_CROSSCOMPILING_EMULATOR AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    find_program(DW3_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)
    if(DW3_QEMU_AARCH64)
        set(CMAKE_CROSSCOMPILING_EMULATOR "${DW3_QEMU_AARCH64};-L;${DW3_ARM64_SYSROOT}")
    endif()
endif()
