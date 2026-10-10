#include "psxdisc/psxexe.h"

#include <cstring>

namespace psxdisc {

std::optional<PsxExe> parse_psxexe(std::span<const uint8_t> file) {
    if (file.size() < kPsxExeHeaderSize || std::memcmp(file.data(), "PS-X EXE", 8) != 0) return std::nullopt;
    auto u32 = [&](std::size_t off) {
        return static_cast<uint32_t>(file[off]) | (static_cast<uint32_t>(file[off + 1]) << 8) |
               (static_cast<uint32_t>(file[off + 2]) << 16) | (static_cast<uint32_t>(file[off + 3]) << 24);
    };
    PsxExe exe;
    exe.pc = u32(0x10);
    exe.gp = u32(0x14);
    exe.text_addr = u32(0x18);
    exe.text_size = u32(0x1C);
    exe.data_addr = u32(0x20);
    exe.data_size = u32(0x24);
    exe.bss_addr = u32(0x28);
    exe.bss_size = u32(0x2C);
    exe.sp_base = u32(0x30);
    exe.sp_offset = u32(0x34);
    if (exe.text_size > file.size() - kPsxExeHeaderSize) return std::nullopt;
    exe.text.assign(file.begin() + kPsxExeHeaderSize, file.begin() + kPsxExeHeaderSize + exe.text_size);
    return exe;
}

}  // namespace psxdisc
