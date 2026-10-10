#pragma once

// Tiny MIPS I encoder for building test programs.

#include <cstdint>
#include <vector>

#include "psxrecomp/analysis.h"

namespace masm {

enum Reg : uint32_t {
    zero, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7,
    s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp, fp, ra,
};

constexpr uint32_t r_type(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh, uint32_t fn) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | fn;
}
constexpr uint32_t i_type(uint32_t op, uint32_t rs, uint32_t rt, int32_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<uint32_t>(imm) & 0xFFFF);
}

constexpr uint32_t nop() { return 0; }
constexpr uint32_t sll(Reg rd, Reg rt, uint32_t sh) { return r_type(0, rt, rd, sh, 0x00); }
constexpr uint32_t srl(Reg rd, Reg rt, uint32_t sh) { return r_type(0, rt, rd, sh, 0x02); }
constexpr uint32_t sra(Reg rd, Reg rt, uint32_t sh) { return r_type(0, rt, rd, sh, 0x03); }
constexpr uint32_t sllv(Reg rd, Reg rt, Reg rs) { return r_type(rs, rt, rd, 0, 0x04); }
constexpr uint32_t srlv(Reg rd, Reg rt, Reg rs) { return r_type(rs, rt, rd, 0, 0x06); }
constexpr uint32_t srav(Reg rd, Reg rt, Reg rs) { return r_type(rs, rt, rd, 0, 0x07); }
constexpr uint32_t jr(Reg rs) { return r_type(rs, 0, 0, 0, 0x08); }
constexpr uint32_t jalr(Reg rs, Reg rd = ra) { return r_type(rs, 0, rd, 0, 0x09); }
constexpr uint32_t syscall() { return 0x0000000C; }
constexpr uint32_t break_() { return 0x0000000D; }
constexpr uint32_t add(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x20); }
constexpr uint32_t addu(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x21); }
constexpr uint32_t sub(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x22); }
constexpr uint32_t and_(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x24); }
constexpr uint32_t xor_(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x26); }
constexpr uint32_t nor(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x27); }
constexpr uint32_t sltu(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x2B); }
constexpr uint32_t mthi(Reg rs) { return r_type(rs, 0, 0, 0, 0x11); }
constexpr uint32_t mtlo(Reg rs) { return r_type(rs, 0, 0, 0, 0x13); }
constexpr uint32_t multu(Reg rs, Reg rt) { return r_type(rs, rt, 0, 0, 0x19); }
constexpr uint32_t divu(Reg rs, Reg rt) { return r_type(rs, rt, 0, 0, 0x1B); }
constexpr uint32_t subu(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x23); }
constexpr uint32_t or_(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x25); }
constexpr uint32_t slt(Reg rd, Reg rs, Reg rt) { return r_type(rs, rt, rd, 0, 0x2A); }
constexpr uint32_t mult(Reg rs, Reg rt) { return r_type(rs, rt, 0, 0, 0x18); }
constexpr uint32_t div(Reg rs, Reg rt) { return r_type(rs, rt, 0, 0, 0x1A); }
constexpr uint32_t mflo(Reg rd) { return r_type(0, 0, rd, 0, 0x12); }
constexpr uint32_t mfhi(Reg rd) { return r_type(0, 0, rd, 0, 0x10); }
constexpr uint32_t move(Reg rd, Reg rs) { return addu(rd, rs, zero); }

