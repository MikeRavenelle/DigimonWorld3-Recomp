# Host tools that turn the SPIR-V shaders into DXIL for the Direct3D 12 backend:
#   DW3_SPIRV_CROSS  SPIR-V -> HLSL (SPIRV-Cross)
#   DW3_DXC          HLSL -> DXIL (DirectX Shader Compiler)
# Either can be given on the command line; otherwise spirv-cross / dxc from the PATH are used, and failing that
# SPIRV-Cross is built from source with the host compiler and the DXC release for the host is downloaded.
# Sets DW3_D3D12_TOOLS_FOUND and, when SPIRV-Cross is built here, DW3_SPIRV_CROSS_TARGET to a target that provides it.

include(FetchContent)
include(ExternalProject)

set(DW3_D3D12_TOOLS_FOUND OFF)
set(DW3_SPIRV_CROSS_TARGET "")

set(DW3_SPIRV_CROSS "" CACHE FILEPATH "spirv-cross executable (built from source when empty and not on the PATH)")
set(DW3_DXC "" CACHE FILEPATH "dxc executable (downloaded when empty and not on the PATH)")

# DXC
if(NOT DW3_DXC)
    find_program(DW3_DXC_FOUND dxc)
    if(DW3_DXC_FOUND)
        set(DW3_DXC "${DW3_DXC_FOUND}" CACHE FILEPATH "" FORCE)
    endif()
endif()
if(NOT DW3_DXC)
    set(DXC_TAG v1.9.2609)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        FetchContent_Declare(dxc_release
            URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_TAG}/linux_dxc_2026_09_28.x86_x64.tar.gz)
        set(DXC_EXE bin/dxc)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        FetchContent_Declare(dxc_release
            URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_TAG}/dxc_2026_09_29.zip)
        set(DXC_EXE bin/x64/dxc.exe)
    endif()
    if(DXC_EXE)
        FetchContent_MakeAvailable(dxc_release)
        set(DW3_DXC "${dxc_release_SOURCE_DIR}/${DXC_EXE}" CACHE FILEPATH "" FORCE)
    endif()
endif()

# SPIRV-Cross
if(NOT DW3_SPIRV_CROSS)
    find_program(DW3_SPIRV_CROSS_FOUND spirv-cross)
    if(DW3_SPIRV_CROSS_FOUND)
        set(DW3_SPIRV_CROSS "${DW3_SPIRV_CROSS_FOUND}" CACHE FILEPATH "" FORCE)
    endif()
endif()
if(NOT DW3_SPIRV_CROSS)
    FetchContent_Declare(spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG vulkan-sdk-1.4.328.1
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build-separately)  # only fetched here; it is built for the host below
    FetchContent_MakeAvailable(spirv_cross)
    # A build tool: always compiled for the machine that runs the build, not for the target.
    set(exe_suffix "")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(exe_suffix ".exe")
    endif()
    set(cross_exe ${CMAKE_BINARY_DIR}/tools/spirv-cross/bin/spirv-cross${exe_suffix})
    set(host_compilers)
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(host_compilers -DCMAKE_C_COMPILER=cc -DCMAKE_CXX_COMPILER=c++)
    endif()
    ExternalProject_Add(dw3_spirv_cross
        SOURCE_DIR ${spirv_cross_SOURCE_DIR}
        BINARY_DIR ${CMAKE_BINARY_DIR}/tools/spirv-cross/build
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_CLI=ON -DSPIRV_CROSS_ENABLE_TESTS=OFF
                   -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=ON -DSPIRV_CROSS_ENABLE_MSL=ON
                   -DSPIRV_CROSS_ENABLE_CPP=ON -DSPIRV_CROSS_ENABLE_REFLECT=ON -DSPIRV_CROSS_ENABLE_C_API=OFF
                   -DSPIRV_CROSS_ENABLE_UTIL=ON -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON
                   -DCMAKE_TOOLCHAIN_FILE= ${host_compilers}
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/spirv-cross${exe_suffix} ${cross_exe}
        BUILD_BYPRODUCTS ${cross_exe})
    set(DW3_SPIRV_CROSS ${cross_exe})
    set(DW3_SPIRV_CROSS_TARGET dw3_spirv_cross)
endif()

if(DW3_DXC AND DW3_SPIRV_CROSS)
    set(DW3_D3D12_TOOLS_FOUND ON)
endif()
message(STATUS "DW3: Direct3D 12 shader tools: spirv-cross=${DW3_SPIRV_CROSS} dxc=${DW3_DXC}")
