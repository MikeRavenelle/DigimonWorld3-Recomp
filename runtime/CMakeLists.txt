add_library(dw3runtime STATIC
    bios/bios.cpp
    core/bus.cpp
    core/machine.cpp
    hw/gpu.cpp
    render/soft_rasterizer.cpp
    hw/gte.cpp
    hw/scheduler.cpp
    hw/spu.cpp
    hw/irq.cpp
    hw/video_timing.cpp
    hw/timers.cpp
    hw/dma.cpp
    hw/cdrom.cpp
    platform/platform.cpp)
target_include_directories(dw3runtime PUBLIC include)
dw3_target_defaults(dw3runtime)
