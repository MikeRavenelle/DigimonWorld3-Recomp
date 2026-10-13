add_executable(texpack main.cpp)
target_link_libraries(texpack PRIVATE dw3texrep dw3runtime)
dw3_target_defaults(texpack)
