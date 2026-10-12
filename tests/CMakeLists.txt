include(GoogleTest)

# Paths given to a Windows test executable that runs under Wine: Wine maps the Linux root to drive Z:.
function(dw3_test_path out path)
    if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows" AND path MATCHES "^/")
        set(path "Z:${path}")
    endif()
    set(${out} "${path}" PARENT_SCOPE)
endfunction()
dw3_test_path(DW3_TEST_GAME_DIR "${DW3_GAME_DIR}")

function(dw3_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${T_LIBS} GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE DW3_GAME_DIR="${DW3_TEST_GAME_DIR}")
    dw3_target_defaults(${name})
    if(CMAKE_CROSSCOMPILING)
        # The executable cannot be run at build time to list its tests; register it as a single test.
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 1800)
    else()
        gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
    endif()
endfunction()

dw3_add_test(test_psxdisc
    SOURCES psxdisc/test_cue.cpp psxdisc/test_iso9660.cpp psxdisc/test_psxexe.cpp psxdisc/test_real_disc.cpp
    LIBS psxdisc)

dw3_add_test(test_psxrecomp
    SOURCES psxrecomp/test_decoder.cpp psxrecomp/test_golden.cpp psxrecomp/test_analysis.cpp
            psxrecomp/test_analysis_real.cpp psxrecomp/test_emitter_text.cpp
    LIBS psxrecomp_core psxdisc)

dw3_add_test(test_runtime
    SOURCES runtime/test_bus.cpp runtime/test_platform.cpp runtime/test_gte.cpp
            runtime/test_scheduler.cpp runtime/test_irq.cpp runtime/test_video_timing.cpp
            runtime/test_timers.cpp runtime/test_dma.cpp runtime/test_cdrom.cpp runtime/test_machine.cpp runtime/test_spu.cpp
            runtime/test_bios.cpp runtime/test_state.cpp runtime/test_state_bios.cpp runtime/test_state_core.cpp
            runtime/test_state_cdrom.cpp runtime/test_state_sio.cpp runtime/test_state_spu.cpp runtime/test_soft_rasterizer.cpp runtime/test_sio.cpp
            runtime/test_mdec.cpp runtime/test_memcard.cpp runtime/test_disc_ref.cpp
            runtime/test_spu_dsp.cpp runtime/test_spu_voices.cpp runtime/test_xa.cpp
            runtime/test_present_layout.cpp runtime/test_display_snapshot.cpp runtime/test_precise_vertex.cpp
    LIBS dw3runtime)

# Differential emitter tests: emit_testgen recompiles the programs of psxrecomp/test_programs.h at build
# time; test_emitter runs the emitted C and a test-only reference interpreter side by side.
add_executable(emit_testgen psxrecomp/emit_testgen.cpp)
target_include_directories(emit_testgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp)
target_link_libraries(emit_testgen PRIVATE psxrecomp_core)
dw3_target_defaults(emit_testgen)
# When cross-compiling, the generator cannot run here: use the one from a native build (its output is the same C).
set(EMIT_TESTGEN_COMMAND emit_testgen)
if(CMAKE_CROSSCOMPILING)
    set(DW3_HOST_EMIT_TESTGEN "${PROJECT_SOURCE_DIR}/build/linux-x64-release/tests/emit_testgen" CACHE FILEPATH
        "emit_testgen built for the build machine (used when cross-compiling)")
    set(EMIT_TESTGEN_COMMAND ${DW3_HOST_EMIT_TESTGEN})
endif()

set(EMITTED_TESTS_DIR ${CMAKE_CURRENT_BINARY_DIR}/emitted_tests)
set(EMITTED_TESTS_C ${EMITTED_TESTS_DIR}/emitted_funcs.c ${EMITTED_TESTS_DIR}/emitted_tables.c)
add_custom_command(
    OUTPUT ${EMITTED_TESTS_C} ${EMITTED_TESTS_DIR}/emitted_tests.inc
    COMMAND ${EMIT_TESTGEN_COMMAND} ${EMITTED_TESTS_DIR}
    DEPENDS ${EMIT_TESTGEN_COMMAND}
    COMMENT "Recompiling emitter test programs")
