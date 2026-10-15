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
target_link_libraries(dw3recomp PRIVATE dw3runtime dw3render dw3settings dw3ingame dw3wizard_ui dw3icon psxdisc SDL3::SDL3)
if(DW3_HAVE_GENERATED)
    target_link_libraries(dw3recomp PRIVATE dw3host)
    target_compile_definitions(dw3recomp PRIVATE DW3_HAVE_GENERATED=1)
endif()
dw3_target_defaults(dw3recomp)

if(WIN32)
    set(DW3_INSTALL_BINDIR .)
else()
    set(DW3_INSTALL_BINDIR bin)
endif()
install(TARGETS dw3recomp RUNTIME DESTINATION ${DW3_INSTALL_BINDIR} COMPONENT app)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . COMPONENT app)
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
