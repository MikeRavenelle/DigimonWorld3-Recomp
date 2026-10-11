# Script mode: cmake -DPATCH=<file> -P ApplyPatch.cmake   (run in the source directory to patch)
# Applies the patch unless it is already applied, so a repeated populate step is harmless.
execute_process(COMMAND git apply --reverse --check "${PATCH}" RESULT_VARIABLE already
    OUTPUT_QUIET ERROR_QUIET)
if(already EQUAL 0)
    return()
endif()
execute_process(COMMAND git apply "${PATCH}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed to apply ${PATCH}")
endif()
