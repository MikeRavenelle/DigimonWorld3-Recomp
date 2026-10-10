#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace psxdisc {

// PS-X EXE header (first 0x800 bytes of the boot executable).
struct PsxExe {
    uint32_t pc = 0;
    uint32_t gp = 0;
    uint32_t text_addr = 0;
    uint32_t text_size = 0;
    uint32_t data_addr = 0;
    uint32_t data_size = 0;
    uint32_t bss_addr = 0;
    uint32_t bss_size = 0;
    uint32_t sp_base = 0;
    uint32_t sp_offset = 0;
    std::vector<uint8_t> text;  // text_size bytes loaded at text_addr
};

constexpr std::size_t kPsxExeHeaderSize = 0x800;

std::optional<PsxExe> parse_psxexe(std::span<const uint8_t> file);

}  // namespace psxdisc
