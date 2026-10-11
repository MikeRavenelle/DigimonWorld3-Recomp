# GPU renderer. The pure parts (primitive -> vertex conversion, dirty-region bookkeeping) have no GPU dependency and
# are always built; the plume-backed renderer is added when plume is available (see cmake/Dependencies.cmake).
add_library(dw3render STATIC
    src/vram_regions.cpp
    src/vertex_builder.cpp)
target_include_directories(dw3render PUBLIC include)
target_link_libraries(dw3render PUBLIC dw3runtime)
dw3_target_defaults(dw3render)

if(DW3_HAVE_PLUME)
    set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
    set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${SHADER_OUT})
    find_program(DW3_SPIRV_VAL spirv-val)

    set(SHADER_HEADERS)
    # name|source|extra glslang options. The display layer variant of the draw shader writes a second target.
    foreach(entry psx_draw.vert|psx_draw.vert psx_draw.frag|psx_draw.frag psx_draw_layer.frag|psx_draw.frag|-DDISPLAY_LAYER
                  present.vert|present.vert present.frag|present.frag osd.vert|osd.vert osd.frag|osd.frag vram_copy.vert|vram_copy.vert vram_copy.frag|vram_copy.frag)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 shader)
        list(GET parts 1 source)
        list(LENGTH parts nparts)
        set(defs)
        if(nparts GREATER 2)
            list(GET parts 2 defs)
        endif()
        string(REPLACE "." "_" symbol ${shader})
        set(spv ${SHADER_OUT}/${shader}.spv)
        set(hdr ${SHADER_OUT}/${shader}.h)
        set(validate_cmd COMMAND ${CMAKE_COMMAND} -E true)
        if(DW3_SPIRV_VAL)
            set(validate_cmd COMMAND ${DW3_SPIRV_VAL} --target-env vulkan1.2 ${spv})
        endif()
        add_custom_command(
            OUTPUT ${hdr}
            COMMAND ${DW3_GLSLANG} -V --target-env vulkan1.2 ${defs} -o ${spv} ${SHADER_DIR}/${source}
            ${validate_cmd}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${hdr} -DSYMBOL=${symbol}_spv
                    -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
            DEPENDS ${SHADER_DIR}/${source} ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
            COMMENT "Compiling shader ${shader}"
            VERBATIM)
        list(APPEND SHADER_HEADERS ${hdr})
    endforeach()
    add_custom_target(dw3_shaders DEPENDS ${SHADER_HEADERS})

    target_sources(dw3render PRIVATE src/hw_renderer.cpp)
    add_dependencies(dw3render dw3_shaders)
    target_include_directories(dw3render PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
   
    target_link_libraries(dw3render PRIVATE plume ${X11_LIBRARIES} ${CMAKE_DL_LIBS})
    target_include_directories(dw3render SYSTEM PRIVATE ${X11_INCLUDE_DIR})
    target_compile_definitions(dw3render PUBLIC DW3_HW_RENDERER=1)
endif()