constexpr uint32_t addiu(Reg rt, Reg rs, int32_t imm) { return i_type(0x09, rs, rt, imm); }
constexpr uint32_t addi(Reg rt, Reg rs, int32_t imm) { return i_type(0x08, rs, rt, imm); }
constexpr uint32_t xori(Reg rt, Reg rs, uint32_t imm) { return i_type(0x0E, rs, rt, static_cast<int32_t>(imm)); }
constexpr uint32_t li(Reg rt, int32_t imm) { return addiu(rt, zero, imm); }
constexpr uint32_t slti(Reg rt, Reg rs, int32_t imm) { return i_type(0x0A, rs, rt, imm); }
constexpr uint32_t sltiu(Reg rt, Reg rs, int32_t imm) { return i_type(0x0B, rs, rt, imm); }
constexpr uint32_t andi(Reg rt, Reg rs, uint32_t imm) { return i_type(0x0C, rs, rt, static_cast<int32_t>(imm)); }
constexpr uint32_t ori(Reg rt, Reg rs, uint32_t imm) { return i_type(0x0D, rs, rt, static_cast<int32_t>(imm)); }
constexpr uint32_t lui(Reg rt, uint32_t imm) { return i_type(0x0F, 0, rt, static_cast<int32_t>(imm)); }
constexpr uint32_t lb(Reg rt, int32_t off, Reg base) { return i_type(0x20, base, rt, off); }
constexpr uint32_t lh(Reg rt, int32_t off, Reg base) { return i_type(0x21, base, rt, off); }
constexpr uint32_t lw(Reg rt, int32_t off, Reg base) { return i_type(0x23, base, rt, off); }
constexpr uint32_t lbu(Reg rt, int32_t off, Reg base) { return i_type(0x24, base, rt, off); }
constexpr uint32_t lhu(Reg rt, int32_t off, Reg base) { return i_type(0x25, base, rt, off); }
constexpr uint32_t lwl(Reg rt, int32_t off, Reg base) { return i_type(0x22, base, rt, off); }
constexpr uint32_t lwr(Reg rt, int32_t off, Reg base) { return i_type(0x26, base, rt, off); }
constexpr uint32_t sb(Reg rt, int32_t off, Reg base) { return i_type(0x28, base, rt, off); }
constexpr uint32_t sh(Reg rt, int32_t off, Reg base) { return i_type(0x29, base, rt, off); }
constexpr uint32_t sw(Reg rt, int32_t off, Reg base) { return i_type(0x2B, base, rt, off); }
constexpr uint32_t swl(Reg rt, int32_t off, Reg base) { return i_type(0x2A, base, rt, off); }
constexpr uint32_t swr(Reg rt, int32_t off, Reg base) { return i_type(0x2E, base, rt, off); }

// Branch offsets are in instructions relative to the delay slot (MIPS encoding).
constexpr uint32_t beq(Reg rs, Reg rt, int32_t off) { return i_type(0x04, rs, rt, off); }
constexpr uint32_t bne(Reg rs, Reg rt, int32_t off) { return i_type(0x05, rs, rt, off); }
constexpr uint32_t blez(Reg rs, int32_t off) { return i_type(0x06, rs, 0, off); }
constexpr uint32_t bgtz(Reg rs, int32_t off) { return i_type(0x07, rs, 0, off); }
constexpr uint32_t bltz(Reg rs, int32_t off) { return i_type(0x01, rs, 0, off); }
constexpr uint32_t bgez(Reg rs, int32_t off) { return i_type(0x01, rs, 1, off); }
constexpr uint32_t bltzal(Reg rs, int32_t off) { return i_type(0x01, rs, 0x10, off); }
constexpr uint32_t bgezal(Reg rs, int32_t off) { return i_type(0x01, rs, 0x11, off); }
constexpr uint32_t bal(int32_t off) { return bgezal(zero, off); }
constexpr uint32_t beqz(Reg rs, int32_t off) { return beq(rs, zero, off); }
constexpr uint32_t bnez(Reg rs, int32_t off) { return bne(rs, zero, off); }
constexpr uint32_t b(int32_t off) { return beq(zero, zero, off); }
constexpr uint32_t j(uint32_t target) { return (0x02u << 26) | ((target >> 2) & 0x03FFFFFF); }
constexpr uint32_t jal(uint32_t target) { return (0x03u << 26) | ((target >> 2) & 0x03FFFFFF); }

// Builds a segment at `base` from a list of words.
class Program {
public:
    explicit Program(uint32_t base) : base_(base) {}
    uint32_t pc() const { return base_ + static_cast<uint32_t>(words_.size() * 4); }
    Program& emit(uint32_t w) {
        words_.push_back(w);
        return *this;
    }
    Program& emit(std::initializer_list<uint32_t> ws) {
        for (uint32_t w : ws) words_.push_back(w);
        return *this;
    }
    void patch(uint32_t addr, uint32_t w) { words_[(addr - base_) / 4] = w; }
    uint32_t base() const { return base_; }
    const std::vector<uint32_t>& words() const { return words_; }
    psxrecomp::CodeSegment segment(const char* name = "TEST") const {
        psxrecomp::CodeSegment s;
        s.name = name;
        s.base = base_;
        for (uint32_t w : words_)
            for (int i = 0; i < 4; ++i) s.bytes.push_back(static_cast<uint8_t>(w >> (8 * i)));
        return s;
    }

private:
    uint32_t base_;
    std::vector<uint32_t> words_;
};

}  // namespace masm
