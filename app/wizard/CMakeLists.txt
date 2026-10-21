# First-run setup wizard. dw3wizard_core is the logic (state machine, disc identification, downloads plan, hashes, unpacking);
# it has no UI or SDL dependency and is unit-tested. dw3wizard_ui is the Dear ImGui window that drives it.

add_library(miniz STATIC ${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(miniz SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/miniz)
target_compile_options(miniz PRIVATE $<$<C_COMPILER_ID:Clang,GNU>:-w> $<$<C_COMPILER_ID:MSVC>:/w>)

add_library(dw3wizard_core STATIC
    sha256.cpp
    disc_check.cpp
    disc_location.cpp
    assets.cpp
    commands.cpp
    zip_extract.cpp
    wizard_state.cpp
    hd_install.cpp
    module_build.cpp)
target_include_directories(dw3wizard_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dw3wizard_core PUBLIC dw3runtime psxdisc psxrecomp_core PRIVATE miniz)
dw3_target_defaults(dw3wizard_core)

set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR})
target_link_libraries(imgui PUBLIC SDL3::SDL3)
target_compile_options(imgui PRIVATE $<$<CXX_COMPILER_ID:Clang,GNU>:-w> $<$<CXX_COMPILER_ID:MSVC>:/w>)

set(WIZARD_FONT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/roboto_medium.h)
add_custom_command(
    OUTPUT ${WIZARD_FONT_HEADER}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${IMGUI_DIR}/fonts/Roboto-Medium.ttf -DOUTPUT=${WIZARD_FONT_HEADER} -DSYMBOL=roboto_medium
            -DNAMESPACE=wizard -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
    DEPENDS ${IMGUI_DIR}/fonts/Roboto-Medium.ttf ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
    COMMENT "Embedding the wizard font"
    VERBATIM)

add_library(dw3wizard_ui STATIC
    wizard_ui.cpp
    process_runner.cpp
    ${WIZARD_FONT_HEADER})
target_include_directories(dw3wizard_ui PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(dw3wizard_ui PUBLIC dw3wizard_core PRIVATE imgui dw3icon dw3texrep dw3runtime SDL3::SDL3)
dw3_target_defaults(dw3wizard_ui)
