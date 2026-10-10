add_library(psxdisc STATIC
    src/disc.cpp
    src/iso9660.cpp
    src/psxexe.cpp)
target_include_directories(psxdisc PUBLIC include)
dw3_target_defaults(psxdisc)
