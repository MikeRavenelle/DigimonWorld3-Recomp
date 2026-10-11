// Decoder / disassembler tests.
// Expected disassembly strings follow `mips64-linux-gnu-objdump -D -b binary -m mips:3000 -EL` (whitespace
// normalized to a single space between mnemonic and operands).

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "psxrecomp/mips.h"

using namespace psxrecomp;

namespace {

// Encoders ---------------------------------------------------------------------------------------------------
constexpr uint32_t R(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh, uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | funct;
}
constexpr uint32_t I(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF);
}
constexpr uint32_t J(uint32_t op, uint32_t index) { return (op << 26) | (index & 0x3FFFFFF); }

// Register numbers used in tests.
constexpr uint32_t ZERO = 0, AT = 1, V0 = 2, V1 = 3, A0 = 4, A1 = 5, A2 = 6, A3 = 7, T0 = 8, T1 = 9, S0 = 16, S1 = 17,
                   T8 = 24, GP = 28, SP = 29, S8 = 30, RA = 31;

constexpr uint32_t kPc = 0x80010000;

struct Case {
    uint32_t word;
    uint32_t pc;
    Op op;
    InsnClass klass;
    bool delay;
    bool call;
    uint8_t dest;
    const char* text;
};

std::ostream& operator<<(std::ostream& os, const Case& c) { return os << std::hex << "0x" << c.word << " " << c.text; }

constexpr bool N = false, Y = true;

