# Scenario format, input scripting and perceptual hashing (no game dependency; unit-tested).
add_library(dw3scenario_core STATIC scenario.cpp)
target_include_directories(dw3scenario_core PUBLIC .)
target_link_libraries(dw3scenario_core PUBLIC tomlplusplus::tomlplusplus)
dw3_target_defaults(dw3scenario_core)

# Headless runner: boots the recompiled game with the scenario's scripted pad and evaluates its checks.
if(DW3_HAVE_GENERATED)
    add_executable(dw3scenario runner.cpp)
    target_link_libraries(dw3scenario PRIVATE dw3host dw3scenario_core)
    target_compile_definitions(dw3scenario PRIVATE DW3_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    dw3_target_defaults(dw3scenario)
endif()
