#pragma once

// MIPS R3000A (MIPS I, little-endian) instruction decoder for the PS1 CPU.
// Covers the full user ISA plus COP0 (MFC0/MTC0/RFE) and COP2/GTE (MFC2/MTC2/CFC2/CTC2/LWC2/SWC2
// and GTE command words). Unknown encodings decode to Op::Invalid.

#include <cstdint>
#include <string>

namespace psxrecomp {

enum class Op : uint8_t {
    Invalid,
    // SPECIAL
    SLL, SRL, SRA, SLLV, SRLV, SRAV, JR, JALR, SYSCALL, BREAK,
    MFHI, MTHI, MFLO, MTLO, MULT, MULTU, DIV, DIVU,
    ADD, ADDU, SUB, SUBU, AND, OR, XOR, NOR, SLT, SLTU,
    // REGIMM
    BLTZ, BGEZ, BLTZAL, BGEZAL,
    // Jumps / branches
    J, JAL, BEQ, BNE, BLEZ, BGTZ,
    // Immediate ALU
    ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI,
    // COP0
    MFC0, MTC0, RFE,
    // COP2 (GTE)
    MFC2, CFC2, MTC2, CTC2, GTE_CMD,
    // Loads / stores
    LB, LH, LWL, LW, LBU, LHU, LWR, SB, SH, SWL, SW, SWR, LWC2, SWC2,
};

enum class InsnClass : uint8_t {
    Alu,          // register-writing arithmetic/logic
    Branch,       // conditional PC-relative, has delay slot
    Jump,         // J/JAL (absolute target), has delay slot
    JumpReg,      // JR/JALR, has delay slot
    Load,         // memory read into GPR (has load delay)
    Store,
    Cop,          // coprocessor move / command
    System,       // SYSCALL/BREAK/RFE
    Invalid,
};

struct Instruction {
    uint32_t raw = 0;
    uint32_t pc = 0;  // address this word was fetched from
    Op op = Op::Invalid;
    uint8_t rs = 0, rt = 0, rd = 0, shamt = 0;
    int32_t simm = 0;      // sign-extended 16-bit immediate
    uint32_t uimm = 0;     // zero-extended 16-bit immediate
    uint32_t target = 0;   // resolved absolute target for Branch/Jump (0 otherwise)
    uint32_t gte_cmd = 0;  // 25-bit GTE command field for GTE_CMD

    InsnClass klass() const;
    bool has_delay_slot() const;
    bool is_call() const;  // JAL, JALR, BLTZAL, BGEZAL
    // GPR written by this instruction (0 if none; note writes to $zero are discarded).
    uint8_t dest_gpr() const;
};

// Decodes `word` located at address `pc`.
Instruction decode(uint32_t word, uint32_t pc);

// Canonical disassembly, e.g. "addiu $sp, $sp, -24", "jal 0x80012345", "lw $v0, 16($sp)".
// Must match GNU objdump's register naming (no "$" omission) closely enough for golden tests.
std::string disassemble(const Instruction& insn);

const char* gpr_name(uint8_t reg);

}  // namespace psxrecomp
