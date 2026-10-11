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

# plume RHI (MIT): the GPU renderer's graphics API layer. Only the Vulkan backend is built for now (Linux);
# the Metal (macOS) and Direct3D 12 (Windows) backends are part of plume itself and are switched on per platform
# in cmake/Platform.cmake once the renderer has been brought up on them.
option(DW3_HW_RENDERER "Build the plume GPU renderer" ON)
set(DW3_HAVE_PLUME OFF)
if(DW3_HW_RENDERER)
    if(NOT DW3_RENDER_VULKAN OR NOT DW3_OS STREQUAL "linux")
        message(STATUS "DW3: GPU renderer disabled (only the Vulkan backend on Linux is wired up)")
    else()
        find_program(DW3_GLSLANG NAMES glslang glslangValidator)
        find_package(X11 QUIET)
        if(NOT DW3_GLSLANG)
            message(STATUS "DW3: GPU renderer disabled (glslang not found; needed to compile the shaders)")
        elseif(NOT X11_FOUND)
            message(STATUS "DW3: GPU renderer disabled (X11 development files not found)")
        else()
            # Pinned commit; plume has no release tags. The patch adds the texture-to-buffer copy the Vulkan
            # backend lacks (used to read VRAM back to the CPU), and lets a window be described by callbacks so that SDL
            # can supply the surface (X11 and Wayland alike).
            FetchContent_Declare(plume
                GIT_REPOSITORY https://github.com/renderbag/plume.git
                GIT_TAG d72379344dacd3dbf9f810f92ddc87e6de1845b1
                GIT_SUBMODULES contrib/volk contrib/VulkanMemoryAllocator contrib/Vulkan-Headers
                PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=${CMAKE_CURRENT_LIST_DIR}/patches/plume-vulkan.patch
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
