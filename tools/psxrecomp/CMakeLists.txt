add_library(psxrecomp_core STATIC
    src/mips.cpp
    src/analysis.cpp
    src/emitter.cpp
    src/pipeline.cpp)
target_include_directories(psxrecomp_core PUBLIC include)
target_link_libraries(psxrecomp_core PUBLIC psxdisc tomlplusplus::tomlplusplus)
dw3_target_defaults(psxrecomp_core)

add_executable(psxrecomp src/main.cpp)
target_link_libraries(psxrecomp PRIVATE psxrecomp_core psxdisc tomlplusplus::tomlplusplus)
dw3_target_defaults(psxrecomp)
