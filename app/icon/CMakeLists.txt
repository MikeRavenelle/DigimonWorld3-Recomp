# The window icon: a 128x128 RGBA image embedded in the executable and set on every window (X11, Windows, and
# unbundled macOS runs). The file is made by packaging/icons/make_icons.sh.
set(DW3_WINDOW_ICON ${PROJECT_SOURCE_DIR}/packaging/icons/window-icon-128.rgba)
set(DW3_WINDOW_ICON_HEADER ${CMAKE_CURRENT_BINARY_DIR}/window_icon_data.h)
add_custom_command(
    OUTPUT ${DW3_WINDOW_ICON_HEADER}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${DW3_WINDOW_ICON} -DOUTPUT=${DW3_WINDOW_ICON_HEADER} -DSYMBOL=window_icon_128
            -DNAMESPACE=icon -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
    DEPENDS ${DW3_WINDOW_ICON} ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
    COMMENT "Embedding the window icon"
    VERBATIM)

add_library(dw3icon STATIC window_icon.cpp ${DW3_WINDOW_ICON_HEADER})
target_include_directories(dw3icon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(dw3icon PUBLIC SDL3::SDL3)
dw3_target_defaults(dw3icon)
