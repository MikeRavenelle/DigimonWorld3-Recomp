include(FetchContent)

# toml++ : recompiler / game config
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tomlplusplus)

# zstd : save state compression (BSD)
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
    SOURCE_SUBDIR build/cmake)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)
# Linked statically: its headers and library are not part of what gets installed or packaged.
set_property(DIRECTORY "${zstd_SOURCE_DIR}/build/cmake" PROPERTY EXCLUDE_FROM_ALL TRUE)

# GoogleTest: prefer system package, fall back to source.
if(DW3_BUILD_TESTS OR NOT DEFINED DW3_BUILD_TESTS)
    if(NOT WIN32)
        find_package(GTest QUIET)
    endif()
    if(NOT GTest_FOUND)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.17.0
            GIT_SHALLOW TRUE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        if(WIN32)
            set(gtest_disable_pthreads ON CACHE BOOL "" FORCE)
        endif()
        FetchContent_MakeAvailable(googletest)
        if(WIN32)
            # Its own -Werror trips over warnings that newer clang versions add.
            target_compile_options(gtest PRIVATE -Wno-error -Wno-character-conversion)
            target_compile_options(gtest_main PRIVATE -Wno-error)
        endif()
        if(NOT TARGET GTest::gtest_main)
            add_library(GTest::gtest_main ALIAS gtest_main)
        endif()
    endif()
endif()

# SDL3: windowing, input, audio output. Prefer the system package on Linux and macOS; on Windows it is built from
# source and linked statically, so there is no DLL to ship.
if(NOT WIN32)
    find_package(SDL3 QUIET CONFIG)
endif()
if(NOT SDL3_FOUND)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.24
        GIT_SHALLOW TRUE)
    if(WIN32)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(SDL3)
endif()

# plume RHI (MIT): the GPU renderer's graphics API layer. The Vulkan backend is used on Linux, Vulkan and Direct3D 12
# on Windows, Metal on macOS (cmake/Platform.cmake).
option(DW3_HW_RENDERER "Build the plume GPU renderer" ON)
set(DW3_HAVE_PLUME OFF)
if(DW3_HW_RENDERER)
    if(NOT DW3_OS STREQUAL "linux" AND NOT DW3_OS STREQUAL "windows" AND NOT DW3_OS STREQUAL "macos")
        message(STATUS "DW3: GPU renderer disabled (not available on this platform)")
    else()
        find_program(DW3_GLSLANG NAMES glslang glslangValidator)
        if(DW3_OS STREQUAL "linux")
            find_package(X11 QUIET)
        endif()
        if(NOT DW3_GLSLANG)
            message(STATUS "DW3: GPU renderer disabled (glslang not found; needed to compile the shaders)")
        elseif(DW3_OS STREQUAL "linux" AND NOT X11_FOUND)
            message(STATUS "DW3: GPU renderer disabled (X11 development files not found)")
        else()
            # Pinned commit; plume has no release tags. The patch adds the texture-to-buffer copy the Vulkan
            # backend lacks (used to read VRAM back to the CPU), and lets a window be described by callbacks so that SDL
            # can supply the surface (X11 and Wayland alike). The Direct3D 12 and Metal patches make that readback work
            # there too; the Metal one also lets a shader be given as source text, compiled when the device loads it.
            set(DW3_PLUME_SUBMODULES contrib/volk contrib/VulkanMemoryAllocator contrib/Vulkan-Headers)
            if(DW3_RENDER_D3D12)
                list(APPEND DW3_PLUME_SUBMODULES contrib/D3D12MemoryAllocator)
            endif()
            if(DW3_RENDER_METAL)
                list(APPEND DW3_PLUME_SUBMODULES contrib/metal-cpp)
            endif()
            FetchContent_Declare(plume
                GIT_REPOSITORY https://github.com/renderbag/plume.git
                GIT_TAG d72379344dacd3dbf9f810f92ddc87e6de1845b1
                GIT_SUBMODULES ${DW3_PLUME_SUBMODULES}
                PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_LIST_DIR}/patches/plume-vulkan.patch
                              -P ${CMAKE_CURRENT_LIST_DIR}/ApplyPatch.cmake
                      COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_LIST_DIR}/patches/plume-d3d12.patch
                              -P ${CMAKE_CURRENT_LIST_DIR}/ApplyPatch.cmake
                      COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_LIST_DIR}/patches/plume-metal.patch
                              -P ${CMAKE_CURRENT_LIST_DIR}/ApplyPatch.cmake)
            FetchContent_MakeAvailable(plume)
            # Third-party code: build quietly and treat its headers as system headers.
            target_compile_options(plume PRIVATE -w)
            get_target_property(DW3_PLUME_INCLUDES plume INTERFACE_INCLUDE_DIRECTORIES)
            set_target_properties(plume PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
            target_include_directories(plume SYSTEM INTERFACE ${DW3_PLUME_INCLUDES})
            set(DW3_HAVE_PLUME ON)
        endif()
    endif()
endif()
message(STATUS "DW3: GPU renderer (plume) = ${DW3_HAVE_PLUME}")
