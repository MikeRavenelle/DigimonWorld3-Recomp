# Host tool that turns the SPIR-V shaders into Metal Shading Language for the Metal backend: dw3_spirv_msl, built from
# tools/shaders/spirv_to_msl.cpp against SPIRV-Cross. The MSL is embedded as text and compiled when the device
# loads it, so the build needs no Metal compiler.

include(FetchContent)

FetchContent_Declare(spirv_cross_lib
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG vulkan-sdk-1.4.328.1
    GIT_SHALLOW TRUE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spirv_cross_lib)

add_executable(dw3_spirv_msl ${PROJECT_SOURCE_DIR}/tools/shaders/spirv_to_msl.cpp)
target_link_libraries(dw3_spirv_msl PRIVATE spirv-cross-msl spirv-cross-glsl spirv-cross-core)
set(DW3_SPIRV_MSL dw3_spirv_msl)
