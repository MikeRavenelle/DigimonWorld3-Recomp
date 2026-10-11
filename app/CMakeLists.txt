add_executable(dw3recomp main.cpp
    present/presenter.cpp
    present/osd.cpp
    present/software_presenter.cpp
    present/hw_presenter.cpp
    present/native_window.cpp)
target_include_directories(dw3recomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dw3recomp PRIVATE dw3runtime dw3render psxdisc SDL3::SDL3)
if(DW3_HAVE_GENERATED)
    target_link_libraries(dw3recomp PRIVATE dw3host)
    target_compile_definitions(dw3recomp PRIVATE DW3_HAVE_GENERATED=1)
endif()
dw3_target_defaults(dw3recomp)
