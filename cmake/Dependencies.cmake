include(FetchContent)

# toml++ : recompiler / game config
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tomlplusplus)

# GoogleTest: prefer system package, fall back to source.
if(DW3_BUILD_TESTS OR NOT DEFINED DW3_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.15.2
            GIT_SHALLOW TRUE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
        add_library(GTest::gtest_main ALIAS gtest_main)
    endif()
endif()

# SDL3: windowing, input, audio output. Prefer system package.
find_package(SDL3 QUIET CONFIG)
if(NOT SDL3_FOUND)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.24
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

# plume RHI (Vulkan / D3D12 / Metal) is added in the render milestone (see docs/ARCHITECTURE.md).