const std::vector<Case>& cases() {
    static const std::vector<Case> v = {
        // ---- SPECIAL shifts
        {0x00000000, kPc, Op::SLL, InsnClass::Alu, N, N, 0, "nop"},
        {0x00000040, kPc, Op::SLL, InsnClass::Alu, N, N, 0, "ssnop"},
        {0x000000C0, kPc, Op::SLL, InsnClass::Alu, N, N, 0, "ehb"},
        {0x00000080, kPc, Op::SLL, InsnClass::Alu, N, N, 0, "sll zero,zero,0x2"},
        {R(0, V0, V0, 1, 0x00), kPc, Op::SLL, InsnClass::Alu, N, N, V0, "sll v0,v0,0x1"},
        {R(0, A0, V1, 31, 0x00), kPc, Op::SLL, InsnClass::Alu, N, N, V1, "sll v1,a0,0x1f"},
        {R(0, A0, ZERO, 3, 0x00), kPc, Op::SLL, InsnClass::Alu, N, N, 0, "sll zero,a0,0x3"},
        {R(0, A0, A0, 0x10, 0x02), kPc, Op::SRL, InsnClass::Alu, N, N, A0, "srl a0,a0,0x10"},
        {R(0, T0, T1, 0, 0x02), kPc, Op::SRL, InsnClass::Alu, N, N, T1, "srl t1,t0,0x0"},
        {R(0, A0, A0, 0x18, 0x03), kPc, Op::SRA, InsnClass::Alu, N, N, A0, "sra a0,a0,0x18"},
        {R(0, V0, V0, 31, 0x03), kPc, Op::SRA, InsnClass::Alu, N, N, V0, "sra v0,v0,0x1f"},
        {R(A3, V0, ZERO, 0, 0x04), kPc, Op::SLLV, InsnClass::Alu, N, N, 0, "sllv zero,v0,a3"},
        {R(A1, V0, A1, 0, 0x04), kPc, Op::SLLV, InsnClass::Alu, N, N, A1, "sllv a1,v0,a1"},
        {R(A3, V0, V1, 0, 0x06), kPc, Op::SRLV, InsnClass::Alu, N, N, V1, "srlv v1,v0,a3"},
        {R(A3, V0, T8, 0, 0x07), kPc, Op::SRAV, InsnClass::Alu, N, N, T8, "srav t8,v0,a3"},
        // ---- jumps via register
        {R(RA, 0, 0, 0, 0x08), kPc, Op::JR, InsnClass::JumpReg, Y, N, 0, "jr ra"},
        {R(AT, 0, 0, 0, 0x08), kPc, Op::JR, InsnClass::JumpReg, Y, N, 0, "jr at"},
        {R(V0, 0, RA, 0, 0x09), kPc, Op::JALR, InsnClass::JumpReg, Y, Y, RA, "jalr v0"},
        {R(A0, 0, RA, 0, 0x09), kPc, Op::JALR, InsnClass::JumpReg, Y, Y, RA, "jalr a0"},
        {R(RA, 0, ZERO, 0, 0x09), kPc, Op::JALR, InsnClass::JumpReg, Y, Y, 0, "jalr zero,ra"},
        {R(A0, 0, S0, 0, 0x09), kPc, Op::JALR, InsnClass::JumpReg, Y, Y, S0, "jalr s0,a0"},
        // ---- system
        {0x0000000C, kPc, Op::SYSCALL, InsnClass::System, N, N, 0, "syscall"},
        {0x00003E4C, kPc, Op::SYSCALL, InsnClass::System, N, N, 0, "syscall 0xf9"},
        {0x0040000C, kPc, Op::SYSCALL, InsnClass::System, N, N, 0, "syscall 0x10000"},
        {0x0000000D, kPc, Op::BREAK, InsnClass::System, N, N, 0, "break"},
        {0x0007000D, kPc, Op::BREAK, InsnClass::System, N, N, 0, "break 0x7"},
        {0x0000004D, kPc, Op::BREAK, InsnClass::System, N, N, 0, "break 0x0,0x1"},
        {0x0000040D, kPc, Op::BREAK, InsnClass::System, N, N, 0, "break 0x0,0x10"},
        {0x0007004D, kPc, Op::BREAK, InsnClass::System, N, N, 0, "break 0x7,0x1"},
        // ---- HI/LO
        {R(0, 0, A0, 0, 0x10), kPc, Op::MFHI, InsnClass::Alu, N, N, A0, "mfhi a0"},
        {R(0, 0, ZERO, 0, 0x10), kPc, Op::MFHI, InsnClass::Alu, N, N, 0, "mfhi zero"},
        {R(A3, 0, 0, 0, 0x11), kPc, Op::MTHI, InsnClass::Alu, N, N, 0, "mthi a3"},
        {R(0, 0, V0, 0, 0x12), kPc, Op::MFLO, InsnClass::Alu, N, N, V0, "mflo v0"},
        {R(A3, 0, 0, 0, 0x13), kPc, Op::MTLO, InsnClass::Alu, N, N, 0, "mtlo a3"},
        {R(A0, A1, 0, 0, 0x18), kPc, Op::MULT, InsnClass::Alu, N, N, 0, "mult a0,a1"},
        {R(A3, ZERO, 0, 0, 0x19), kPc, Op::MULTU, InsnClass::Alu, N, N, 0, "multu a3,zero"},
        {R(A0, T0, 0, 0, 0x1A), kPc, Op::DIV, InsnClass::Alu, N, N, 0, "div zero,a0,t0"},
        {R(V0, V1, 0, 0, 0x1B), kPc, Op::DIVU, InsnClass::Alu, N, N, 0, "divu zero,v0,v1"},
        // ---- three-register ALU
        {R(T0, T1, V0, 0, 0x20), kPc, Op::ADD, InsnClass::Alu, N, N, V0, "add v0,t0,t1"},
        {R(ZERO, AT, A0, 0, 0x20), kPc, Op::ADD, InsnClass::Alu, N, N, A0, "add a0,zero,at"},
        {R(V1, ZERO, ZERO, 0, 0x20), kPc, Op::ADD, InsnClass::Alu, N, N, 0, "add zero,v1,zero"},
        {R(A0, A1, V0, 0, 0x21), kPc, Op::ADDU, InsnClass::Alu, N, N, V0, "addu v0,a0,a1"},
        {R(ZERO, V1, V0, 0, 0x21), kPc, Op::ADDU, InsnClass::Alu, N, N, V0, "addu v0,zero,v1"},
        {R(SP, ZERO, S8, 0, 0x21), kPc, Op::ADDU, InsnClass::Alu, N, N, S8, "move s8,sp"},
        {R(AT, ZERO, ZERO, 0, 0x21), kPc, Op::ADDU, InsnClass::Alu, N, N, 0, "move zero,at"},
        {R(A1, A2, T1, 0, 0x22), kPc, Op::SUB, InsnClass::Alu, N, N, T1, "sub t1,a1,a2"},
        {R(ZERO, V1, V0, 0, 0x22), kPc, Op::SUB, InsnClass::Alu, N, N, V0, "neg v0,v1"},
        {R(A1, A2, T1, 0, 0x23), kPc, Op::SUBU, InsnClass::Alu, N, N, T1, "subu t1,a1,a2"},
        {R(ZERO, V0, V0, 0, 0x23), kPc, Op::SUBU, InsnClass::Alu, N, N, V0, "negu v0,v0"},
        {R(V1, ZERO, V0, 0, 0x23), kPc, Op::SUBU, InsnClass::Alu, N, N, V0, "subu v0,v1,zero"},
        {R(A0, A1, V0, 0, 0x24), kPc, Op::AND, InsnClass::Alu, N, N, V0, "and v0,a0,a1"},
        {R(A0, A1, V0, 0, 0x25), kPc, Op::OR, InsnClass::Alu, N, N, V0, "or v0,a0,a1"},
        {R(ZERO, A1, V0, 0, 0x25), kPc, Op::OR, InsnClass::Alu, N, N, V0, "or v0,zero,a1"},
        {R(A0, ZERO, S0, 0, 0x25), kPc, Op::OR, InsnClass::Alu, N, N, S0, "move s0,a0"},
        {R(ZERO, ZERO, V1, 0, 0x25), kPc, Op::OR, InsnClass::Alu, N, N, V1, "move v1,zero"},
        {R(A0, A1, V0, 0, 0x26), kPc, Op::XOR, InsnClass::Alu, N, N, V0, "xor v0,a0,a1"},
        {R(A0, ZERO, A0, 0, 0x27), kPc, Op::NOR, InsnClass::Alu, N, N, A0, "nor a0,a0,zero"},
        {R(A0, A1, A0, 0, 0x27), kPc, Op::NOR, InsnClass::Alu, N, N, A0, "nor a0,a0,a1"},
        {R(A0, A1, V0, 0, 0x2A), kPc, Op::SLT, InsnClass::Alu, N, N, V0, "slt v0,a0,a1"},
        {R(ZERO, V0, V1, 0, 0x2B), kPc, Op::SLTU, InsnClass::Alu, N, N, V1, "sltu v1,zero,v0"},
        {R(A0, A1, V0, 0, 0x2B), kPc, Op::SLTU, InsnClass::Alu, N, N, V0, "sltu v0,a0,a1"},
        // ---- REGIMM (pc = 0x80010000, branch target = pc + 4 + off*4)
        {I(1, A0, 0x00, 0x000F), kPc, Op::BLTZ, InsnClass::Branch, Y, N, 0, "bltz a0,0x80010040"},
        {I(1, A0, 0x01, 0x0002), kPc, Op::BGEZ, InsnClass::Branch, Y, N, 0, "bgez a0,0x8001000c"},
        {I(1, A0, 0x01, 0xFFFD), kPc, Op::BGEZ, InsnClass::Branch, Y, N, 0, "bgez a0,0x8000fff8"},
        {I(1, A1, 0x10, 0x0960), kPc, Op::BLTZAL, InsnClass::Branch, Y, Y, RA, "bltzal a1,0x80012584"},
        {I(1, A1, 0x11, 0x005F), kPc, Op::BGEZAL, InsnClass::Branch, Y, Y, RA, "bgezal a1,0x80010180"},
        {I(1, ZERO, 0x10, 0x0001), kPc, Op::BLTZAL, InsnClass::Branch, Y, Y, RA, "bltzal zero,0x80010008"},
        {I(1, ZERO, 0x11, 0x0001), kPc, Op::BGEZAL, InsnClass::Branch, Y, Y, RA, "bal 0x80010008"},
        {I(1, ZERO, 0x01, 0x0000), kPc, Op::BGEZ, InsnClass::Branch, Y, N, 0, "b 0x80010004"},
        {I(1, ZERO, 0x00, 0xFFFF), kPc, Op::BLTZ, InsnClass::Branch, Y, N, 0, "bltz zero,0x80010000"},
        // ---- branches
        {I(4, A1, ZERO, 0x000D), kPc, Op::BEQ, InsnClass::Branch, Y, N, 0, "beqz a1,0x80010038"},
        {I(4, ZERO, ZERO, 0x0003), kPc, Op::BEQ, InsnClass::Branch, Y, N, 0, "b 0x80010010"},
        {I(4, A0, A1, 0x0010), kPc, Op::BEQ, InsnClass::Branch, Y, N, 0, "beq a0,a1,0x80010044"},
        {I(4, ZERO, V0, 0x0000), kPc, Op::BEQ, InsnClass::Branch, Y, N, 0, "beq zero,v0,0x80010004"},
        {I(5, A0, ZERO, 0xFFFC), kPc, Op::BNE, InsnClass::Branch, Y, N, 0, "bnez a0,0x8000fff4"},
        {I(5, A0, A1, 0x0004), kPc, Op::BNE, InsnClass::Branch, Y, N, 0, "bne a0,a1,0x80010014"},
        {I(5, ZERO, ZERO, 0x0001), kPc, Op::BNE, InsnClass::Branch, Y, N, 0, "bnez zero,0x80010008"},
        {I(6, A0, 0, 0x0008), kPc, Op::BLEZ, InsnClass::Branch, Y, N, 0, "blez a0,0x80010024"},
        {I(6, ZERO, 0, 0xFFFF), kPc, Op::BLEZ, InsnClass::Branch, Y, N, 0, "blez zero,0x80010000"},
        {I(7, A0, 0, 0x0003), kPc, Op::BGTZ, InsnClass::Branch, Y, N, 0, "bgtz a0,0x80010010"},
        {I(7, V0, 0, 0xFFFE), kPc, Op::BGTZ, InsnClass::Branch, Y, N, 0, "bgtz v0,0x8000fffc"},
        // ---- J / JAL
        {J(2, 0x00000A), kPc, Op::J, InsnClass::Jump, Y, N, 0, "j 0x80000028"},
        {J(2, 0x0C0000), 0x80010000, Op::J, InsnClass::Jump, Y, N, 0, "j 0x80300000"},
        {J(3, 0x00010C), kPc, Op::JAL, InsnClass::Jump, Y, Y, RA, "jal 0x80000430"},
        {J(3, 0x3FFFFFF), kPc, Op::JAL, InsnClass::Jump, Y, Y, RA, "jal 0x8ffffffc"},
        {J(3, 0x000000), kPc, Op::JAL, InsnClass::Jump, Y, Y, RA, "jal 0x80000000"},
        // ---- immediate ALU
        {I(8, A0, A0, 4), kPc, Op::ADDI, InsnClass::Alu, N, N, A0, "addi a0,a0,4"},
        {I(8, T0, A0, 0xFFFF), kPc, Op::ADDI, InsnClass::Alu, N, N, A0, "addi a0,t0,-1"},
        {I(8, ZERO, ZERO, 1), kPc, Op::ADDI, InsnClass::Alu, N, N, 0, "addi zero,zero,1"},
        {I(8, ZERO, V0, 5), kPc, Op::ADDI, InsnClass::Alu, N, N, V0, "addi v0,zero,5"},
        {I(9, SP, SP, 0xFFE8), kPc, Op::ADDIU, InsnClass::Alu, N, N, SP, "addiu sp,sp,-24"},
        {I(9, SP, SP, 0x0018), kPc, Op::ADDIU, InsnClass::Alu, N, N, SP, "addiu sp,sp,24"},
        {I(9, A0, A0, 0x8000), kPc, Op::ADDIU, InsnClass::Alu, N, N, A0, "addiu a0,a0,-32768"},
        {I(9, A0, A0, 0x7FFF), kPc, Op::ADDIU, InsnClass::Alu, N, N, A0, "addiu a0,a0,32767"},
        {I(9, ZERO, V0, 4), kPc, Op::ADDIU, InsnClass::Alu, N, N, V0, "li v0,4"},
        {I(9, ZERO, V0, 0xFFFF), kPc, Op::ADDIU, InsnClass::Alu, N, N, V0, "li v0,-1"},
        {I(9, ZERO, ZERO, 0xFFFF), kPc, Op::ADDIU, InsnClass::Alu, N, N, 0, "li zero,-1"},
        {I(10, A0, V0, 0x801), kPc, Op::SLTI, InsnClass::Alu, N, N, V0, "slti v0,a0,2049"},
        {I(10, A0, V0, 0xFFFF), kPc, Op::SLTI, InsnClass::Alu, N, N, V0, "slti v0,a0,-1"},
        {I(10, ZERO, AT, 1), kPc, Op::SLTI, InsnClass::Alu, N, N, AT, "slti at,zero,1"},
        {I(11, A0, V0, 0x80), kPc, Op::SLTIU, InsnClass::Alu, N, N, V0, "sltiu v0,a0,128"},
        {I(11, ZERO, V0, 0xFFFF), kPc, Op::SLTIU, InsnClass::Alu, N, N, V0, "sltiu v0,zero,-1"},
        {I(12, A0, A0, 0x100), kPc, Op::ANDI, InsnClass::Alu, N, N, A0, "andi a0,a0,0x100"},
        {I(12, A0, V0, 0xFFFF), kPc, Op::ANDI, InsnClass::Alu, N, N, V0, "andi v0,a0,0xffff"},
        {I(12, ZERO, AT, 1), kPc, Op::ANDI, InsnClass::Alu, N, N, AT, "andi at,zero,0x1"},
        {I(13, A0, A0, 1), kPc, Op::ORI, InsnClass::Alu, N, N, A0, "ori a0,a0,0x1"},
        {I(13, ZERO, A0, 0x8000), kPc, Op::ORI, InsnClass::Alu, N, N, A0, "li a0,0x8000"},
        {I(13, ZERO, V0, 0xFFFF), kPc, Op::ORI, InsnClass::Alu, N, N, V0, "li v0,0xffff"},
        {I(13, ZERO, V0, 0), kPc, Op::ORI, InsnClass::Alu, N, N, V0, "li v0,0x0"},
        {I(14, S0, V0, 0x4924), kPc, Op::XORI, InsnClass::Alu, N, N, V0, "xori v0,s0,0x4924"},
        {I(14, ZERO, AT, 1), kPc, Op::XORI, InsnClass::Alu, N, N, AT, "xori at,zero,0x1"},
        {I(15, 0, V0, 0x8006), kPc, Op::LUI, InsnClass::Alu, N, N, V0, "lui v0,0x8006"},
        {I(15, 0, AT, 0x0001), kPc, Op::LUI, InsnClass::Alu, N, N, AT, "lui at,0x1"},
        {I(15, 0, ZERO, 0xFFFF), kPc, Op::LUI, InsnClass::Alu, N, N, 0, "lui zero,0xffff"},
        // ---- COP0
        {0x40026000, kPc, Op::MFC0, InsnClass::Cop, N, N, V0, "mfc0 v0,c0_sr"},
        {0x40026800, kPc, Op::MFC0, InsnClass::Cop, N, N, V0, "mfc0 v0,c0_cause"},
        {0x40037000, kPc, Op::MFC0, InsnClass::Cop, N, N, V1, "mfc0 v1,c0_epc"},
        {0x40087800, kPc, Op::MFC0, InsnClass::Cop, N, N, T0, "mfc0 t0,c0_prid"},
        {0x40084000, kPc, Op::MFC0, InsnClass::Cop, N, N, T0, "mfc0 t0,c0_badvaddr"},
        {0x40084800, kPc, Op::MFC0, InsnClass::Cop, N, N, T0, "mfc0 t0,$9"},
        {0x4008F800, kPc, Op::MFC0, InsnClass::Cop, N, N, T0, "mfc0 t0,$31"},
        {0x40880000, kPc, Op::MTC0, InsnClass::Cop, N, N, 0, "mtc0 t0,c0_index"},
        {0x40826000, kPc, Op::MTC0, InsnClass::Cop, N, N, 0, "mtc0 v0,c0_sr"},
        {0x42000010, kPc, Op::RFE, InsnClass::System, N, N, 0, "rfe"},
        // ---- COP2 moves
        {0x48044800, kPc, Op::MFC2, InsnClass::Cop, N, N, A0, "mfc2 a0,$9"},
        {0x4802F800, kPc, Op::MFC2, InsnClass::Cop, N, N, V0, "mfc2 v0,$31"},
        {0x48440000, kPc, Op::CFC2, InsnClass::Cop, N, N, A0, "cfc2 a0,$0"},
        {0x48440800, kPc, Op::CFC2, InsnClass::Cop, N, N, A0, "cfc2 a0,$1"},
        {0x4884F000, kPc, Op::MTC2, InsnClass::Cop, N, N, 0, "mtc2 a0,$30"},
        {0x48874800, kPc, Op::MTC2, InsnClass::Cop, N, N, 0, "mtc2 a3,$9"},
        {0x48C46800, kPc, Op::CTC2, InsnClass::Cop, N, N, 0, "ctc2 a0,$13"},
        {0x48C4F800, kPc, Op::CTC2, InsnClass::Cop, N, N, 0, "ctc2 a0,$31"},
        // ---- GTE commands
        {0x4A41E012, kPc, Op::GTE_CMD, InsnClass::Cop, N, N, 0, "c2 0x41e012"},
        {0x4A400001, kPc, Op::GTE_CMD, InsnClass::Cop, N, N, 0, "c2 0x400001"},
        {0x4A000000, kPc, Op::GTE_CMD, InsnClass::Cop, N, N, 0, "c2 0x0"},
        {0x4BFFFFFF, kPc, Op::GTE_CMD, InsnClass::Cop, N, N, 0, "c2 0x1ffffff"},
        {0x4B8FFFFF, kPc, Op::GTE_CMD, InsnClass::Cop, N, N, 0, "c2 0x18fffff"},
        {0x4A180001, kPc, Op::GTE_CMD, InsnClass::Cop, N, N, 0, "c2 0x180001"},
        // ---- loads / stores
        {I(0x20, ZERO, 1, 0x1168), kPc, Op::LB, InsnClass::Load, N, N, AT, "lb at,4456(zero)"},
        {I(0x20, A0, V0, 0xFFFF), kPc, Op::LB, InsnClass::Load, N, N, V0, "lb v0,-1(a0)"},
        {I(0x21, A0, V0, 0x0004), kPc, Op::LH, InsnClass::Load, N, N, V0, "lh v0,4(a0)"},
        {I(0x22, A0, V0, 0x0003), kPc, Op::LWL, InsnClass::Load, N, N, V0, "lwl v0,3(a0)"},
        {I(0x23, SP, A0, 0x0010), kPc, Op::LW, InsnClass::Load, N, N, A0, "lw a0,16(sp)"},
        {I(0x23, A0, V0, 0x8000), kPc, Op::LW, InsnClass::Load, N, N, V0, "lw v0,-32768(a0)"},
        {I(0x23, A0, ZERO, 0), kPc, Op::LW, InsnClass::Load, N, N, 0, "lw zero,0(a0)"},
        {I(0x24, A0, V0, 0x0009), kPc, Op::LBU, InsnClass::Load, N, N, V0, "lbu v0,9(a0)"},
        {I(0x25, V0, V1, 0x0022), kPc, Op::LHU, InsnClass::Load, N, N, V1, "lhu v1,34(v0)"},
        {I(0x26, A0, V0, 0x0003), kPc, Op::LWR, InsnClass::Load, N, N, V0, "lwr v0,3(a0)"},
        {I(0x28, A0, V0, 0x0000), kPc, Op::SB, InsnClass::Store, N, N, 0, "sb v0,0(a0)"},
        {I(0x29, A0, V0, 0x0002), kPc, Op::SH, InsnClass::Store, N, N, 0, "sh v0,2(a0)"},
        {I(0x2A, A0, V0, 0x0003), kPc, Op::SWL, InsnClass::Store, N, N, 0, "swl v0,3(a0)"},
        {I(0x2B, SP, RA, 0x0014), kPc, Op::SW, InsnClass::Store, N, N, 0, "sw ra,20(sp)"},
        {I(0x2B, A0, V0, 0xFFFC), kPc, Op::SW, InsnClass::Store, N, N, 0, "sw v0,-4(a0)"},
        {I(0x2E, A0, V0, 0x0000), kPc, Op::SWR, InsnClass::Store, N, N, 0, "swr v0,0(a0)"},
        {I(0x32, A1, ZERO, 0), kPc, Op::LWC2, InsnClass::Cop, N, N, 0, "lwc2 $0,0(a1)"},
        {I(0x32, S0, 12, 10), kPc, Op::LWC2, InsnClass::Cop, N, N, 0, "lwc2 $12,10(s0)"},
        {I(0x3A, A0, 11, 16), kPc, Op::SWC2, InsnClass::Cop, N, N, 0, "swc2 $11,16(a0)"},
        {I(0x3A, ZERO, 1, 0x0A05), kPc, Op::SWC2, InsnClass::Cop, N, N, 0, "swc2 $1,2565(zero)"},
        {I(0x3A, GP, 31, 0x8000), kPc, Op::SWC2, InsnClass::Cop, N, N, 0, "swc2 $31,-32768(gp)"},
        // ---- invalid
        {0x7C000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x7c000000"},
        {0xDEAD0000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0xdead0000"},
        {0x00000001, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x1"},
        {0x00000005, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x5"},
        {0x0000001C, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x1c"},
        {0x0000003F, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x3f"},
        {0x44000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x44000000"},  // COP1
        {0x4C000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x4c000000"},  // COP3
        {0xC0000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0xc0000000"},  // LWC0
        {0xC4000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0xc4000000"},  // LWC1
        {0xCC000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0xcc000000"},  // LWC3
        {0xE0000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0xe0000000"},  // SWC0
        {0xE4000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0xe4000000"},  // SWC1
        {0xEC000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0xec000000"},  // SWC3
        {0x40A00000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x40a00000"},  // COP0 rs=5
        {0x40C0F800, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x40c0f800"},  // CTC0
        {0x42000001, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x42000001"},  // TLBR
        {0x48200000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x48200000"},  // COP2 rs=1
        {0x48600000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x48600000"},  // COP2 rs=3
        {0x49000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x49000000"},  // BC2F
        {0x70000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x70000000"},
        {0x9C000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0x9c000000"},  // LWU
        {0xBC000000, kPc, Op::Invalid, InsnClass::Invalid, N, N, 0, ".word 0xbc000000"},  // CACHE
    };
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Parameterized per-opcode table
// ---------------------------------------------------------------------------------------------------------------
class DecoderCase : public ::testing::TestWithParam<Case> {};

TEST_P(DecoderCase, Decode) {
    const Case& c = GetParam();
    Instruction in = decode(c.word, c.pc);
    EXPECT_EQ(in.raw, c.word);
    EXPECT_EQ(in.pc, c.pc);
    EXPECT_EQ(in.op, c.op);
    EXPECT_EQ(in.klass(), c.klass);
    EXPECT_EQ(in.has_delay_slot(), c.delay);
    EXPECT_EQ(in.is_call(), c.call);
    EXPECT_EQ(in.dest_gpr(), c.dest);
    EXPECT_EQ(disassemble(in), c.text);
}

TEST_P(DecoderCase, FieldsMatchRaw) {
    const Case& c = GetParam();
    Instruction in = decode(c.word, c.pc);
    EXPECT_EQ(in.rs, (c.word >> 21) & 31);
    EXPECT_EQ(in.rt, (c.word >> 16) & 31);
    EXPECT_EQ(in.rd, (c.word >> 11) & 31);
    EXPECT_EQ(in.shamt, (c.word >> 6) & 31);
    EXPECT_EQ(in.uimm, c.word & 0xFFFF);
    EXPECT_EQ(in.simm, static_cast<int16_t>(c.word & 0xFFFF));
}

TEST_P(DecoderCase, TargetOnlyForControlFlow) {
    const Case& c = GetParam();
    Instruction in = decode(c.word, c.pc);
    if (in.klass() != InsnClass::Branch && in.klass() != InsnClass::Jump) {
        EXPECT_EQ(in.target, 0u);
    }
    if (in.klass() == InsnClass::Branch) {
        EXPECT_EQ(in.target, c.pc + 4 + (static_cast<uint32_t>(in.simm) << 2));
    }
    if (in.klass() == InsnClass::Jump) {
        EXPECT_EQ(in.target & 3u, 0u);
        EXPECT_EQ(in.target & 0xF0000000u, (c.pc + 4) & 0xF0000000u);
    }
}

TEST_P(DecoderCase, PcDoesNotChangeNonPcRelativeText) {
    const Case& c = GetParam();
    Instruction a = decode(c.word, c.pc);
    Instruction b = decode(c.word, c.pc + 0x1000);
    EXPECT_EQ(a.op, b.op);
    if (a.klass() != InsnClass::Branch && a.klass() != InsnClass::Jump) {
        EXPECT_EQ(disassemble(a), disassemble(b));
    }
}

INSTANTIATE_TEST_SUITE_P(Table, DecoderCase, ::testing::ValuesIn(cases()));

// ---------------------------------------------------------------------------------------------------------------
// Targeted tests
// ---------------------------------------------------------------------------------------------------------------
TEST(Decoder, GprNames) {
    EXPECT_STREQ(gpr_name(0), "zero");
    EXPECT_STREQ(gpr_name(1), "at");
    EXPECT_STREQ(gpr_name(2), "v0");
    EXPECT_STREQ(gpr_name(4), "a0");
    EXPECT_STREQ(gpr_name(8), "t0");
    EXPECT_STREQ(gpr_name(16), "s0");
    EXPECT_STREQ(gpr_name(24), "t8");
    EXPECT_STREQ(gpr_name(26), "k0");
    EXPECT_STREQ(gpr_name(28), "gp");
    EXPECT_STREQ(gpr_name(29), "sp");
    EXPECT_STREQ(gpr_name(30), "s8");  // objdump spelling
    EXPECT_STREQ(gpr_name(31), "ra");
    EXPECT_STREQ(gpr_name(32), "?");
    EXPECT_STREQ(gpr_name(255), "?");
}

TEST(Decoder, GprNamesUniqueAndNonEmpty) {
    std::set<std::string> seen;
    for (uint8_t r = 0; r < 32; ++r) {
        ASSERT_NE(gpr_name(r), nullptr);
        EXPECT_TRUE(seen.insert(gpr_name(r)).second) << "duplicate name for r" << int(r);
    }
}

TEST(Decoder, DefaultInstructionIsInvalid) {
    Instruction in;
    EXPECT_EQ(in.op, Op::Invalid);
    EXPECT_EQ(in.klass(), InsnClass::Invalid);
    EXPECT_FALSE(in.has_delay_slot());
    EXPECT_FALSE(in.is_call());
    EXPECT_EQ(in.dest_gpr(), 0);
}

TEST(Decoder, BranchTargetNegativeOffset) {
    // beq zero,zero,-1 -> branches to itself.
    Instruction in = decode(I(4, 0, 0, 0xFFFF), 0x80020000);
    EXPECT_EQ(in.op, Op::BEQ);
    EXPECT_EQ(in.simm, -1);
    EXPECT_EQ(in.target, 0x80020000u);
    EXPECT_EQ(disassemble(in), "b 0x80020000");
}

TEST(Decoder, BranchTargetMaxBackward) {
    Instruction in = decode(I(5, A0, A1, 0x8000), 0x80100000);
    EXPECT_EQ(in.simm, -32768);
    EXPECT_EQ(in.target, 0x80100004u - 0x20000u);
    EXPECT_EQ(disassemble(in), "bne a0,a1,0x800e0004");
}

TEST(Decoder, BranchTargetMaxForward) {
    Instruction in = decode(I(5, A0, A1, 0x7FFF), 0x80100000);
    EXPECT_EQ(in.simm, 32767);
    EXPECT_EQ(in.target, 0x80100004u + 0x1FFFCu);
}

TEST(Decoder, BranchTargetDependsOnPc) {
    uint32_t w = I(4, A0, A1, 4);
    EXPECT_EQ(decode(w, 0x80010000).target, 0x80010014u);
    EXPECT_EQ(decode(w, 0x80010100).target, 0x80010114u);
    EXPECT_EQ(decode(w, 0xBFC00000).target, 0xBFC00014u);
    EXPECT_EQ(decode(w, 0x00000000).target, 0x00000014u);
}

TEST(Decoder, BranchTargetWrapsAt32Bits) {
    Instruction in = decode(I(4, 0, 0, 0x0010), 0xFFFFFFF0);
    EXPECT_EQ(in.target, 0xFFFFFFF0u + 4 + 0x40);  // wraps modulo 2^32
}

TEST(Decoder, JumpTargetUsesDelaySlotRegion) {
    // Region bits come from pc+4, so a jump in the last word of a 256MB region lands in the next region.
    uint32_t w = J(2, 0x0000100);
    EXPECT_EQ(decode(w, 0x80010000).target, 0x80000400u);
    EXPECT_EQ(decode(w, 0x0FFFFFFC).target, 0x10000400u);
    EXPECT_EQ(decode(w, 0x1FFFFFFC).target, 0x20000400u);
    EXPECT_EQ(decode(w, 0xBFC00000).target, 0xB0000400u);
    EXPECT_EQ(decode(w, 0xA0001000).target, 0xA0000400u);
    EXPECT_EQ(decode(w, 0x00000000).target, 0x00000400u);
}

TEST(Decoder, JumpTargetMaxIndex) {
    EXPECT_EQ(decode(J(2, 0x3FFFFFF), 0x80010000).target, 0x8FFFFFFCu);
    EXPECT_EQ(decode(J(3, 0x3FFFFFF), 0xA0000000).target, 0xAFFFFFFCu);
    EXPECT_EQ(decode(J(2, 0x3FFFFFF), 0x00000000).target, 0x0FFFFFFCu);
}

TEST(Decoder, JalSetsLinkAndCall) {
    Instruction in = decode(J(3, 0x1234), 0x80010000);
    EXPECT_EQ(in.op, Op::JAL);
    EXPECT_TRUE(in.is_call());
    EXPECT_TRUE(in.has_delay_slot());
    EXPECT_EQ(in.dest_gpr(), RA);
    EXPECT_EQ(in.target, 0x800048D0u);
    EXPECT_EQ(disassemble(in), "jal 0x800048d0");
}

TEST(Decoder, JumpIsNotCall) {
    Instruction in = decode(J(2, 0x1234), 0x80010000);
    EXPECT_FALSE(in.is_call());
    EXPECT_EQ(in.dest_gpr(), 0);
}

TEST(Decoder, WritesToZeroReportRegisterZero) {
    EXPECT_EQ(decode(R(A0, A1, ZERO, 0, 0x21), kPc).dest_gpr(), 0);
    EXPECT_EQ(decode(I(9, A0, ZERO, 1), kPc).dest_gpr(), 0);
    EXPECT_EQ(decode(I(0x23, A0, ZERO, 0), kPc).dest_gpr(), 0);
    EXPECT_EQ(decode(0x48000000 | (ZERO << 16), kPc).dest_gpr(), 0);  // mfc2 zero,$0
    EXPECT_EQ(decode(R(A0, 0, ZERO, 0, 0x09), kPc).dest_gpr(), 0);    // jalr zero,a0
}

TEST(Decoder, DestGprForEachClass) {
    EXPECT_EQ(decode(R(A0, A1, V0, 0, 0x21), kPc).dest_gpr(), V0);  // R-type -> rd
    EXPECT_EQ(decode(I(9, A0, V1, 1), kPc).dest_gpr(), V1);         // I-type -> rt
    EXPECT_EQ(decode(I(0x23, A0, V1, 1), kPc).dest_gpr(), V1);      // load -> rt
    EXPECT_EQ(decode(I(0x2B, A0, V1, 1), kPc).dest_gpr(), 0);       // store -> none
    EXPECT_EQ(decode(I(4, A0, V1, 1), kPc).dest_gpr(), 0);          // branch -> none
    EXPECT_EQ(decode(R(A0, A1, 0, 0, 0x18), kPc).dest_gpr(), 0);    // mult -> none (HI/LO)
    EXPECT_EQ(decode(R(A0, 0, 0, 0, 0x08), kPc).dest_gpr(), 0);     // jr -> none
    EXPECT_EQ(decode(0x0000000C, kPc).dest_gpr(), 0);               // syscall -> none
    EXPECT_EQ(decode(0x4A41E012, kPc).dest_gpr(), 0);               // GTE command -> none
}

TEST(Decoder, MoveToCopHasNoGprDest) {
    EXPECT_EQ(decode(0x40826000, kPc).dest_gpr(), 0);  // mtc0
    EXPECT_EQ(decode(0x4884F000, kPc).dest_gpr(), 0);  // mtc2
    EXPECT_EQ(decode(0x48C46800, kPc).dest_gpr(), 0);  // ctc2
    EXPECT_EQ(decode(I(0x32, A0, 5, 0), kPc).dest_gpr(), 0);  // lwc2
}

TEST(Decoder, LinkingBranchesAlwaysWriteRa) {
    EXPECT_EQ(decode(I(1, A0, 0x10, 1), kPc).dest_gpr(), RA);
    EXPECT_EQ(decode(I(1, A0, 0x11, 1), kPc).dest_gpr(), RA);
    EXPECT_EQ(decode(I(1, A0, 0x00, 1), kPc).dest_gpr(), 0);
    EXPECT_EQ(decode(I(1, A0, 0x01, 1), kPc).dest_gpr(), 0);
}

TEST(Decoder, JalrCallAndDest) {
    Instruction in = decode(R(A0, 0, S1, 0, 0x09), kPc);
    EXPECT_TRUE(in.is_call());
    EXPECT_EQ(in.dest_gpr(), S1);
    EXPECT_EQ(in.klass(), InsnClass::JumpReg);
    EXPECT_FALSE(decode(R(A0, 0, 0, 0, 0x08), kPc).is_call());
}

// ---- REGIMM quirk ----------------------------------------------------------------------------------------------
struct RegimmCase {
    uint32_t rt;
    Op op;
};

class RegimmQuirk : public ::testing::TestWithParam<RegimmCase> {};

TEST_P(RegimmQuirk, RtBitsSelectVariant) {
    const RegimmCase& c = GetParam();
    Instruction in = decode(I(1, A0, c.rt, 4), kPc);
    EXPECT_EQ(in.op, c.op) << "rt=" << c.rt;
    EXPECT_EQ(in.klass(), InsnClass::Branch);
    EXPECT_TRUE(in.has_delay_slot());
    EXPECT_EQ(in.target, kPc + 4 + 16);
    bool link = (c.rt & 0x10) != 0;
    EXPECT_EQ(in.is_call(), link);
    EXPECT_EQ(in.dest_gpr(), link ? RA : 0);
}

std::vector<RegimmCase> regimm_cases() {
    std::vector<RegimmCase> v;
    for (uint32_t rt = 0; rt < 32; ++rt) {
        bool ge = rt & 1, link = rt & 0x10;
        v.push_back({rt, link ? (ge ? Op::BGEZAL : Op::BLTZAL) : (ge ? Op::BGEZ : Op::BLTZ)});
    }
    return v;
}

INSTANTIATE_TEST_SUITE_P(AllRt, RegimmQuirk, ::testing::ValuesIn(regimm_cases()));

TEST(Decoder, RegimmCanonicalEncodings) {
    EXPECT_EQ(decode(I(1, A0, 0x00, 0), kPc).op, Op::BLTZ);
    EXPECT_EQ(decode(I(1, A0, 0x01, 0), kPc).op, Op::BGEZ);
    EXPECT_EQ(decode(I(1, A0, 0x10, 0), kPc).op, Op::BLTZAL);
    EXPECT_EQ(decode(I(1, A0, 0x11, 0), kPc).op, Op::BGEZAL);
}

TEST(Decoder, RegimmQuirkDisassemblyMatchesCanonicalVariant) {
    EXPECT_EQ(disassemble(decode(I(1, A0, 0x02, 1), kPc)), "bltz a0,0x80010008");
    EXPECT_EQ(disassemble(decode(I(1, A0, 0x03, 1), kPc)), "bgez a0,0x80010008");
    EXPECT_EQ(disassemble(decode(I(1, A0, 0x12, 1), kPc)), "bltzal a0,0x80010008");
    EXPECT_EQ(disassemble(decode(I(1, A0, 0x13, 1), kPc)), "bgezal a0,0x80010008");
}

// ---- GTE ---------------------------------------------------------------------------------------------------------
TEST(Decoder, GteCommandFieldIs25Bits) {
    Instruction in = decode(0x4A41E012, kPc);
    EXPECT_EQ(in.op, Op::GTE_CMD);
    EXPECT_EQ(in.gte_cmd, 0x0041E012u);
    EXPECT_EQ(in.gte_cmd, 0x4A41E012u & 0x1FFFFFFu);
    EXPECT_EQ(in.gte_cmd & 0x3F, 0x12u);  // MVMVA
}

TEST(Decoder, GteCommandAnyCoFlagBit25) {
    for (uint32_t rs = 0x10; rs < 0x20; ++rs) {
        Instruction in = decode((0x12u << 26) | (rs << 21) | 0x123456u, kPc);
        EXPECT_EQ(in.op, Op::GTE_CMD) << "rs=" << rs;
        EXPECT_EQ(in.gte_cmd, (rs << 21 & 0x1FFFFFFu) | 0x123456u);
    }
}

TEST(Decoder, GteCmdFieldZeroForOtherOps) {
    EXPECT_EQ(decode(0x48C46800, kPc).gte_cmd, 0u);
    EXPECT_EQ(decode(0x00000000, kPc).gte_cmd, 0u);
}

TEST(Decoder, GteCommonCommands) {
    // RTPS 0x01, NCLIP 0x06, MVMVA 0x12, SQR 0x28, RTPT 0x30, AVSZ3 0x2D, AVSZ4 0x2E, GPF 0x3D, GPL 0x3E, NCCT 0x3F
    const uint32_t cmds[] = {0x01, 0x06, 0x12, 0x28, 0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F};
    for (uint32_t c : cmds) {
        Instruction in = decode(0x4A180000u | c, kPc);
        EXPECT_EQ(in.op, Op::GTE_CMD);
        EXPECT_EQ(in.gte_cmd & 0x3F, c);
        EXPECT_EQ(in.klass(), InsnClass::Cop);
    }
}

TEST(Decoder, Cop2MoveSelectors) {
    EXPECT_EQ(decode(0x48000000, kPc).op, Op::MFC2);
    EXPECT_EQ(decode(0x48400000, kPc).op, Op::CFC2);
    EXPECT_EQ(decode(0x48800000, kPc).op, Op::MTC2);
    EXPECT_EQ(decode(0x48C00000, kPc).op, Op::CTC2);
    for (uint32_t rs : {1u, 3u, 5u, 7u, 8u, 9u, 10u, 15u}) {
        EXPECT_EQ(decode((0x12u << 26) | (rs << 21), kPc).op, Op::Invalid) << "rs=" << rs;
    }
}

TEST(Decoder, Cop2MoveRegisterOperands) {
    Instruction in = decode(0x48C46800, kPc);  // ctc2 a0,$13
    EXPECT_EQ(in.rt, A0);
    EXPECT_EQ(in.rd, 13);
    in = decode(0x48044800, kPc);  // mfc2 a0,$9
    EXPECT_EQ(in.rt, A0);
    EXPECT_EQ(in.rd, 9);
}

TEST(Decoder, Cop0Selectors) {
    EXPECT_EQ(decode(0x40000000, kPc).op, Op::MFC0);
    EXPECT_EQ(decode(0x40800000, kPc).op, Op::MTC0);
    EXPECT_EQ(decode(0x42000010, kPc).op, Op::RFE);
    for (uint32_t rs : {1u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 15u}) {
        EXPECT_EQ(decode((0x10u << 26) | (rs << 21), kPc).op, Op::Invalid) << "rs=" << rs;
    }
}

TEST(Decoder, Cop0CoFunctionsOtherThanRfeInvalid) {
    for (uint32_t f : {0x01u, 0x02u, 0x06u, 0x08u, 0x18u, 0x20u, 0x3Fu}) {
        EXPECT_EQ(decode(0x42000000 | f, kPc).op, Op::Invalid) << "funct=" << f;
    }
}

TEST(Decoder, RfeIsSystemClassNoDelaySlot) {
    Instruction in = decode(0x42000010, kPc);
    EXPECT_EQ(in.klass(), InsnClass::System);
    EXPECT_FALSE(in.has_delay_slot());
}

TEST(Decoder, Cop0RegisterNames) {
    struct { uint32_t rd; const char* name; } names[] = {
        {0, "c0_index"}, {1, "c0_random"}, {2, "c0_entrylo"}, {3, "$3"}, {4, "c0_context"}, {5, "$5"},
        {6, "$6"}, {7, "$7"}, {8, "c0_badvaddr"}, {9, "$9"}, {10, "c0_entryhi"}, {11, "$11"}, {12, "c0_sr"},
        {13, "c0_cause"}, {14, "c0_epc"}, {15, "c0_prid"}, {16, "$16"}, {31, "$31"},
    };
    for (auto& n : names) {
        std::string expect = std::string("mfc0 t0,") + n.name;
        EXPECT_EQ(disassemble(decode(0x40080000u | (n.rd << 11), kPc)), expect);
        expect = std::string("mtc0 t0,") + n.name;
        EXPECT_EQ(disassemble(decode(0x40880000u | (n.rd << 11), kPc)), expect);
    }
}

// ---- pseudo-ops --------------------------------------------------------------------------------------------------
TEST(Disasm, NopOnlyForAllZero) {
    EXPECT_EQ(disassemble(decode(0, kPc)), "nop");
    EXPECT_EQ(disassemble(decode(R(0, 0, 0, 1, 0), kPc)), "ssnop");
    EXPECT_EQ(disassemble(decode(R(0, 0, 0, 3, 0), kPc)), "ehb");
    EXPECT_EQ(disassemble(decode(R(0, 0, 0, 2, 0), kPc)), "sll zero,zero,0x2");
    EXPECT_EQ(disassemble(decode(R(0, 0, 0, 5, 0), kPc)), "sll zero,zero,0x5");
    EXPECT_EQ(disassemble(decode(R(0, 1, 0, 0, 0), kPc)), "sll zero,at,0x0");
    EXPECT_EQ(disassemble(decode(R(0, 0, 1, 0, 0), kPc)), "sll at,zero,0x0");
}

TEST(Disasm, MovePseudoOnlyWhenRtZero) {
    EXPECT_EQ(disassemble(decode(R(A0, 0, V0, 0, 0x21), kPc)), "move v0,a0");
    EXPECT_EQ(disassemble(decode(R(A0, 0, V0, 0, 0x25), kPc)), "move v0,a0");
    EXPECT_EQ(disassemble(decode(R(0, A0, V0, 0, 0x21), kPc)), "addu v0,zero,a0");
    EXPECT_EQ(disassemble(decode(R(0, A0, V0, 0, 0x25), kPc)), "or v0,zero,a0");
    EXPECT_EQ(disassemble(decode(R(A0, 0, V0, 0, 0x20), kPc)), "add v0,a0,zero");  // add is never "move"
    EXPECT_EQ(disassemble(decode(R(A0, 0, V0, 0, 0x24), kPc)), "and v0,a0,zero");
}

TEST(Disasm, NegPseudo) {
    EXPECT_EQ(disassemble(decode(R(0, A0, V0, 0, 0x22), kPc)), "neg v0,a0");
    EXPECT_EQ(disassemble(decode(R(0, A0, V0, 0, 0x23), kPc)), "negu v0,a0");
}

TEST(Disasm, LiPseudo) {
    EXPECT_EQ(disassemble(decode(I(9, 0, A0, 0x7FFF), kPc)), "li a0,32767");
    EXPECT_EQ(disassemble(decode(I(9, 0, A0, 0x8000), kPc)), "li a0,-32768");
    EXPECT_EQ(disassemble(decode(I(13, 0, A0, 0x7FFF), kPc)), "li a0,0x7fff");
    EXPECT_EQ(disassemble(decode(I(13, 0, A0, 0x8000), kPc)), "li a0,0x8000");
    EXPECT_EQ(disassemble(decode(I(8, 0, A0, 1), kPc)), "addi a0,zero,1");  // addi is not li
    EXPECT_EQ(disassemble(decode(I(14, 0, A0, 1), kPc)), "xori a0,zero,0x1");
    EXPECT_EQ(disassemble(decode(I(12, 0, A0, 1), kPc)), "andi a0,zero,0x1");
}

TEST(Disasm, BranchPseudos) {
    EXPECT_EQ(disassemble(decode(I(4, 0, 0, 1), kPc)), "b 0x80010008");
    EXPECT_EQ(disassemble(decode(I(4, A0, 0, 1), kPc)), "beqz a0,0x80010008");
    EXPECT_EQ(disassemble(decode(I(4, 0, A0, 1), kPc)), "beq zero,a0,0x80010008");
    EXPECT_EQ(disassemble(decode(I(5, A0, 0, 1), kPc)), "bnez a0,0x80010008");
    EXPECT_EQ(disassemble(decode(I(5, 0, A0, 1), kPc)), "bne zero,a0,0x80010008");
    EXPECT_EQ(disassemble(decode(I(1, 0, 1, 1), kPc)), "b 0x80010008");
    EXPECT_EQ(disassemble(decode(I(1, 0, 0x11, 1), kPc)), "bal 0x80010008");
    EXPECT_EQ(disassemble(decode(I(1, 0, 0x10, 1), kPc)), "bltzal zero,0x80010008");
}

TEST(Disasm, JalrFormats) {
    EXPECT_EQ(disassemble(decode(R(A0, 0, RA, 0, 9), kPc)), "jalr a0");
    EXPECT_EQ(disassemble(decode(R(A0, 0, V0, 0, 9), kPc)), "jalr v0,a0");
}

TEST(Disasm, ImmediateSignedness) {
    EXPECT_EQ(disassemble(decode(I(9, A0, A1, 0xFFFF), kPc)), "addiu a1,a0,-1");
    EXPECT_EQ(disassemble(decode(I(12, A0, A1, 0xFFFF), kPc)), "andi a1,a0,0xffff");
    EXPECT_EQ(disassemble(decode(I(13, A0, A1, 0xFFFF), kPc)), "ori a1,a0,0xffff");
    EXPECT_EQ(disassemble(decode(I(14, A0, A1, 0xFFFF), kPc)), "xori a1,a0,0xffff");
    EXPECT_EQ(disassemble(decode(I(10, A0, A1, 0xFFFF), kPc)), "slti a1,a0,-1");
    EXPECT_EQ(disassemble(decode(I(11, A0, A1, 0xFFFF), kPc)), "sltiu a1,a0,-1");
    EXPECT_EQ(disassemble(decode(I(15, 0, A1, 0xFFFF), kPc)), "lui a1,0xffff");
}

TEST(Disasm, SignAndZeroExtendedFields) {
    Instruction in = decode(I(9, A0, A1, 0x8001), kPc);
    EXPECT_EQ(in.simm, -32767);
    EXPECT_EQ(in.uimm, 0x8001u);
    in = decode(I(13, A0, A1, 0x7FFF), kPc);
    EXPECT_EQ(in.simm, 32767);
    EXPECT_EQ(in.uimm, 0x7FFFu);
}

TEST(Disasm, DivUsesThreeOperandForm) {
    EXPECT_EQ(disassemble(decode(R(A0, A1, 0, 0, 0x1A), kPc)), "div zero,a0,a1");
    EXPECT_EQ(disassemble(decode(R(A0, A1, 0, 0, 0x1B), kPc)), "divu zero,a0,a1");
    EXPECT_EQ(disassemble(decode(R(A0, A1, 0, 0, 0x18), kPc)), "mult a0,a1");
    EXPECT_EQ(disassemble(decode(R(A0, A1, 0, 0, 0x19), kPc)), "multu a0,a1");
}

TEST(Disasm, NoDollarOnGprsButDollarOnCop2Regs) {
    EXPECT_EQ(disassemble(decode(I(0x23, SP, S8, 8), kPc)), "lw s8,8(sp)");
    EXPECT_EQ(disassemble(decode(I(0x32, SP, 5, 8), kPc)), "lwc2 $5,8(sp)");
    EXPECT_EQ(disassemble(decode(0x4802F800, kPc)), "mfc2 v0,$31");
}

TEST(Disasm, ShiftAmountIsHexVariableShiftOrderIsRtRs) {
    EXPECT_EQ(disassemble(decode(R(0, A0, A1, 10, 0), kPc)), "sll a1,a0,0xa");
    EXPECT_EQ(disassemble(decode(R(A2, A0, A1, 0, 4), kPc)), "sllv a1,a0,a2");
    EXPECT_EQ(disassemble(decode(R(A2, A0, A1, 0, 6), kPc)), "srlv a1,a0,a2");
    EXPECT_EQ(disassemble(decode(R(A2, A0, A1, 0, 7), kPc)), "srav a1,a0,a2");
}

TEST(Disasm, InvalidPrintsRawWord) {
    EXPECT_EQ(disassemble(decode(0xFFFFFFFF, kPc)), ".word 0xffffffff");
    EXPECT_EQ(disassemble(decode(0x00000001, kPc)), ".word 0x1");
}

TEST(Disasm, NoTabsOrRunsOfSpaces) {
    for (const Case& c : cases()) {
        std::string s = disassemble(decode(c.word, c.pc));
        EXPECT_EQ(s.find('\t'), std::string::npos) << s;
        EXPECT_EQ(s.find("  "), std::string::npos) << s;
        EXPECT_EQ(s.find(", "), std::string::npos) << s;
    }
}

// ---- classification ---------------------------------------------------------------------------------------------
TEST(Decoder, DelaySlotClasses) {
    EXPECT_TRUE(decode(J(2, 1), kPc).has_delay_slot());
    EXPECT_TRUE(decode(J(3, 1), kPc).has_delay_slot());
    EXPECT_TRUE(decode(R(RA, 0, 0, 0, 8), kPc).has_delay_slot());
    EXPECT_TRUE(decode(R(RA, 0, RA, 0, 9), kPc).has_delay_slot());
    EXPECT_TRUE(decode(I(4, 0, 0, 1), kPc).has_delay_slot());
    EXPECT_TRUE(decode(I(5, 0, 0, 1), kPc).has_delay_slot());
    EXPECT_TRUE(decode(I(6, 0, 0, 1), kPc).has_delay_slot());
    EXPECT_TRUE(decode(I(7, 0, 0, 1), kPc).has_delay_slot());
    EXPECT_TRUE(decode(I(1, 0, 0, 1), kPc).has_delay_slot());
    EXPECT_FALSE(decode(0x0000000C, kPc).has_delay_slot());  // syscall
    EXPECT_FALSE(decode(0x0000000D, kPc).has_delay_slot());  // break
    EXPECT_FALSE(decode(0x42000010, kPc).has_delay_slot());  // rfe
    EXPECT_FALSE(decode(I(0x23, 0, 0, 0), kPc).has_delay_slot());
    EXPECT_FALSE(decode(0, kPc).has_delay_slot());
    EXPECT_FALSE(decode(0xFFFFFFFF, kPc).has_delay_slot());
}

TEST(Decoder, KlassLoadsAndStores) {
    for (uint32_t op : {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u}) {
        EXPECT_EQ(decode(I(op, A0, V0, 0), kPc).klass(), InsnClass::Load) << op;
    }
    for (uint32_t op : {0x28u, 0x29u, 0x2Au, 0x2Bu, 0x2Eu}) {
        EXPECT_EQ(decode(I(op, A0, V0, 0), kPc).klass(), InsnClass::Store) << op;
    }
}

TEST(Decoder, UnusedPrimaryOpcodesInvalid) {
    std::set<uint32_t> valid = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                                0x0D, 0x0E, 0x0F, 0x10, 0x12, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28,
                                0x29, 0x2A, 0x2B, 0x2E, 0x32, 0x3A};
    for (uint32_t op = 0; op < 64; ++op) {
        if (op == 0x00 || op == 0x10 || op == 0x12) continue;  // sub-decoded; see dedicated tests
        Instruction in = decode((op << 26) | 0x00A41234u, kPc);
        if (valid.count(op)) {
            EXPECT_NE(in.op, Op::Invalid) << "opcode " << op;
        } else {
            EXPECT_EQ(in.op, Op::Invalid) << "opcode " << op;
        }
    }
}

TEST(Decoder, SpecialFunctTable) {
    const std::pair<uint32_t, Op> valid[] = {
        {0x00, Op::SLL}, {0x02, Op::SRL}, {0x03, Op::SRA}, {0x04, Op::SLLV}, {0x06, Op::SRLV}, {0x07, Op::SRAV},
        {0x08, Op::JR}, {0x09, Op::JALR}, {0x0C, Op::SYSCALL}, {0x0D, Op::BREAK}, {0x10, Op::MFHI},
        {0x11, Op::MTHI}, {0x12, Op::MFLO}, {0x13, Op::MTLO}, {0x18, Op::MULT}, {0x19, Op::MULTU},
        {0x1A, Op::DIV}, {0x1B, Op::DIVU}, {0x20, Op::ADD}, {0x21, Op::ADDU}, {0x22, Op::SUB}, {0x23, Op::SUBU},
        {0x24, Op::AND}, {0x25, Op::OR}, {0x26, Op::XOR}, {0x27, Op::NOR}, {0x2A, Op::SLT}, {0x2B, Op::SLTU},
    };
    std::set<uint32_t> seen;
    for (auto& [f, op] : valid) {
        EXPECT_EQ(decode(R(1, 2, 3, 4, f), kPc).op, op) << "funct " << f;
        seen.insert(f);
    }
    for (uint32_t f = 0; f < 64; ++f) {
        if (!seen.count(f)) EXPECT_EQ(decode(R(1, 2, 3, 4, f), kPc).op, Op::Invalid) << "funct " << f;
    }
}

// ---- exhaustive / randomized sweeps ----------------------------------------------------------------------------
namespace {
uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
}  // namespace

TEST(DecoderSweep, EveryOpIsReachable) {
    std::set<Op> seen;
    uint32_t s = 0x12345678;
    for (int n = 0; n < 2000000; ++n) seen.insert(decode(xorshift(s), kPc).op);
    for (int op = static_cast<int>(Op::Invalid); op <= static_cast<int>(Op::SWC2); ++op) {
        EXPECT_TRUE(seen.count(static_cast<Op>(op))) << "op " << op << " never decoded";
    }
}

TEST(DecoderSweep, InvariantsHoldForRandomWords) {
    uint32_t s = 0xC0FFEE11;
    for (int n = 0; n < 1000000; ++n) {
        uint32_t w = xorshift(s);
        uint32_t pc = xorshift(s) & ~3u;
        Instruction in = decode(w, pc);
        InsnClass k = in.klass();
        ASSERT_EQ(in.raw, w);
        ASSERT_EQ(in.pc, pc);
        ASSERT_EQ(k == InsnClass::Invalid, in.op == Op::Invalid);
        ASSERT_EQ(in.has_delay_slot(), k == InsnClass::Branch || k == InsnClass::Jump || k == InsnClass::JumpReg);
        ASSERT_LT(in.dest_gpr(), 32);
        if (in.is_call()) ASSERT_TRUE(in.has_delay_slot());
        if (k != InsnClass::Branch && k != InsnClass::Jump) ASSERT_EQ(in.target, 0u);
        if (in.op != Op::GTE_CMD) ASSERT_EQ(in.gte_cmd, 0u);
        std::string d = disassemble(in);
        ASSERT_FALSE(d.empty());
        ASSERT_EQ(d.rfind(".word ", 0) == 0, in.op == Op::Invalid) << d;
    }
}

TEST(DecoderSweep, AllEncodingsOfOpcodeAndFunctFieldsNeverCrash) {
    for (uint32_t op = 0; op < 64; ++op) {
        for (uint32_t low = 0; low < 0x10000; low += 7) {
            Instruction in = decode((op << 26) | (0x15u << 16) | low, kPc);
            (void)disassemble(in);
        }
    }
}

TEST(DecoderSweep, SpecialFunctSweepOverAllRegisterFields) {
    for (uint32_t f = 0; f < 64; ++f) {
        for (uint32_t r = 0; r < 32; ++r) {
            Instruction in = decode(R(r, 31 - r, (r * 7) & 31, r, f), kPc);
            if (in.op != Op::Invalid) {
                EXPECT_EQ(in.rs, r);
                EXPECT_EQ(in.rt, 31 - r);
                EXPECT_EQ(in.shamt, r);
            }
        }
    }
}
