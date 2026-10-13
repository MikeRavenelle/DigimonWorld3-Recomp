# GPU renderer. The pure parts (primitive -> vertex conversion, dirty-region bookkeeping) have no GPU dependency and
# are always built; the plume-backed renderer is added when plume is available (see cmake/Dependencies.cmake).
add_library(dw3render STATIC
    src/vertex_builder.cpp)
target_include_directories(dw3render PUBLIC include)
target_link_libraries(dw3render PUBLIC dw3runtime dw3texrep)
dw3_target_defaults(dw3render)

# Texture replacement: block hashing and registry, texture packs, dumping, and the image helpers of the pack generator.
add_library(dw3texrep STATIC
    src/vram_regions.cpp
    src/texrep/blocks.cpp
    src/texrep/pack.cpp
    src/texrep/dump.cpp
    src/texrep/generate.cpp
    src/texrep/window.cpp
    src/texrep/stb_impl.cpp)
target_include_directories(dw3texrep PUBLIC include)
target_include_directories(dw3texrep SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/stb)
target_link_libraries(dw3texrep PUBLIC dw3runtime tomlplusplus::tomlplusplus)
dw3_target_defaults(dw3texrep)
set_source_files_properties(src/texrep/stb_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

if(DW3_HAVE_PLUME)
    set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
    set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${SHADER_OUT})
    find_program(DW3_SPIRV_VAL spirv-val)

    # Direct3D 12 takes DXIL: the SPIR-V is converted to HLSL with SPIRV-Cross and compiled with DXC.
    set(DW3_D3D12_SHADERS OFF)
    if(DW3_RENDER_D3D12)
        include(D3D12Tools)
        if(DW3_D3D12_TOOLS_FOUND)
            set(DW3_D3D12_SHADERS ON)
        else()
            message(WARNING "DW3: spirv-cross or dxc not found; the Direct3D 12 backend is disabled")
        endif()
    endif()

    # Metal takes Metal Shading Language: the SPIR-V is converted with SPIRV-Cross and the text is embedded.
    set(DW3_METAL_SHADERS OFF)
    if(DW3_RENDER_METAL)
        include(MetalTools)
        set(DW3_METAL_SHADERS ON)
    endif()

    set(SHADER_HEADERS)
    file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${SHADER_DIR}/post/*.glsl)
    # Lookup textures of the SMAA passes (generated data from the reference implementation, MIT licence).
    foreach(blob smaa_area smaa_search)
        set(blob_hdr ${SHADER_OUT}/${blob}.h)
        add_custom_command(
            OUTPUT ${blob_hdr}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/data/${blob}.bin -DOUTPUT=${blob_hdr} -DSYMBOL=${blob}
                    -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/${blob}.bin ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
            COMMENT "Embedding ${blob}"
            VERBATIM)
        list(APPEND SHADER_HEADERS ${blob_hdr})
    endforeach()
    # name|source|extra glslang options. The display layer variant of the draw shader writes a second target.
    foreach(entry psx_draw.vert|psx_draw.vert psx_draw.frag|psx_draw.frag psx_draw_layer.frag|psx_draw.frag|-DDISPLAY_LAYER
                  present.vert|present.vert present.frag|present.frag osd.vert|osd.vert osd.frag|osd.frag vram_copy.vert|vram_copy.vert vram_copy.frag|vram_copy.frag
                  # Post-processing passes (render/shaders/post).
                  post.vert|post/post.vert post_scale.frag|post/scale.frag post_cas.frag|post/cas.frag
                  post_grade.frag|post/grade.frag post_crt.frag|post/crt.frag post_easu.frag|post/fsr_easu.frag
                  post_rcas.frag|post/fsr_rcas.frag post_smaa_edges.frag|post/smaa_edges.frag
                  post_smaa_weights.frag|post/smaa_weights.frag post_smaa_blend.frag|post/smaa_blend.frag)
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
        set(dxil_cmds)
        set(dxil_hdr)
        if(DW3_D3D12_SHADERS)
            # Vulkan clip space is y-down and Direct3D's is y-up, so vertex shaders are flipped to draw the same image.
            if(shader MATCHES "\\.vert$")
                set(stage vs_6_0)
                set(cross_opts --flip-vert-y)
            else()
                set(stage ps_6_0)
                set(cross_opts)
            endif()
            set(hlsl ${SHADER_OUT}/${shader}.hlsl)
            set(dxil ${SHADER_OUT}/${shader}.dxil)
            set(dxil_hdr ${SHADER_OUT}/${shader}.dxil.h)
            set(dxil_cmds
                COMMAND ${DW3_SPIRV_CROSS} --hlsl --shader-model 60 ${cross_opts} --output ${hlsl} ${spv}
                COMMAND ${DW3_DXC} -nologo -T ${stage} -E main -O3 -Fo ${dxil} ${hlsl}
                COMMAND ${CMAKE_COMMAND} -DINPUT=${dxil} -DOUTPUT=${dxil_hdr} -DSYMBOL=${symbol}_dxil
                        -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake)
        endif()
        set(msl_cmds)
        set(msl_hdr)
        if(DW3_METAL_SHADERS)
            set(msl ${SHADER_OUT}/${shader}.metal)
            set(msl_hdr ${SHADER_OUT}/${shader}.msl.h)
            set(msl_cmds
                COMMAND ${DW3_SPIRV_MSL} ${spv} ${msl}
                COMMAND ${CMAKE_COMMAND} -DINPUT=${msl} -DOUTPUT=${msl_hdr} -DSYMBOL=${symbol}_msl -DTEXT=ON
                        -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake)
        endif()
        add_custom_command(
            OUTPUT ${hdr} ${dxil_hdr} ${msl_hdr}
            COMMAND ${DW3_GLSLANG} -V --target-env vulkan1.2 ${defs} -o ${spv} ${SHADER_DIR}/${source}
            ${validate_cmd}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${hdr} -DSYMBOL=${symbol}_spv
                    -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
            ${dxil_cmds}
            ${msl_cmds}
            DEPENDS ${SHADER_DIR}/${source} ${SHADER_INCLUDES} ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake ${DW3_SPIRV_CROSS_TARGET}
                    ${DW3_SPIRV_MSL}
            COMMENT "Compiling shader ${shader}"
            VERBATIM)
        list(APPEND SHADER_HEADERS ${hdr} ${dxil_hdr} ${msl_hdr})
    endforeach()
    add_custom_target(dw3_shaders DEPENDS ${SHADER_HEADERS})

    target_sources(dw3render PRIVATE src/hw_renderer.cpp src/post_chain.cpp)
    add_dependencies(dw3render dw3_shaders)
    target_include_directories(dw3render PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
   
    target_link_libraries(dw3render PRIVATE plume ${CMAKE_DL_LIBS})
    if(DW3_OS STREQUAL "linux")
        target_link_libraries(dw3render PRIVATE ${X11_LIBRARIES})
        target_include_directories(dw3render SYSTEM PRIVATE ${X11_INCLUDE_DIR})
    endif()
    target_compile_definitions(dw3render PUBLIC DW3_HW_RENDERER=1)
    if(DW3_METAL_SHADERS)
        target_compile_definitions(dw3render PUBLIC DW3_HAVE_METAL=1)
        target_link_libraries(dw3render PRIVATE "-framework Metal" "-framework QuartzCore" "-framework AppKit"
                              "-framework Foundation" "-framework IOKit" "-framework CoreFoundation")
    endif()
    if(DW3_D3D12_SHADERS)
        target_compile_definitions(dw3render PUBLIC DW3_HAVE_D3D12=1)
        target_link_libraries(dw3render PRIVATE d3d12 dxgi)
    endif()
endif()
