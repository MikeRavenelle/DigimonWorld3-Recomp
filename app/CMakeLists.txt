add_executable(dw3recomp main.cpp)
target_link_libraries(dw3recomp PRIVATE dw3runtime SDL3::SDL3)
dw3_target_defaults(dw3recomp)
