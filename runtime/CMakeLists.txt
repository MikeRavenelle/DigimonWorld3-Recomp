add_library(dw3runtime STATIC
    bios/bios.cpp
    bios/bios_card.cpp
    bios/bios_devices.cpp
    bios/bios_state.cpp
    core/bus.cpp
    core/disc_ref.cpp
    core/machine.cpp
    core/memcard.cpp
    core/state.cpp
    hw/gpu.cpp
    render/soft_rasterizer.cpp
    render/present_layout.cpp
    hw/gte.cpp
    hw/precise_vertex.cpp
    hw/scheduler.cpp
    hw/spu.cpp
    hw/spu_engine.cpp
    hw/spu_dsp.cpp
    hw/xa.cpp
    hw/irq.cpp
    hw/video_timing.cpp
    hw/timers.cpp
    hw/dma.cpp
    hw/cdrom.cpp
    hw/sio.cpp
    hw/mdec.cpp
    platform/platform.cpp)
target_include_directories(dw3runtime PUBLIC include)
target_link_libraries(dw3runtime PUBLIC libzstd_static)
target_include_directories(dw3runtime PUBLIC ${zstd_SOURCE_DIR}/lib)
dw3_target_defaults(dw3runtime)
if(WIN32)
    target_link_libraries(dw3runtime PUBLIC shell32)
endif()
