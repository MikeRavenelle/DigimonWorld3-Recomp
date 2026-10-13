# The settings screen inside the game's own START menu: the list model and the text encoding are plain C++ (unit-tested),
# the glue hooks the menu task of the recompiled game.
add_library(dw3ingame STATIC settings_list.cpp game_text.cpp field_menu.cpp gameplay.cpp audio_groups.cpp)
target_include_directories(dw3ingame PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dw3ingame PUBLIC dw3settings dw3runtime)
dw3_target_defaults(dw3ingame)
