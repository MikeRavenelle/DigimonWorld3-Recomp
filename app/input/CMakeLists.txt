# Host input model: bindings, stick maths, pad selection and button prompts. No SDL dependency; unit-tested.
add_library(dw3input STATIC bindings.cpp glyphs.cpp)
target_include_directories(dw3input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dw3input PUBLIC dw3runtime)
dw3_target_defaults(dw3input)
