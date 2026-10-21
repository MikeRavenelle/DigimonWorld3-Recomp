add_executable(dw3recomp main.cpp
    host_input.cpp
    present/presenter.cpp
    present/osd.cpp
    present/software_presenter.cpp
    present/hw_presenter.cpp
    present/texture_packs.cpp
    present/native_window.cpp
    present/monitors.cpp)
if(WIN32)
    target_sources(dw3recomp PRIVATE windows/dw3recomp.rc)
endif()
target_include_directories(dw3recomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dw3recomp PRIVATE dw3runtime dw3render dw3settings dw3ingame dw3wizard_ui dw3icon dw3host psxdisc SDL3::SDL3)
dw3_target_defaults(dw3recomp)

# The recompiler configuration is built into the program: it holds addresses only, and the setup turns it and the disc
# into the game module.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/config/dw3.toml")
file(READ "${PROJECT_SOURCE_DIR}/config/dw3.toml" DW3_GAME_CONFIG_TEXT)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/game_config.h"
    "// Generated from config/dw3.toml.\n#pragma once\ninline constexpr const char kGameConfigToml[] = R\"DW3CFG(${DW3_GAME_CONFIG_TEXT})DW3CFG\";\n")
target_include_directories(dw3recomp PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(dw3recomp PRIVATE DW3_VERSION="${PROJECT_VERSION}")

if(WIN32)
    set(DW3_INSTALL_BINDIR .)
else()
    set(DW3_INSTALL_BINDIR bin)
endif()
install(TARGETS dw3recomp RUNTIME DESTINATION ${DW3_INSTALL_BINDIR} COMPONENT app)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . COMPONENT app)

# The toolchain the setup builds the game module with, beside the executable (see wizard/module_build.h). The compiler
# itself is added from a zig distribution: -DDW3_ZIG_DIR=<unpacked zig folder> (the release workflow fetches and checks it).
install(FILES "${PROJECT_SOURCE_DIR}/runtime/include/dw3/recomp.h" DESTINATION ${DW3_INSTALL_BINDIR}/toolchain/include/dw3 COMPONENT app)
install(FILES "${PROJECT_SOURCE_DIR}/runtime/module/module_entry.c" DESTINATION ${DW3_INSTALL_BINDIR}/toolchain/module COMPONENT app)
set(DW3_ZIG_DIR "" CACHE PATH "Unpacked zig distribution to ship as the game module compiler")
if(DW3_ZIG_DIR)
    install(PROGRAMS "${DW3_ZIG_DIR}/zig${CMAKE_EXECUTABLE_SUFFIX}" DESTINATION ${DW3_INSTALL_BINDIR}/toolchain COMPONENT app)
    install(DIRECTORY "${DW3_ZIG_DIR}/lib" DESTINATION ${DW3_INSTALL_BINDIR}/toolchain COMPONENT app)
    install(FILES "${DW3_ZIG_DIR}/LICENSE" DESTINATION ${DW3_INSTALL_BINDIR}/toolchain COMPONENT app)
endif()
if(NOT WIN32 AND NOT APPLE)
    # Desktop integration for Linux: the launcher entry and the hicolor icons (the macOS .app has its own icon).
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/dw3recomp.desktop" DESTINATION share/applications COMPONENT app)
    foreach(size 16 32 48 64 128 256 512)
        install(FILES "${PROJECT_SOURCE_DIR}/packaging/icons/png/dw3recomp-${size}.png"
                DESTINATION share/icons/hicolor/${size}x${size}/apps RENAME dw3recomp.png COMPONENT app)
    endforeach()
endif()
if(WIN32)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/windows/README.txt" DESTINATION . COMPONENT app)
endif()
