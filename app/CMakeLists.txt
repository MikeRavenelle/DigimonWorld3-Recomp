add_executable(dw3recomp main.cpp
    host_input.cpp
    present/presenter.cpp
    present/osd.cpp
    present/software_presenter.cpp
    present/hw_presenter.cpp
    present/texture_packs.cpp
    present/native_window.cpp
    present/monitors.cpp)
target_include_directories(dw3recomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dw3recomp PRIVATE dw3runtime dw3render dw3settings dw3ingame dw3wizard_ui psxdisc SDL3::SDL3)
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
if(WIN32)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/windows/README.txt" DESTINATION . COMPONENT app)
endif()
