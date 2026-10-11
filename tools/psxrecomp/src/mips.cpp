// MIPS R3000A decoder and objdump-style disassembler.
//
// Decoding is lenient about reserved/don't-care bits (like the real CPU) but strict about opcodes:
// unknown primary/SPECIAL/COP selectors decode to Op::Invalid. REGIMM follows the R3000 quirk: rt bit 0 selects
// BGEZ vs BLTZ, rt bit 4 selects the linking variant, all other rt bits are ignored.
//
// Disassembly mirrors `objdump -D -m mips:3000` (no '$' on GPR names, $30 is "s8", pseudo-ops nop/ssnop/ehb/move/
// li/neg/negu/b/bal/beqz/bnez, "div zero,rs,rt", c0_* names, "c2 0xNNN" GTE commands). Intentional differences:
//   * Invalid words print as ".word 0xNNNN" (objdump also prints this for its own invalid set, but objdump
//     additionally accepts non-R3000 encodings and reserved-bit variants that we either reject or decode leniently).
//   * "rfe" is only produced for the canonical word 0x42000010 by objdump; we accept any COP0 CO-op with funct 0x10.
#include "psxrecomp/mips.h"

#include <cstdarg>
#include <cstdio>

namespace psxrecomp {

static const char* const kGprNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra",
};

const char* gpr_name(uint8_t reg) { return reg < 32 ? kGprNames[reg] : "?"; }

namespace {

Op decode_special(uint32_t funct) {
    switch (funct) {
        case 0x00: return Op::SLL;
        case 0x02: return Op::SRL;
        case 0x03: return Op::SRA;
        case 0x04: return Op::SLLV;
        case 0x06: return Op::SRLV;
        case 0x07: return Op::SRAV;
        case 0x08: return Op::JR;
        case 0x09: return Op::JALR;
        case 0x0C: return Op::SYSCALL;
        case 0x0D: return Op::BREAK;
        case 0x10: return Op::MFHI;
        case 0x11: return Op::MTHI;
        case 0x12: return Op::MFLO;
        case 0x13: return Op::MTLO;
        case 0x18: return Op::MULT;
        case 0x19: return Op::MULTU;
        case 0x1A: return Op::DIV;
        case 0x1B: return Op::DIVU;
        case 0x20: return Op::ADD;
        case 0x21: return Op::ADDU;
        case 0x22: return Op::SUB;
        case 0x23: return Op::SUBU;
        case 0x24: return Op::AND;
        case 0x25: return Op::OR;
        case 0x26: return Op::XOR;
        case 0x27: return Op::NOR;
        case 0x2A: return Op::SLT;
        case 0x2B: return Op::SLTU;
        default: return Op::Invalid;
    }
}

Op decode_regimm(uint32_t rt) {
    bool ge = (rt & 1) != 0;
    bool link = (rt & 0x10) != 0;
    if (link) return ge ? Op::BGEZAL : Op::BLTZAL;
    return ge ? Op::BGEZ : Op::BLTZ;
}

Op decode_cop0(uint32_t word) {
    uint32_t rs = (word >> 21) & 31;
    if (word & (1u << 25)) return (word & 0x3F) == 0x10 ? Op::RFE : Op::Invalid;
    if (rs == 0) return Op::MFC0;
    if (rs == 4) return Op::MTC0;
    return Op::Invalid;
}

Op decode_cop2(uint32_t word) {
    if (word & (1u << 25)) return Op::GTE_CMD;
    switch ((word >> 21) & 31) {
        case 0: return Op::MFC2;
        case 2: return Op::CFC2;
        case 4: return Op::MTC2;
        case 6: return Op::CTC2;
        default: return Op::Invalid;
    }
}

Op decode_primary(uint32_t word) {
    uint32_t rt = (word >> 16) & 31;
    switch (word >> 26) {
        case 0x00: return decode_special(word & 0x3F);
        case 0x01: return decode_regimm(rt);
        case 0x02: return Op::J;
        case 0x03: return Op::JAL;
        case 0x04: return Op::BEQ;
        case 0x05: return Op::BNE;
        case 0x06: return Op::BLEZ;
        case 0x07: return Op::BGTZ;
        case 0x08: return Op::ADDI;
        case 0x09: return Op::ADDIU;
        case 0x0A: return Op::SLTI;
        case 0x0B: return Op::SLTIU;
        case 0x0C: return Op::ANDI;
        case 0x0D: return Op::ORI;
        case 0x0E: return Op::XORI;
        case 0x0F: return Op::LUI;
        case 0x10: return decode_cop0(word);
        case 0x12: return decode_cop2(word);
        case 0x20: return Op::LB;
        case 0x21: return Op::LH;
        case 0x22: return Op::LWL;
        case 0x23: return Op::LW;
        case 0x24: return Op::LBU;
        case 0x25: return Op::LHU;
        case 0x26: return Op::LWR;
        case 0x28: return Op::SB;
        case 0x29: return Op::SH;
        case 0x2A: return Op::SWL;
        case 0x2B: return Op::SW;
        case 0x2E: return Op::SWR;
        case 0x32: return Op::LWC2;
        case 0x3A: return Op::SWC2;
        default: return Op::Invalid;
    }
}

bool is_branch(Op op) {
    switch (op) {
        case Op::BLTZ: case Op::BGEZ: case Op::BLTZAL: case Op::BGEZAL:
        case Op::BEQ: case Op::BNE: case Op::BLEZ: case Op::BGTZ:
            return true;
        default:
            return false;
    }
}

}  // namespace

