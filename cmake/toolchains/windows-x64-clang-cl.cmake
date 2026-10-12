# Cross-compiles for Windows x64 from a Linux (or macOS) host with clang-cl and lld-link against the Windows SDK and
# MSVC CRT laid out by xwin (the Jake-Shadle/xwin tool):
#
#   xwin --accept-license splat --output <dir>
#
# Point DW3_XWIN_DIR (CMake cache variable or environment variable) at that directory; ~/.local/share/xwin is
# used when it exists.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DW3_XWIN_DIR AND DEFINED ENV{DW3_XWIN_DIR})
    set(DW3_XWIN_DIR "$ENV{DW3_XWIN_DIR}")
elseif(NOT DW3_XWIN_DIR AND DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/.local/share/xwin/crt/include")
    set(DW3_XWIN_DIR "$ENV{HOME}/.local/share/xwin")
endif()
set(DW3_XWIN_DIR "${DW3_XWIN_DIR}" CACHE PATH "Windows SDK and MSVC CRT laid out by `xwin splat`")
if(NOT DW3_XWIN_DIR OR NOT EXISTS "${DW3_XWIN_DIR}/crt/include")
    message(FATAL_ERROR "DW3_XWIN_DIR must point at an `xwin splat` output directory (see docs/BUILDING.md)")
endif()

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_AR llvm-lib)

set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(DW3_XWIN_CFLAGS
    "--target=x86_64-pc-windows-msvc /imsvc${DW3_XWIN_DIR}/crt/include /imsvc${DW3_XWIN_DIR}/sdk/include/ucrt /imsvc${DW3_XWIN_DIR}/sdk/include/um /imsvc${DW3_XWIN_DIR}/sdk/include/shared /imsvc${DW3_XWIN_DIR}/sdk/include/winrt")
set(CMAKE_C_FLAGS_INIT "${DW3_XWIN_CFLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${DW3_XWIN_CFLAGS}")
set(CMAKE_RC_FLAGS_INIT "-I${DW3_XWIN_DIR}/sdk/include/um -I${DW3_XWIN_DIR}/sdk/include/shared")

set(DW3_XWIN_LDFLAGS
    "/libpath:${DW3_XWIN_DIR}/crt/lib/x86_64 /libpath:${DW3_XWIN_DIR}/sdk/lib/um/x86_64 /libpath:${DW3_XWIN_DIR}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${DW3_XWIN_LDFLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${DW3_XWIN_LDFLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${DW3_XWIN_LDFLAGS}")

# Static CRT: nothing to install next to the executables, and Wine needs no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
