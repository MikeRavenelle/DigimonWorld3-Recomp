# Script mode: cmake -DINPUT=<file> -DOUTPUT=<header> -DSYMBOL=<name> -P EmbedBinary.cmake
# Writes the file as a 4-byte aligned byte array so that SPIR-V can be handed to the graphics API directly.
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_len)
math(EXPR size "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}"
    "// Generated from ${INPUT}; do not edit.\n"
    "#pragma once\n#include <cstddef>\n#include <cstdint>\n\n"
    "namespace dw3::shaders {\n"
    "alignas(4) inline constexpr uint8_t ${SYMBOL}[] = {\n${bytes}\n};\n"
    "inline constexpr size_t ${SYMBOL}_size = ${size};\n"
    "}  // namespace dw3::shaders\n")
