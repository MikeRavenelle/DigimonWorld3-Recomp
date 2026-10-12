cmake_minimum_required(VERSION 3.24)
project(DW3Recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # /EHsc would let the compiler assume that extern "C" functions never throw, but the runtime's fatal-error and
    # state-restore paths throw through recompiled (C) frames, so use /EHs.
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Platform)
include(Dependencies)

option(DW3_BUILD_TESTS "Build unit tests" ON)
option(DW3_BUILD_APP "Build the game executable (requires recompiled sources)" ON)

# Location of the user-supplied disc image. Never committed.
set(DW3_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/game" CACHE PATH "Directory containing the Digimon World 3 (USA) bin/cue")

# Recompiled game code, produced by `psxrecomp config/dw3.toml emit --out generated` (never committed).
set(DW3_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "psxrecomp output directory")

add_subdirectory(tools/psxdisc)
add_subdirectory(tools/dw3extract)
add_subdirectory(tools/psxrecomp)
add_subdirectory(runtime)
add_subdirectory(render)

if(EXISTS "${DW3_GENERATED_DIR}/sources.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DW3_GENERATED_DIR}/sources.txt")
    file(STRINGS "${DW3_GENERATED_DIR}/sources.txt" DW3_GENERATED_NAMES)
    list(TRANSFORM DW3_GENERATED_NAMES PREPEND "${DW3_GENERATED_DIR}/")
    add_library(dw3_generated STATIC ${DW3_GENERATED_NAMES})
    target_link_libraries(dw3_generated PUBLIC dw3runtime)
    # Generated code is large and label-heavy; always optimise it and keep it quiet.
    # -fexceptions: the HLE kernel's ReturnFromException unwinds through recompiled frames.
    target_compile_options(dw3_generated PRIVATE $<$<C_COMPILER_ID:Clang,GNU>:-O1> $<$<C_COMPILER_ID:Clang,GNU>:-w>
        $<$<C_COMPILER_ID:Clang,GNU>:-fexceptions> $<$<C_COMPILER_ID:MSVC>:/O1> $<$<C_COMPILER_ID:MSVC>:/w>
        $<$<C_COMPILER_ID:MSVC>:/EHsc>)
    set(DW3_HAVE_GENERATED ON)
    add_subdirectory(host)
    message(STATUS "DW3: using recompiled code from ${DW3_GENERATED_DIR}")
else()
    set(DW3_HAVE_GENERATED OFF)
    message(STATUS "DW3: no recompiled code found; run psxrecomp emit to generate it")
endif()

add_subdirectory(tools/scenario)  # after the generated-code check: the runner needs dw3host

# Regenerate recompiled code: cmake --build <dir> --target recompile
add_custom_target(recompile
    COMMAND psxrecomp "${CMAKE_CURRENT_SOURCE_DIR}/config/dw3.toml" emit --out "${DW3_GENERATED_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS psxrecomp
    COMMENT "Recompiling game code with psxrecomp")

if(DW3_BUILD_APP)
    add_subdirectory(app)
endif()

if(DW3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