Instruction decode(uint32_t word, uint32_t pc) {
    Instruction i;
    i.raw = word;
    i.pc = pc;
    i.rs = (word >> 21) & 31;
    i.rt = (word >> 16) & 31;
    i.rd = (word >> 11) & 31;
    i.shamt = (word >> 6) & 31;
    i.simm = static_cast<int16_t>(word & 0xFFFF);
    i.uimm = word & 0xFFFF;
    i.op = decode_primary(word);
    if (is_branch(i.op)) {
        i.target = pc + 4 + (static_cast<uint32_t>(i.simm) << 2);
    } else if (i.op == Op::J || i.op == Op::JAL) {
        i.target = ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
    } else if (i.op == Op::GTE_CMD) {
        i.gte_cmd = word & 0x1FFFFFFu;
    }
    return i;
}

InsnClass Instruction::klass() const {
    switch (op) {
        case Op::Invalid: return InsnClass::Invalid;
        case Op::SLL: case Op::SRL: case Op::SRA: case Op::SLLV: case Op::SRLV: case Op::SRAV:
        case Op::MFHI: case Op::MTHI: case Op::MFLO: case Op::MTLO:
        case Op::MULT: case Op::MULTU: case Op::DIV: case Op::DIVU:
        case Op::ADD: case Op::ADDU: case Op::SUB: case Op::SUBU:
        case Op::AND: case Op::OR: case Op::XOR: case Op::NOR: case Op::SLT: case Op::SLTU:
        case Op::ADDI: case Op::ADDIU: case Op::SLTI: case Op::SLTIU:
        case Op::ANDI: case Op::ORI: case Op::XORI: case Op::LUI:
            return InsnClass::Alu;
        case Op::JR: case Op::JALR: return InsnClass::JumpReg;
        case Op::J: case Op::JAL: return InsnClass::Jump;
        case Op::SYSCALL: case Op::BREAK: case Op::RFE: return InsnClass::System;
        case Op::BLTZ: case Op::BGEZ: case Op::BLTZAL: case Op::BGEZAL:
        case Op::BEQ: case Op::BNE: case Op::BLEZ: case Op::BGTZ:
            return InsnClass::Branch;
        case Op::MFC0: case Op::MTC0: case Op::MFC2: case Op::CFC2: case Op::MTC2: case Op::CTC2:
        case Op::GTE_CMD: case Op::LWC2: case Op::SWC2:
            return InsnClass::Cop;
        case Op::LB: case Op::LH: case Op::LWL: case Op::LW: case Op::LBU: case Op::LHU: case Op::LWR:
            return InsnClass::Load;
        case Op::SB: case Op::SH: case Op::SWL: case Op::SW: case Op::SWR:
            return InsnClass::Store;
    }
    return InsnClass::Invalid;
}

bool Instruction::has_delay_slot() const {
    InsnClass k = klass();
    return k == InsnClass::Branch || k == InsnClass::Jump || k == InsnClass::JumpReg;
}

bool Instruction::is_call() const {
    return op == Op::JAL || op == Op::JALR || op == Op::BLTZAL || op == Op::BGEZAL;
}

uint8_t Instruction::dest_gpr() const {
    switch (op) {
        case Op::JAL: case Op::BLTZAL: case Op::BGEZAL:
            return 31;
        case Op::JALR:
        case Op::SLL: case Op::SRL: case Op::SRA: case Op::SLLV: case Op::SRLV: case Op::SRAV:
        case Op::MFHI: case Op::MFLO:
        case Op::ADD: case Op::ADDU: case Op::SUB: case Op::SUBU:
        case Op::AND: case Op::OR: case Op::XOR: case Op::NOR: case Op::SLT: case Op::SLTU:
            return rd;
        case Op::ADDI: case Op::ADDIU: case Op::SLTI: case Op::SLTIU:
        case Op::ANDI: case Op::ORI: case Op::XORI: case Op::LUI:
        case Op::LB: case Op::LH: case Op::LWL: case Op::LW: case Op::LBU: case Op::LHU: case Op::LWR:
        case Op::MFC0: case Op::MFC2: case Op::CFC2:
            return rt;
        default:
            return 0;
    }
}

