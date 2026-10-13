# Settings model, presets, settings.toml and fast-forward pacing (no SDL or graphics dependency; unit-tested).
add_library(dw3settings STATIC settings.cpp presets.cpp controller.cpp)
target_include_directories(dw3settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dw3settings PUBLIC dw3runtime tomlplusplus::tomlplusplus)
dw3_target_defaults(dw3settings)
