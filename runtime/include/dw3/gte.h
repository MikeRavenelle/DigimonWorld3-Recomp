#pragma once

// PS1 Geometry Transformation Engine (COP2).
// Register numbering follows psx-spx: data regs 0-31 (cop2r0-31), control regs 0-31 (cop2r32-63).

#include <cstdint>

namespace dw3 {

class Gte {
public:
    // MFC2 / MTC2 (with all read/write side effects: SXYP FIFO push, IRGB/ORGB, LZCS/LZCR, etc.)
    uint32_t read_data(unsigned reg) const;
    void write_data(unsigned reg, uint32_t value);
    // CFC2 / CTC2 (with sign-extension quirks of 16-bit control regs)
    uint32_t read_ctrl(unsigned reg) const;
    void write_ctrl(unsigned reg, uint32_t value);

    // Executes a COP2 command word (bits 0-24 of the instruction). Updates FLAG (ctrl 31).
    void execute(uint32_t command);

    // Approximate cycle cost of a command, for scheduler accounting.
    static int command_cycles(uint32_t command);

    void reset();

private:
    friend struct GteImpl;

    uint32_t data_[32]{};
    uint32_t ctrl_[32]{};
    uint32_t cmd_ = 0;
};

}  // namespace dw3