set_source_files_properties(${EMITTED_TESTS_C} PROPERTIES GENERATED TRUE
    COMPILE_OPTIONS "$<$<C_COMPILER_ID:Clang,GNU>:-w>;$<$<C_COMPILER_ID:MSVC>:/w>")
set_source_files_properties(psxrecomp/test_emitter.cpp PROPERTIES
    OBJECT_DEPENDS ${EMITTED_TESTS_DIR}/emitted_tests.inc)

dw3_add_test(test_emitter
    SOURCES psxrecomp/test_emitter.cpp psxrecomp/ref_interp.cpp ${EMITTED_TESTS_C}
    LIBS dw3runtime)
target_include_directories(test_emitter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp ${EMITTED_TESTS_DIR}
    ${PROJECT_SOURCE_DIR}/tools/psxrecomp/include)  # header-only use of CodeSegment via mips_asm.h

dw3_add_test(test_render
    SOURCES render/test_vram_regions.cpp render/test_vertex_builder.cpp render/test_hw_renderer.cpp
    LIBS dw3render)

dw3_add_test(test_scenario_format
    SOURCES scenario/test_scenario_format.cpp
    LIBS dw3scenario_core)

# In-game scenarios: one ctest test per tests/scenarios/*.toml, run headless by dw3scenario (needs the recompiled
# code and the disc image; exits 77 = SKIP without the disc). Scenarios run ~6x faster than real time in the
# release build, so the fast path is:  ctest --preset linux-x64-release -L scenario
# (Debug works but is far slower.) Screenshots land in game/audit/scenarios/<name>/ (git-ignored).
if(TARGET dw3scenario)
    file(GLOB DW3_SCENARIO_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scenarios/*.toml")
    foreach(scenario_file ${DW3_SCENARIO_FILES})
        get_filename_component(scenario_name "${scenario_file}" NAME_WE)
        dw3_test_path(scenario_arg "${scenario_file}")
        dw3_test_path(state_dir_arg "${CMAKE_BINARY_DIR}/states")
        add_test(NAME scenario.${scenario_name}
            COMMAND dw3scenario "${scenario_arg}" --disc "${DW3_TEST_GAME_DIR}/Digimon World 3 (USA).cue"
                    --state-dir "${state_dir_arg}")
        set_tests_properties(scenario.${scenario_name} PROPERTIES
            LABELS scenario SKIP_RETURN_CODE 77 TIMEOUT 1800)
        # Save states are build products (they contain game data): a scenario that writes one is the fixture setup
        # for the scenarios that start from it, so ctest runs it first.
        file(READ "${scenario_file}" scenario_text)
        string(REGEX MATCHALL "\\[\\[savestate\\]\\][^\\[]*path *= *\"[^\"]+\"" saved_states "${scenario_text}")
        set(setup_fixtures "")
        foreach(entry ${saved_states})
            string(REGEX REPLACE ".*path *= *\"([^\"]+)\"" "\\1" state_file "${entry}")
            list(APPEND setup_fixtures "state_${state_file}")
        endforeach()
        if(setup_fixtures)
            set_tests_properties(scenario.${scenario_name} PROPERTIES FIXTURES_SETUP "${setup_fixtures}")
        endif()
        if(scenario_text MATCHES "start_state *= *\"([^\"]+)\"")
            set_tests_properties(scenario.${scenario_name} PROPERTIES FIXTURES_REQUIRED "state_${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()

# On-screen notification text rasterization (app/present/osd.cpp has no SDL dependency).
dw3_add_test(test_osd
    SOURCES app/test_osd.cpp ${PROJECT_SOURCE_DIR}/app/present/osd.cpp
    LIBS dw3runtime)
target_include_directories(test_osd PRIVATE ${PROJECT_SOURCE_DIR}/app)
