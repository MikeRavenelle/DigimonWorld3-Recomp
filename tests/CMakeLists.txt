include(GoogleTest)

function(dw3_add_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE ${T_LIBS} GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE DW3_GAME_DIR="${DW3_GAME_DIR}")
    dw3_target_defaults(${name})
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
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
            runtime/test_timers.cpp runtime/test_dma.cpp runtime/test_machine.cpp
    LIBS dw3runtime)

# Differential emitter tests: emit_testgen recompiles the programs of psxrecomp/test_programs.h at build
# time; test_emitter runs the emitted C and a test-only reference interpreter side by side.
add_executable(emit_testgen psxrecomp/emit_testgen.cpp)
target_include_directories(emit_testgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp)
target_link_libraries(emit_testgen PRIVATE psxrecomp_core)
dw3_target_defaults(emit_testgen)

set(EMITTED_TESTS_DIR ${CMAKE_CURRENT_BINARY_DIR}/emitted_tests)
set(EMITTED_TESTS_C ${EMITTED_TESTS_DIR}/emitted_funcs.c ${EMITTED_TESTS_DIR}/emitted_tables.c)
add_custom_command(
    OUTPUT ${EMITTED_TESTS_C} ${EMITTED_TESTS_DIR}/emitted_tests.inc
    COMMAND emit_testgen ${EMITTED_TESTS_DIR}
    DEPENDS emit_testgen
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
