cmake_minimum_required(VERSION 3.24)
project(DW3Recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Platform)
include(Dependencies)

option(DW3_BUILD_TESTS "Build unit tests" ON)
option(DW3_BUILD_APP "Build the game executable (requires recompiled sources)" ON)

# Location of the user-supplied disc image. Never committed.
set(DW3_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/game" CACHE PATH "Directory containing the Digimon World 3 (USA) bin/cue")

add_subdirectory(tools/psxdisc)
add_subdirectory(tools/dw3extract)
add_subdirectory(tools/psxrecomp)
add_subdirectory(runtime)

if(DW3_BUILD_APP)
    add_subdirectory(app)
endif()

if(DW3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
