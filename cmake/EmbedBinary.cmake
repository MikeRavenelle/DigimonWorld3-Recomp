# Script mode: cmake -DINPUT=<file> -DOUTPUT=<header> -DSYMBOL=<name> -P EmbedBinary.cmake
# Writes the file as a 4-byte aligned byte array so that SPIR-V can be handed to the graphics API directly.
# With -DNAMESPACE=<name> the array is declared in dw3::<name> instead of dw3::shaders.
# With -DTEXT=ON a terminating NUL is appended (and counted in the size), for shader source text.
if(NOT NAMESPACE)
    set(NAMESPACE shaders)
endif()
file(READ "${INPUT}" content HEX)
if(TEXT)
    string(APPEND content "00")
endif()
string(LENGTH "${content}" hex_len)
math(EXPR size "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}"
    "// Generated from ${INPUT}; do not edit.\n"
    "#pragma once\n#include <cstddef>\n#include <cstdint>\n\n"
    "namespace dw3::${NAMESPACE} {\n"
    "alignas(4) inline constexpr uint8_t ${SYMBOL}[] = {\n${bytes}\n};\n"
    "inline constexpr size_t ${SYMBOL}_size = ${size};\n"
    "}  // namespace dw3::${NAMESPACE}\n")
