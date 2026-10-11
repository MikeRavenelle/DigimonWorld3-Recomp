# Boot/run logic shared by the SDL app and the headless scenario runner (no SDL dependency).
add_library(dw3host STATIC host.cpp)
target_include_directories(dw3host PUBLIC include)
target_link_libraries(dw3host PUBLIC dw3runtime psxdisc dw3_generated)
dw3_target_defaults(dw3host)
