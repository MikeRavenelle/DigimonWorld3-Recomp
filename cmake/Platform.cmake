# Host OS / architecture detection. Everything platform-specific keys off these.

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(DW3_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(DW3_ARCH "arm64")
else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(WIN32)
    set(DW3_OS "windows")
elseif(APPLE)
    set(DW3_OS "macos")
elseif(UNIX)
    set(DW3_OS "linux")
else()
    message(FATAL_ERROR "Unsupported OS")
endif()

# Graphics backends available per OS (plume RHI).
set(DW3_RENDER_VULKAN OFF)
set(DW3_RENDER_METAL OFF)
set(DW3_RENDER_D3D12 OFF)
if(DW3_OS STREQUAL "linux")
    set(DW3_RENDER_VULKAN ON)
elseif(DW3_OS STREQUAL "macos")
    set(DW3_RENDER_METAL ON)
elseif(DW3_OS STREQUAL "windows")
    set(DW3_RENDER_D3D12 ON)
    set(DW3_RENDER_VULKAN ON)
endif()

message(STATUS "DW3: os=${DW3_OS} arch=${DW3_ARCH} vulkan=${DW3_RENDER_VULKAN} metal=${DW3_RENDER_METAL} d3d12=${DW3_RENDER_D3D12}")

function(dw3_target_defaults target)
    target_compile_definitions(${target} PUBLIC
        DW3_OS_${DW3_OS}=1
        DW3_ARCH_${DW3_ARCH}=1)
    if(WIN32)
        target_compile_definitions(${target} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endfunction()