namespace {

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));

std::string fmt(const char* f, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

const char* cop0_reg_name(uint8_t r, char* scratch) {
    switch (r) {
        case 0: return "c0_index";
        case 1: return "c0_random";
        case 2: return "c0_entrylo";
        case 4: return "c0_context";
        case 8: return "c0_badvaddr";
        case 10: return "c0_entryhi";
        case 12: return "c0_sr";
        case 13: return "c0_cause";
        case 14: return "c0_epc";
        case 15: return "c0_prid";
        default: std::snprintf(scratch, 8, "$%u", r); return scratch;
    }
}

const char* mnemonic(Op op) {
    switch (op) {
        case Op::SLL: return "sll";     case Op::SRL: return "srl";     case Op::SRA: return "sra";
        case Op::SLLV: return "sllv";   case Op::SRLV: return "srlv";   case Op::SRAV: return "srav";
        case Op::JR: return "jr";       case Op::JALR: return "jalr";
        case Op::SYSCALL: return "syscall"; case Op::BREAK: return "break";
        case Op::MFHI: return "mfhi";   case Op::MTHI: return "mthi";
        case Op::MFLO: return "mflo";   case Op::MTLO: return "mtlo";
        case Op::MULT: return "mult";   case Op::MULTU: return "multu";
        case Op::DIV: return "div";     case Op::DIVU: return "divu";
        case Op::ADD: return "add";     case Op::ADDU: return "addu";
        case Op::SUB: return "sub";     case Op::SUBU: return "subu";
        case Op::AND: return "and";     case Op::OR: return "or";
        case Op::XOR: return "xor";     case Op::NOR: return "nor";
        case Op::SLT: return "slt";     case Op::SLTU: return "sltu";
        case Op::BLTZ: return "bltz";   case Op::BGEZ: return "bgez";
        case Op::BLTZAL: return "bltzal"; case Op::BGEZAL: return "bgezal";
        case Op::J: return "j";         case Op::JAL: return "jal";
        case Op::BEQ: return "beq";     case Op::BNE: return "bne";
        case Op::BLEZ: return "blez";   case Op::BGTZ: return "bgtz";
        case Op::ADDI: return "addi";   case Op::ADDIU: return "addiu";
        case Op::SLTI: return "slti";   case Op::SLTIU: return "sltiu";
        case Op::ANDI: return "andi";   case Op::ORI: return "ori";
        case Op::XORI: return "xori";   case Op::LUI: return "lui";
        case Op::MFC0: return "mfc0";   case Op::MTC0: return "mtc0";   case Op::RFE: return "rfe";
        case Op::MFC2: return "mfc2";   case Op::CFC2: return "cfc2";
        case Op::MTC2: return "mtc2";   case Op::CTC2: return "ctc2";   case Op::GTE_CMD: return "c2";
        case Op::LB: return "lb";       case Op::LH: return "lh";       case Op::LWL: return "lwl";
        case Op::LW: return "lw";       case Op::LBU: return "lbu";     case Op::LHU: return "lhu";
        case Op::LWR: return "lwr";     case Op::SB: return "sb";       case Op::SH: return "sh";
        case Op::SWL: return "swl";     case Op::SW: return "sw";       case Op::SWR: return "swr";
        case Op::LWC2: return "lwc2";   case Op::SWC2: return "swc2";
        case Op::Invalid: break;
    }
    return "?";
}

}  // namespace

