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
    SOURCES psxrecomp/test_decoder.cpp psxrecomp/test_golden.cpp
    LIBS psxrecomp_core)

dw3_add_test(test_runtime
    SOURCES runtime/test_bus.cpp runtime/test_platform.cpp runtime/test_gte.cpp
    LIBS dw3runtime)
