add_library(dw3runtime STATIC
    core/bus.cpp
    core/machine.cpp
    hw/gte.cpp
    platform/platform.cpp)
target_include_directories(dw3runtime PUBLIC include)
dw3_target_defaults(dw3runtime)