std::string disassemble(const Instruction& in) {
    const char* m = mnemonic(in.op);
    const char* rs = gpr_name(in.rs);
    const char* rt = gpr_name(in.rt);
    const char* rd = gpr_name(in.rd);
    switch (in.op) {
        case Op::Invalid:
            return fmt(".word 0x%x", in.raw);
        case Op::SLL:
            if (in.rd == 0 && in.rt == 0) {
                if (in.shamt == 0) return "nop";
                if (in.shamt == 1) return "ssnop";
                if (in.shamt == 3) return "ehb";
            }
            [[fallthrough]];
        case Op::SRL: case Op::SRA:
            return fmt("%s %s,%s,0x%x", m, rd, rt, in.shamt);
        case Op::SLLV: case Op::SRLV: case Op::SRAV:
            return fmt("%s %s,%s,%s", m, rd, rt, rs);
        case Op::JR: case Op::MTHI: case Op::MTLO:
            return fmt("%s %s", m, rs);
        case Op::JALR:
            return in.rd == 31 ? fmt("jalr %s", rs) : fmt("jalr %s,%s", rd, rs);
        case Op::SYSCALL: {
            uint32_t code = (in.raw >> 6) & 0xFFFFF;
            return code ? fmt("syscall 0x%x", code) : "syscall";
        }
        case Op::BREAK: {
            uint32_t c1 = (in.raw >> 16) & 0x3FF, c2 = (in.raw >> 6) & 0x3FF;
            if (c2) return fmt("break 0x%x,0x%x", c1, c2);
            return c1 ? fmt("break 0x%x", c1) : "break";
        }
        case Op::MFHI: case Op::MFLO:
            return fmt("%s %s", m, rd);
        case Op::MULT: case Op::MULTU:
            return fmt("%s %s,%s", m, rs, rt);
        case Op::DIV: case Op::DIVU:
            return fmt("%s zero,%s,%s", m, rs, rt);
        case Op::OR: case Op::ADDU:
            if (in.rt == 0) return fmt("move %s,%s", rd, rs);
            return fmt("%s %s,%s,%s", m, rd, rs, rt);
        case Op::SUB:
            if (in.rs == 0) return fmt("neg %s,%s", rd, rt);
            return fmt("%s %s,%s,%s", m, rd, rs, rt);
        case Op::SUBU:
            if (in.rs == 0) return fmt("negu %s,%s", rd, rt);
            return fmt("%s %s,%s,%s", m, rd, rs, rt);
        case Op::ADD: case Op::AND: case Op::XOR: case Op::NOR: case Op::SLT: case Op::SLTU:
            return fmt("%s %s,%s,%s", m, rd, rs, rt);
        case Op::BLTZ: case Op::BLTZAL: case Op::BLEZ: case Op::BGTZ:
            return fmt("%s %s,0x%x", m, rs, in.target);
        case Op::BGEZ:
            if (in.rs == 0) return fmt("b 0x%x", in.target);
            return fmt("bgez %s,0x%x", rs, in.target);
        case Op::BGEZAL:
            if (in.rs == 0) return fmt("bal 0x%x", in.target);
            return fmt("bgezal %s,0x%x", rs, in.target);
        case Op::J: case Op::JAL:
            return fmt("%s 0x%x", m, in.target);
        case Op::BEQ:
            if (in.rt == 0) return in.rs == 0 ? fmt("b 0x%x", in.target) : fmt("beqz %s,0x%x", rs, in.target);
            return fmt("beq %s,%s,0x%x", rs, rt, in.target);
        case Op::BNE:
            if (in.rt == 0) return fmt("bnez %s,0x%x", rs, in.target);
            return fmt("bne %s,%s,0x%x", rs, rt, in.target);
        case Op::ADDIU:
            if (in.rs == 0) return fmt("li %s,%d", rt, in.simm);
            return fmt("addiu %s,%s,%d", rt, rs, in.simm);
        case Op::ADDI: case Op::SLTI: case Op::SLTIU:
            return fmt("%s %s,%s,%d", m, rt, rs, in.simm);
        case Op::ORI:
            if (in.rs == 0) return fmt("li %s,0x%x", rt, in.uimm);
            return fmt("ori %s,%s,0x%x", rt, rs, in.uimm);
        case Op::ANDI: case Op::XORI:
            return fmt("%s %s,%s,0x%x", m, rt, rs, in.uimm);
        case Op::LUI:
            return fmt("lui %s,0x%x", rt, in.uimm);
        case Op::MFC0: case Op::MTC0: {
            char scratch[8];
            return fmt("%s %s,%s", m, rt, cop0_reg_name(in.rd, scratch));
        }
        case Op::RFE:
            return "rfe";
        case Op::MFC2: case Op::CFC2: case Op::MTC2: case Op::CTC2:
            return fmt("%s %s,$%u", m, rt, in.rd);
        case Op::GTE_CMD:
            return fmt("c2 0x%x", in.gte_cmd);
        case Op::LB: case Op::LH: case Op::LWL: case Op::LW: case Op::LBU: case Op::LHU: case Op::LWR:
        case Op::SB: case Op::SH: case Op::SWL: case Op::SW: case Op::SWR:
            return fmt("%s %s,%d(%s)", m, rt, in.simm, rs);
        case Op::LWC2: case Op::SWC2:
            return fmt("%s $%u,%d(%s)", m, in.rt, in.simm, rs);
    }
    return fmt(".word 0x%x", in.raw);
}

}  // namespace psxrecomp
