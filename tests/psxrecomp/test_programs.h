#pragma once

// Deterministic list of MIPS test programs shared by the build-time generator (emit_testgen, which
// recompiles them) and the differential test (test_emitter, which also runs them in the reference
// interpreter). Register conventions for every program (set up by the test harness):
//   $sp = kStackTop, $ra = kSentinel, $s7 = kDataBase, $gp/$fp/$k1 = aliases of kDataBase (KUSEG,
//   KSEG1, RAM mirror), $k0 = scratchpad window. Programs treat these as read-only (except $sp/$ra
//   saves). All other registers hold random/edge-case values on entry.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "mips_asm.h"

namespace tp {

using namespace masm;

constexpr uint32_t kProgBase = 0x80010000u;
constexpr uint32_t kProgStride = 0x800u;
constexpr uint32_t kDataBase = 0x801E1000u;     // $s7 (KSEG0)
constexpr uint32_t kDataKuseg = 0x001E1000u;    // $gp
constexpr uint32_t kDataKseg1 = 0xA01E1000u;    // $fp
constexpr uint32_t kDataMirror = 0x803E1000u;   // $k1 (second 2 MiB RAM mirror)
constexpr uint32_t kScratchBase = 0x1F800200u;  // $k0
constexpr uint32_t kDataFillLo = 0x801E0000u;   // random data fill [lo, lo + size)
constexpr uint32_t kDataFillSize = 0x2000u;
constexpr uint32_t kStackTop = 0x801FF000u;
constexpr uint32_t kSentinel = 0xFFFF0000u;

struct TestProgram {
    std::string name;  // segment name, "T000"...
    std::string desc;
    uint32_t base = 0;
    uint32_t entry = 0;
    std::vector<uint32_t> words;
    std::vector<uint32_t> extra_funcs;  // forced function starts (indirect call targets, tail-call targets)
};

// splitmix64: tiny, portable and fully deterministic.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t next() {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<uint32_t>((z ^ (z >> 31)) >> 16);
    }
    uint32_t below(uint32_t n) { return next() % n; }
};

constexpr uint32_t kEdge32[] = {0u, 1u, 2u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000001u, 0xFFFFu,
                                0x8000u, 0x10000u, 0xFFFF0000u, 0x12345678u, 0xDEADBEEFu, 31u, 32u, 0xFFu};

// ---- small assembly helpers ----

inline void li32(Program& p, Reg r, uint32_t v) {
    p.emit(lui(r, v >> 16));
    p.emit(ori(r, r, v & 0xFFFF));
}
inline int32_t boff(uint32_t from, uint32_t to) { return static_cast<int32_t>(to - (from + 4)) / 4; }
inline void ret(Program& p) {
    p.emit(jr(ra));
    p.emit(nop());
}
inline void pad_to(Program& p, uint32_t addr) {
    while (p.pc() < addr) p.emit(nop());
}
// Result slots live in the data window at $s7 + 0x400 + 4 * idx.
inline void save(Program& p, Reg r, int idx) { p.emit(sw(r, 0x400 + 4 * idx, s7)); }

// Calls `fn` with `args` already set; wraps main-style prologue/epilogue keeping $ra in $s6.
inline void main_begin(Program& p) { p.emit(move(s6, ra)); }
inline void main_end(Program& p) {
    p.emit(move(ra, s6));
    ret(p);
}

class Suite {
public:
    using Body = std::function<void(Program&, std::vector<uint32_t>& extra_funcs)>;
    void push(const std::string& desc, const Body& body) {
        uint32_t base = kProgBase + static_cast<uint32_t>(progs_.size()) * kProgStride;
        Program p(base);
        std::vector<uint32_t> extras;
        body(p, extras);
        if (p.words().size() * 4 > kProgStride) throw std::runtime_error("test program too large: " + desc);
        char nm[16];
        std::snprintf(nm, sizeof(nm), "T%03zu", progs_.size());
        progs_.push_back({nm, desc, base, base, p.words(), extras});
    }
    std::vector<TestProgram>& programs() { return progs_; }

private:
    std::vector<TestProgram> progs_;
};

// ---- random instruction generation ----

class RandAsm {
public:
    explicit RandAsm(uint64_t seed) : rng_(seed) {}
    Rng& rng() { return rng_; }

    Reg dst() {
        uint32_t n = rng_.below(25);  // 0..22, t8, t9: never sp/ra/s7/gp/fp/k0/k1
        return static_cast<Reg>(n <= 22 ? n : (n == 23 ? 24 : 25));
    }
    Reg src() { return static_cast<Reg>(rng_.below(32)); }

    int32_t simm() {
        static const int32_t e[] = {0, 1, -1, 0x7FFF, -0x8000, 2, -2, 0x100, 31, 32};
        if (rng_.below(3) == 0) return e[rng_.below(10)];
        return static_cast<int16_t>(rng_.next());
    }
    uint32_t uimm() {
        static const uint32_t e[] = {0, 1, 0xFFFF, 0x8000, 0x7FFF, 0xFF, 0xFF00, 31};
        if (rng_.below(3) == 0) return e[rng_.below(8)];
        return rng_.next() & 0xFFFF;
    }

    // Non-memory, non-control instruction.
    uint32_t alu() {
        Reg d = dst(), s = src(), t = src();
        switch (rng_.below(32)) {
            case 0: return add(d, s, t);
            case 1: return addu(d, s, t);
            case 2: return sub(d, s, t);
            case 3: return subu(d, s, t);
            case 4: return and_(d, s, t);
            case 5: return or_(d, s, t);
            case 6: return xor_(d, s, t);
            case 7: return nor(d, s, t);
            case 8: return slt(d, s, t);
            case 9: return sltu(d, s, t);
            case 10: return sllv(d, s, t);
            case 11: return srlv(d, s, t);
            case 12: return srav(d, s, t);
            case 13: return sll(d, s, rng_.below(32));
            case 14: return srl(d, s, rng_.below(32));
            case 15: return sra(d, s, rng_.below(32));
            case 16: return addi(d, s, simm());
            case 17: return addiu(d, s, simm());
            case 18: return slti(d, s, simm());
            case 19: return sltiu(d, s, simm());
            case 20: return andi(d, s, uimm());
            case 21: return ori(d, s, uimm());
            case 22: return xori(d, s, uimm());
            case 23: return lui(d, rng_.next() & 0xFFFF);
            case 24: return mult(s, t);
            case 25: return multu(s, t);
            case 26: return div(s, t);
            case 27: return divu(s, t);
            case 28: return mfhi(d);
            case 29: return mflo(d);
            case 30: return mthi(s);
            default: return mtlo(s);
        }
    }

    // Load or store inside the data window / scratchpad.
    uint32_t mem(bool loads = true) {
        Reg base = s7;
        int lo = -0x800, hi = 0x7F0;
        switch (rng_.below(10)) {
            case 0: case 1: base = gp; break;
            case 2: case 3: base = fp; break;
            case 4: base = k1; break;
            case 5: base = k0; lo = -0x200; hi = 0x1F0; break;
            default: break;
        }
        int32_t off = lo + static_cast<int32_t>(rng_.below(static_cast<uint32_t>(hi - lo)));
        Reg d = dst(), t = src();
        switch (rng_.below(loads ? 12 : 5) + (loads ? 0 : 7)) {
            case 0: return lb(d, off, base);
            case 1: return lbu(d, off, base);
            case 2: return lh(d, off & ~1, base);
            case 3: return lhu(d, off & ~1, base);
            case 4: return lw(d, off & ~3, base);
            case 5: case 6: return lwl(d, off, base);
            case 7: return lwr(d, off, base);
            case 8: return sb(t, off, base);
            case 9: return sh(t, off & ~1, base);
            case 10: return sw(t, off & ~3, base);
            default: return rng_.below(2) ? swl(t, off, base) : swr(t, off, base);
        }
    }

    // Any of the above, ~30% memory.
    uint32_t any() { return rng_.below(10) < 3 ? mem() : alu(); }

private:
    Rng rng_;
};

inline bool is_load_word(uint32_t w) {
    uint32_t op = w >> 26;
    return op == 0x20 || op == 0x21 || op == 0x22 || op == 0x23 || op == 0x24 || op == 0x25 || op == 0x26;
}

// Instruction guaranteed not to be a load (ends blocks before branches/labels).
inline uint32_t non_load(RandAsm& g) {
    uint32_t w;
    do w = g.any();
    while (is_load_word(w));
    return w;
}

inline void random_straight(Program& p, uint64_t seed) {
    RandAsm g(seed);
    uint32_t n = 30 + g.rng().below(51);
    for (uint32_t i = 0; i < n; ++i) p.emit(g.any());
    p.emit(jr(ra));
    p.emit(g.rng().below(3) ? nop() : non_load(g));
}

inline void random_branchy(Program& p, uint64_t seed) {
    RandAsm g(seed);
    Rng& r = g.rng();
    uint32_t blocks = 3 + r.below(4);
    for (uint32_t b_i = 0; b_i < blocks; ++b_i) {
        uint32_t n = 2 + r.below(8);
        for (uint32_t i = 0; i + 1 < n; ++i) p.emit(g.any());
        p.emit(non_load(g));  // never a load right before a branch
        uint32_t m = r.below(7);
        int32_t off = static_cast<int32_t>(m) + 1;
        Reg s = g.src(), t = g.src();
        switch (r.below(6)) {
            case 0: p.emit(beq(s, t, off)); break;
            case 1: p.emit(bne(s, t, off)); break;
            case 2: p.emit(blez(s, off)); break;
            case 3: p.emit(bgtz(s, off)); break;
            case 4: p.emit(bltz(s, off)); break;
            default: p.emit(bgez(s, off)); break;
        }
        p.emit(non_load(g));  // delay slot: ALU/store
        for (uint32_t i = 0; i + 1 < m; ++i) p.emit(g.any());
        if (m) p.emit(non_load(g));  // block ends before the branch target
    }
    for (uint32_t i = 0; i < 3; ++i) p.emit(g.any());
    p.emit(jr(ra));
    p.emit(nop());
}

// ---- the program list ----

inline std::vector<TestProgram> build_test_programs() {
    Suite s;
    using Op3 = uint32_t (*)(Reg, Reg, Reg);
    using Op2 = uint32_t (*)(Reg, Reg);
    using OpI = uint32_t (*)(Reg, Reg, int32_t);
    const int32_t kMin = INT32_MIN, kMax = INT32_MAX;

    // ---- ALU, register forms: all 8x8 combinations of edge values ----
    struct Alu3 {
        const char* n;
        Op3 f;
    };
    const Alu3 alu3[] = {
        {"add", [](Reg d, Reg a, Reg c) { return add(d, a, c); }},
        {"addu", [](Reg d, Reg a, Reg c) { return addu(d, a, c); }},
        {"sub", [](Reg d, Reg a, Reg c) { return sub(d, a, c); }},
        {"subu", [](Reg d, Reg a, Reg c) { return subu(d, a, c); }},
        {"and", [](Reg d, Reg a, Reg c) { return and_(d, a, c); }},
        {"or", [](Reg d, Reg a, Reg c) { return or_(d, a, c); }},
        {"xor", [](Reg d, Reg a, Reg c) { return xor_(d, a, c); }},
        {"nor", [](Reg d, Reg a, Reg c) { return nor(d, a, c); }},
        {"slt", [](Reg d, Reg a, Reg c) { return slt(d, a, c); }},
        {"sltu", [](Reg d, Reg a, Reg c) { return sltu(d, a, c); }},
        {"sllv", [](Reg d, Reg a, Reg c) { return sllv(d, a, c); }},
        {"srlv", [](Reg d, Reg a, Reg c) { return srlv(d, a, c); }},
        {"srav", [](Reg d, Reg a, Reg c) { return srav(d, a, c); }},
    };
    static const uint32_t e8[] = {0, 1, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0xFFFFu, 0x8000u, 0x12345678u};
    for (const auto& o : alu3) {
        Op3 f = o.f;
        s.push(std::string("alu ") + o.n, [=](Program& p, auto&) {
            int idx = 0;
            for (uint32_t a : e8) {
                li32(p, t0, a);
                for (uint32_t c : e8) {
                    li32(p, t1, c);
                    p.emit(f(v0, t0, t1));
                    save(p, v0, idx++);
                }
            }
            ret(p);
        });
    }

    // ---- multiply / divide ----
    struct MulDiv {
        const char* n;
        Op2 f;
    };
    const MulDiv muldiv[] = {
        {"mult", [](Reg a, Reg c) { return mult(a, c); }},
        {"multu", [](Reg a, Reg c) { return multu(a, c); }},
        {"div", [](Reg a, Reg c) { return div(a, c); }},
        {"divu", [](Reg a, Reg c) { return divu(a, c); }},
    };
    static const uint32_t e7[] = {0, 1, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0xFFFFFFF9u, 100};
    for (const auto& o : muldiv) {
        Op2 f = o.f;
        s.push(std::string("hilo ") + o.n, [=](Program& p, auto&) {
            int idx = 0;
            for (uint32_t a : e7) {
                li32(p, t0, a);
                for (uint32_t c : e7) {
                    li32(p, t1, c);
                    p.emit(f(t0, t1));
                    p.emit(mflo(v0));
                    p.emit(mfhi(v1));
                    save(p, v0, idx++);
                    save(p, v1, idx++);
                }
            }
            ret(p);
        });
    }
    s.push("hilo moves", [](Program& p, auto&) {
        p.emit({mfhi(v0), mflo(v1), mthi(t0), mtlo(t1), mfhi(a0), mflo(a1)});
        p.emit({mult(t0, t1), mthi(zero), mflo(a2), mfhi(a3), mtlo(t1), mult(a0, a1), mtlo(t2), mfhi(t3)});
        p.emit({divu(t0, t1), mfhi(s0), mflo(s1), mthi(t4), mtlo(t5), mfhi(s2), mflo(s3), mflo(zero), mfhi(zero)});
        ret(p);
    });

    // ---- ALU immediates ----
    struct AluI {
        const char* n;
        OpI f;
        bool is_signed;
    };
    const AluI alui[] = {
        {"addi", [](Reg d, Reg a, int32_t i) { return addi(d, a, i); }, true},
        {"addiu", [](Reg d, Reg a, int32_t i) { return addiu(d, a, i); }, true},
        {"slti", [](Reg d, Reg a, int32_t i) { return slti(d, a, i); }, true},
        {"sltiu", [](Reg d, Reg a, int32_t i) { return sltiu(d, a, i); }, true},
        {"andi", [](Reg d, Reg a, int32_t i) { return andi(d, a, static_cast<uint32_t>(i)); }, false},
        {"ori", [](Reg d, Reg a, int32_t i) { return ori(d, a, static_cast<uint32_t>(i)); }, false},
        {"xori", [](Reg d, Reg a, int32_t i) { return xori(d, a, static_cast<uint32_t>(i)); }, false},
    };
    for (const auto& o : alui) {
        OpI f = o.f;
        bool sg = o.is_signed;
        s.push(std::string("alu imm ") + o.n, [=](Program& p, auto&) {
            static const int32_t simms[] = {0, 1, -1, 0x7FFF, -0x8000, 0x1234, -2};
            static const int32_t uimms[] = {0, 1, 0xFFFF, 0x8000, 0x7FFF, 0xFF, 0xFF00};
            int idx = 0;
            for (uint32_t a : e8) {
                li32(p, t0, a);
                for (int32_t i : sg ? simms : uimms) {
                    p.emit(f(v0, t0, i));
                    save(p, v0, idx++);
                }
            }
            ret(p);
        });
    }
    s.push("sltiu negative immediates", [](Program& p, auto&) {
        int idx = 0;
        for (uint32_t a : {0u, 1u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x7FFFu, 0xFFFF8000u, 0xFFFF7FFFu, 0x80000000u}) {
            li32(p, t0, a);
            for (int32_t i : {-1, -2, -0x8000, 0x7FFF, 0, 1}) {
                p.emit(sltiu(v0, t0, i));
                p.emit(slti(v1, t0, i));
                save(p, v0, idx++);
                save(p, v1, idx++);
            }
        }
        ret(p);
    });
    struct Shift {
        const char* n;
        uint32_t (*f)(Reg, Reg, uint32_t);
    };
    const Shift shifts[] = {
        {"sll", [](Reg d, Reg a, uint32_t i) { return sll(d, a, i); }},
        {"srl", [](Reg d, Reg a, uint32_t i) { return srl(d, a, i); }},
        {"sra", [](Reg d, Reg a, uint32_t i) { return sra(d, a, i); }},
    };
    for (const auto& o : shifts) {
        auto f = o.f;
        s.push(std::string("shift imm ") + o.n, [=](Program& p, auto&) {
            int idx = 0;
            for (uint32_t a : e8) {
                li32(p, t0, a);
                for (uint32_t sh_amt : {0u, 1u, 15u, 16u, 31u}) {
                    p.emit(f(v0, t0, sh_amt));
                    save(p, v0, idx++);
                }
            }
            ret(p);
        });
    }
    s.push("lui/ori composition", [](Program& p, auto&) {
        p.emit({lui(v0, 0x1234), ori(v0, v0, 0x5678), lui(v1, 0xFFFF), ori(v1, v1, 0xFFFF), lui(a0, 0x8000),
                lui(a1, 0), ori(a1, a1, 0xFFFF), addiu(a2, zero, -1), lui(a3, 0x7FFF), ori(a3, a3, 0xFFFF),
                lui(t0, 0xDEAD), ori(t0, t0, 0xBEEF), addiu(t1, t0, -0x8000), xori(t2, t0, 0xFFFF), li(t3, 0x7FFF)});
        li32(p, t4, 0x80008000u);
        li32(p, t5, 0u);
        ret(p);
    });
    s.push("writes to zero (alu)", [](Program& p, auto&) {
        p.emit({addu(zero, t0, t1), addiu(zero, zero, 5), lui(zero, 0x1234), ori(zero, t0, 0xFF), sll(zero, t0, 3),
                slt(zero, t0, t1), or_(zero, t0, t1), mflo(zero), mfhi(zero), sub(zero, t0, t1), srav(zero, t0, t1),
                nor(zero, t0, t1), addu(v0, zero, zero), addiu(v1, zero, 3), sw(zero, 0x400, s7), mult(t0, t1),
                mflo(zero), addu(a0, zero, t0)});
        ret(p);
    });
    s.push("writes to zero (loads)", [](Program& p, auto&) {
        p.emit({lw(zero, 0, s7), nop(), addu(v0, zero, zero), lb(zero, 1, s7), lbu(zero, 2, s7), lh(zero, 2, s7),
                lhu(zero, 4, s7), lwl(zero, 3, s7), lwr(zero, 0, s7), nop(), addu(v1, zero, t0), lw(zero, 8, s7),
                addu(a0, zero, zero), lw(zero, 12, s7), sw(zero, 0x404, s7), addiu(a1, zero, 9)});
        ret(p);
    });

    // ---- memory: loads/stores of every width through every address alias ----
    struct BaseSpec {
        Reg r;
        const char* n;
        int lo;  // smallest safe offset
    };
    const BaseSpec bases[] = {{s7, "kseg0", -0x200}, {gp, "kuseg", -0x200}, {fp, "kseg1", -0x200},
                              {k1, "mirror", -0x200}, {k0, "scratchpad", -0x200}};
    for (const auto& bs : bases) {
        Reg br = bs.r;
        s.push(std::string("loads via ") + bs.n, [=](Program& p, auto&) {
            int idx = 0;
            auto one = [&](uint32_t w) {
                p.emit(w);
                p.emit(nop());
                save(p, v0, idx++);
            };
            for (int off : {-0x200, -5, -4, -3, -2, -1, 0, 1, 2, 3, 0x1FF}) {
                one(lb(v0, off, br));
                one(lbu(v0, off, br));
            }
            for (int off : {-0x200, -6, -2, 0, 2, 6, 0x1FE}) {
                one(lh(v0, off, br));
                one(lhu(v0, off, br));
            }
            for (int off : {-0x200, -8, -4, 0, 4, 8, 0x1FC}) one(lw(v0, off, br));
            ret(p);
        });
        s.push(std::string("stores via ") + bs.n, [=](Program& p, auto&) {
            int idx = 0;
            li32(p, t0, 0x89ABCDEFu);
            li32(p, t1, 0x7F6E5D4Cu);
            for (int i = 0; i < 4; ++i) p.emit(sb(t0, 0x20 + i, br));
            for (int i = 0; i < 4; ++i) p.emit(sb(t1, 0x28 + i, br));
            p.emit({sh(t0, 0x30, br), sh(t0, 0x32, br), sh(t1, 0x38, br), sh(t1, 0x3A, br), sw(t0, 0x40, br),
                    sw(t1, 0x44, br), sw(t0, -0x40, br), sh(t1, -0x42, br), sb(t0, -0x43, br)});
            for (int i = 0; i < 4; ++i) {
                p.emit(swl(t0, 0x60 + i, br));
                p.emit(swr(t1, 0x70 + i, br));
                p.emit(swr(t0, 0x90 + i, br));
                p.emit(swl(t0, 0x90 + i + 3, br));
                p.emit(swl(t1, 0xA0 + i + 3, br));
                p.emit(swr(t1, 0xA0 + i, br));
            }
            for (int off : {0x20, 0x28, 0x30, 0x38, 0x40, 0x44, 0x60, 0x70, 0x90, 0xA0, -0x40}) {
                p.emit(lw(v0, off, br));
                p.emit(nop());
                save(p, v0, idx++);
            }
            ret(p);
        });
        s.push(std::string("lwl/lwr at all alignments via ") + bs.n, [=](Program& p, auto&) {
            int idx = 0;
            for (int al = 0; al < 4; ++al) {
                int a = 0x40 + al;
                li32(p, t0, 0x11223344u + 0x01010101u * static_cast<uint32_t>(al));
                p.emit({lwl(t0, a, br), nop()});
                save(p, t0, idx++);
                li32(p, t0, 0xAABBCCDDu - 0x01010101u * static_cast<uint32_t>(al));
                p.emit({lwr(t0, a, br), nop()});
                save(p, t0, idx++);
                li32(p, t0, 0xCAFEBABEu);
                p.emit({lwr(t0, a, br), lwl(t0, a + 3, br), nop()});  // classic unaligned lw
                save(p, t0, idx++);
                li32(p, t0, 0x0BADF00Du);
                p.emit({lwl(t0, a + 3, br), lwr(t0, a, br), nop()});  // reverse order
                save(p, t0, idx++);
                li32(p, t1, 0x55667788u);
                p.emit({lwl(t1, a + 3, br), lwr(t1, a, br), addu(v1, t1, t1)});  // forwarding then use
                save(p, v1, idx++);
                save(p, t1, idx++);
            }
            ret(p);
        });
    }
    s.push("absolute addressing and aliases", [](Program& p, auto&) {
        int idx = 0;
        p.emit({lui(at, 0x801E), lw(v0, 0x1000, at), nop()});
        save(p, v0, idx++);
        p.emit({lui(at, 0x001E), lw(v0, 0x1004, at), nop()});
        save(p, v0, idx++);
        p.emit({lui(at, 0xA01E), lw(v0, 0x1008, at), nop()});
        save(p, v0, idx++);
        p.emit({lui(at, 0x803E), lw(v0, 0x100C, at), nop()});
        save(p, v0, idx++);
        p.emit({lui(at, 0x801F), lw(v0, -0x1000 + 0x10, at), nop()});
        save(p, v0, idx++);
        li32(p, t0, 0xA1B2C3D4u);
        p.emit({lui(at, 0x801E), sw(t0, 0x1100, at), lui(at, 0xA03E), lw(v0, 0x1100, at), nop()});  // store/load via aliases
        save(p, v0, idx++);
        p.emit({lui(at, 0x9F80), sw(t0, 0x10, at), lui(at, 0x1F80), lw(v0, 0x10, at), nop()});  // scratchpad aliases
        save(p, v0, idx++);
        p.emit({lui(at, 0x1F80), lb(v0, 0x3FF, at), nop()});
        save(p, v0, idx++);
        p.emit({sw(t0, 0x100, zero), lw(v0, 0x100, zero), sb(t0, 0x105, zero), lbu(v1, 0x105, zero), nop()});
        save(p, v0, idx++);
        save(p, v1, idx++);
        p.emit({lui(at, 0x8020), lw(v0, -4, at), nop()});  // last word of RAM
        save(p, v0, idx++);
        ret(p);
    });

    // ---- branches ----
    struct Br1 {
        const char* n;
        uint32_t (*f)(Reg, int32_t);
    };
    const Br1 br1[] = {
        {"blez", [](Reg r, int32_t o) { return blez(r, o); }},
        {"bgtz", [](Reg r, int32_t o) { return bgtz(r, o); }},
        {"bltz", [](Reg r, int32_t o) { return bltz(r, o); }},
        {"bgez", [](Reg r, int32_t o) { return bgez(r, o); }},
    };
    for (const auto& o : br1) {
        auto f = o.f;
        s.push(std::string("branch ") + o.n, [=](Program& p, auto&) {
            int idx = 0;
            for (int32_t v : {kMin, -1, 0, 1, kMax, 0x10000}) {
                li32(p, t0, static_cast<uint32_t>(v));
                p.emit(li(v0, 1));
                p.emit(f(t0, 2));
                p.emit(addiu(s0, s0, 1));  // delay slot always runs
                p.emit(li(v0, 2));         // skipped when taken
                save(p, v0, idx++);
            }
            save(p, s0, idx++);
            ret(p);
        });
    }
    struct Br2 {
        const char* n;
        uint32_t (*f)(Reg, Reg, int32_t);
    };
    const Br2 br2[] = {{"beq", [](Reg a, Reg c, int32_t o) { return beq(a, c, o); }},
                       {"bne", [](Reg a, Reg c, int32_t o) { return bne(a, c, o); }}};
    for (const auto& o : br2) {
        auto f = o.f;
        s.push(std::string("branch ") + o.n, [=](Program& p, auto&) {
            int idx = 0;
            const uint32_t pairs[][2] = {{5, 5}, {5, 6}, {0, 0}, {0, 0xFFFFFFFFu}, {0x80000000u, 0x80000000u},
                                         {0x80000000u, 0x7FFFFFFFu}, {1, 0}};
            for (const auto& pr : pairs) {
                li32(p, t0, pr[0]);
                li32(p, t1, pr[1]);
                p.emit(li(v0, 1));
                p.emit(f(t0, t1, 2));
                p.emit(addiu(s0, s0, 1));
                p.emit(li(v0, 2));
                save(p, v0, idx++);
                p.emit(li(v0, 1));  // compare against $zero too
                p.emit(f(t0, zero, 2));
                p.emit(nop());
                p.emit(li(v0, 2));
                save(p, v0, idx++);
            }
            ret(p);
        });
    }
    struct BrAl {
        const char* n;
        uint32_t (*f)(Reg, int32_t);
    };
    const BrAl bral[] = {{"bltzal", [](Reg r, int32_t o) { return bltzal(r, o); }},
                         {"bgezal", [](Reg r, int32_t o) { return bgezal(r, o); }}};
    for (const auto& o : bral) {
        auto f = o.f;
        s.push(std::string("branch and link ") + o.n, [=](Program& p, auto&) {
            int idx = 0;
            std::vector<uint32_t> sites;
            main_begin(p);
            for (int32_t v : {kMin, -1, 0, 1, kMax}) {
                li32(p, t0, static_cast<uint32_t>(v));
                li32(p, ra, 0x12345678u);  // link must be written even when not taken
                sites.push_back(p.pc());
                p.emit(0);                 // patched below
                p.emit(addiu(s0, s0, 1));  // delay slot
                save(p, ra, idx++);
                save(p, s1, idx++);
            }
            main_end(p);
            uint32_t callee = p.pc();
            p.emit({jr(ra), addiu(s1, s1, 1)});
            for (uint32_t site : sites) p.patch(site, f(t0, boff(site, callee)));
        });
    }
    s.push("bal / bgezal zero", [](Program& p, auto&) {
        main_begin(p);
        uint32_t s1_at = p.pc();
        p.emit(0);
        p.emit(nop());
        save(p, ra, 0);
        uint32_t s2_at = p.pc();
        p.emit(0);
        p.emit(addiu(s0, s0, 5));
        save(p, ra, 1);
        save(p, s1, 2);
        main_end(p);
        uint32_t callee = p.pc();
        p.emit({jr(ra), addiu(s1, s1, 1)});
        p.patch(s1_at, bal(boff(s1_at, callee)));
        p.patch(s2_at, bgezal(zero, boff(s2_at, callee)));
    });
    s.push("unconditional branch forms", [](Program& p, auto&) {
        p.emit({li(v0, 0), b(2), addiu(v0, v0, 1), addiu(v0, v0, 100), bgez(zero, 2), addiu(v0, v0, 2),
                addiu(v0, v0, 100), blez(zero, 2), addiu(v0, v0, 4), addiu(v0, v0, 100), beq(t1, t1, 2),
                addiu(v0, v0, 8), addiu(v0, v0, 100), beq(zero, zero, 1), nop()});
        save(p, v0, 0);
        ret(p);
    });
    s.push("count-down loop (bgtz)", [](Program& p, auto&) {
        p.emit({li(t0, 10), li(v0, 0)});
        uint32_t top = p.pc();
        p.emit({addu(v0, v0, t0), addiu(t0, t0, -1)});
        p.emit({bgtz(t0, boff(p.pc(), top)), nop()});
        save(p, v0, 0);
        ret(p);
    });
    s.push("loop whose delay slot modifies the operand", [](Program& p, auto&) {
        p.emit({li(t0, 5), li(v0, 0)});
        uint32_t top = p.pc();
        p.emit(addu(v0, v0, t0));
        p.emit({bne(t0, zero, boff(p.pc(), top)), addiu(t0, t0, -1)});  // compares the old t0
        save(p, v0, 0);
        save(p, t0, 1);
        ret(p);
    });
    s.push("nested loops with stores", [](Program& p, auto&) {
        p.emit({li(s0, 0), li(s1, 4)});
        uint32_t outer = p.pc();
        p.emit(li(s2, 3));
        uint32_t inner = p.pc();
        p.emit({addu(s0, s0, s2), sll(t0, s1, 3), addu(t0, t0, s2), sll(t0, t0, 2), addu(t0, t0, s7)});
        p.emit({sw(s0, 0x500, t0), addiu(s2, s2, -1)});
        p.emit({bgtz(s2, boff(p.pc(), inner)), nop()});
        p.emit({addiu(s1, s1, -1)});
        p.emit({bne(s1, zero, boff(p.pc(), outer)), nop()});
        save(p, s0, 0);
        ret(p);
    });
    s.push("loop with slt and bnez", [](Program& p, auto&) {
        p.emit({li(t0, 0), li(v0, 1)});
        uint32_t top = p.pc();
        p.emit({addiu(t0, t0, 1), sll(v0, v0, 1), slti(t1, t0, 9)});
        p.emit({bnez(t1, boff(p.pc(), top)), nop()});
        save(p, v0, 0);
        ret(p);
    });
    s.push("loop built from j", [](Program& p, auto&) {
        p.emit({li(t0, 5), li(v0, 0)});
        uint32_t top = p.pc();
        p.emit({addu(v0, v0, t0), addiu(t0, t0, -1)});
        p.emit({beqz(t0, 3), nop(), j(top), nop()});
        save(p, v0, 0);
        ret(p);
    });
    // A delay slot that is also a branch target is emitted standalone by the emitter.
    for (int tv = 0; tv < 2; ++tv) {
        s.push(std::string("branch into a delay slot, inline branch ") + (tv ? "taken" : "not taken"),
               [=](Program& p, auto&) {
            p.emit({li(v0, 0), li(t0, tv), b(2), nop()});  // b -> the delay slot of the bnez below
            p.emit({bnez(t0, 4), addiu(v0, v0, 5), addiu(v0, v0, 100), addiu(v0, v0, 1000)});
            p.emit({addiu(v0, v0, 7)});
            save(p, v0, 0);
            // Same shape, but the branch executes inline this time.
            p.emit({li(v1, 0), bnez(t0, 3), addiu(v1, v1, 5), addiu(v1, v1, 100), addiu(v1, v1, 1000)});
            save(p, v1, 1);
            ret(p);
        });
    }
    s.push("conditional branch into a delay slot", [](Program& p, auto&) {
        // Parity of the (random) $t8 picks between jumping into the slot and running the branch inline.
        p.emit({li(v0, 0), andi(t0, t8, 1), bnez(t0, 2), nop()});  // -> word 5, the delay slot below
        p.emit({bnez(t5, 3), addiu(v0, v0, 5), addiu(v0, v0, 20), addiu(v0, v0, 40)});
        save(p, v0, 0);
        ret(p);
    });

    // ---- calls ----
    s.push("jal basics", [](Program& p, auto&) {
        uint32_t f = p.base() + 0x100;
        main_begin(p);
        p.emit({jal(f), li(a0, 5)});
        save(p, v0, 0);
        p.emit({jal(f), nop()});
        save(p, v0, 1);
        p.emit({jal(f), addu(a0, v0, v0)});
        save(p, v0, 2);
        p.emit({jal(f), move(s0, ra)});  // delay slot reads the link
        save(p, s0, 3);
        save(p, ra, 4);
        main_end(p);
        pad_to(p, f);
        p.emit({sll(v1, a0, 1), addu(v0, v1, a0), jr(ra), addiu(v0, v0, 1)});
    });
    s.push("nested calls with stack frames", [](Program& p, auto&) {
        uint32_t g = p.base() + 0x100, h = p.base() + 0x180;
        main_begin(p);
        p.emit({jal(g), li(a0, 7)});
        save(p, v0, 0);
        p.emit({jal(g), li(a0, -3)});
        save(p, v0, 1);
        main_end(p);
        pad_to(p, g);  // g(x) = h(x) + h(x + 1)
        p.emit({addiu(sp, sp, -24), sw(ra, 16, sp), sw(s0, 20, sp), jal(h), move(s0, a0), sw(v0, 12, sp),
                jal(h), addiu(a0, s0, 1), lw(t0, 12, sp), nop(), addu(v0, v0, t0), lw(ra, 16, sp), lw(s0, 20, sp),
                jr(ra), addiu(sp, sp, 24)});
        pad_to(p, h);  // h(x) = 2x + 1
        p.emit({sll(v0, a0, 1), jr(ra), addiu(v0, v0, 1)});
    });
    s.push("recursive factorial", [](Program& p, auto&) {
        uint32_t f = p.base() + 0x100;
        main_begin(p);
        p.emit({jal(f), li(a0, 10)});
        save(p, v0, 0);
        p.emit({jal(f), li(a0, 0)});
        save(p, v0, 1);
        p.emit({jal(f), li(a0, 1)});
        save(p, v0, 2);
        main_end(p);
        pad_to(p, f);
        p.emit({addiu(sp, sp, -24), sw(ra, 16, sp), sw(s0, 20, sp), move(s0, a0), slti(t0, a0, 2)});
        uint32_t br = p.pc();
        p.emit({0, nop(), li(v0, 1)});
        uint32_t bdone = p.pc();
        p.emit({0, nop()});
        uint32_t rec = p.pc();
        p.emit({jal(f), addiu(a0, a0, -1), mult(v0, s0), mflo(v0)});
        uint32_t done = p.pc();
        p.emit({lw(ra, 16, sp), lw(s0, 20, sp), addiu(sp, sp, 24), jr(ra), nop()});
        p.patch(br, beqz(t0, boff(br, rec)));
        p.patch(bdone, b(boff(bdone, done)));
    });
    s.push("recursive fibonacci", [](Program& p, auto&) {
        uint32_t f = p.base() + 0x100;
        main_begin(p);
        p.emit({jal(f), li(a0, 11)});
        save(p, v0, 0);
        p.emit({jal(f), li(a0, 1)});
        save(p, v0, 1);
        main_end(p);
        pad_to(p, f);
        p.emit({addiu(sp, sp, -32), sw(ra, 24, sp), sw(s0, 16, sp), sw(s1, 20, sp), move(s0, a0), slti(t0, a0, 2)});
        uint32_t br = p.pc();
        p.emit({0, nop()});
        p.emit({jal(f), addiu(a0, s0, -1), move(s1, v0), jal(f), addiu(a0, s0, -2), addu(v0, v0, s1)});
        uint32_t bdone = p.pc();
        p.emit({0, nop()});
        uint32_t base_case = p.pc();
        p.emit(move(v0, a0));
        uint32_t done = p.pc();
        p.emit({lw(ra, 24, sp), lw(s1, 20, sp), lw(s0, 16, sp), addiu(sp, sp, 32), jr(ra), nop()});
        p.patch(br, bnez(t0, boff(br, base_case)));
        p.patch(bdone, b(boff(bdone, done)));
    });
    s.push("jalr to constant addresses and aliases", [](Program& p, auto& extra) {
        uint32_t f0 = p.base() + 0x100, f1 = p.base() + 0x120, f2 = p.base() + 0x140;
        extra = {f0, f1, f2};
        main_begin(p);
        li32(p, t9, f1);
        p.emit({jalr(t9), li(a0, 10)});
        save(p, v0, 0);
        li32(p, t0, f2);
        p.emit({jalr(t0), nop()});
        save(p, v0, 1);
        li32(p, t9, (f1 & 0x1FFFFFFFu) | 0xA0000000u);  // KSEG1 alias
        p.emit({jalr(t9), li(a0, 3)});
        save(p, v0, 2);
        li32(p, t9, f0 & 0x1FFFFFFFu);  // KUSEG alias
        p.emit({jalr(t9), li(a0, 4)});
        save(p, v0, 3);
        li32(p, t8, f0);
        p.emit({jalr(t8, ra), move(a0, ra)});  // delay slot sees the link
        save(p, v0, 4);
        main_end(p);
        pad_to(p, f0);
        p.emit({jr(ra), addiu(v0, a0, 1)});
        pad_to(p, f1);
        p.emit({sll(v0, a0, 1), jr(ra), addiu(v0, v0, 3)});
        pad_to(p, f2);
        p.emit({mult(a0, a0), mflo(v0), jr(ra), nop()});
    });
    s.push("jalr through a pointer table in memory", [](Program& p, auto& extra) {
        uint32_t f0 = p.base() + 0x100, f1 = p.base() + 0x120, f2 = p.base() + 0x140;
        extra = {f0, f1, f2};
        main_begin(p);
        li32(p, t0, f0);
        p.emit(sw(t0, 0x200, s7));
        li32(p, t0, f1);
        p.emit(sw(t0, 0x204, s7));
        li32(p, t0, f2);
        p.emit(sw(t0, 0x208, s7));
        p.emit(li(s5, 0));
        uint32_t top = p.pc();
        p.emit({sll(t0, s5, 2), addu(t0, t0, s7), lw(t9, 0x200, t0), nop(), jalr(t9), addiu(a0, s5, 6),
                sw(v0, 0x300, t0), addiu(s5, s5, 1), slti(t1, s5, 3)});
        p.emit({bnez(t1, boff(p.pc(), top)), nop()});
        main_end(p);
        pad_to(p, f0);
        p.emit({jr(ra), addiu(v0, a0, 1)});
        pad_to(p, f1);
        p.emit({sll(v0, a0, 1), jr(ra), addiu(v0, v0, 3)});
        pad_to(p, f2);
        p.emit({mult(a0, a0), mflo(v0), jr(ra), nop()});
    });
    s.push("switch via jump table", [](Program& p, auto&) {
        uint32_t f = p.base() + 0x100, table = p.base() + 0x1E0;
        main_begin(p);
        int idx = 0;
        for (int32_t k : {0, 1, 2, 3, 4, 5, 6, -1}) {
            p.emit({jal(f), li(a0, k)});
            save(p, v0, idx++);
        }
        main_end(p);
        pad_to(p, f);
        p.emit({sltiu(v0, a0, 5)});
        uint32_t br = p.pc();
        p.emit({0, sll(v0, a0, 2), lui(at, (table + 0x8000) >> 16), addu(at, at, v0),
                lw(v0, static_cast<int16_t>(table & 0xFFFF), at), nop(), jr(v0), nop()});
        uint32_t c0 = p.pc();
        p.emit({jr(ra), li(v0, 100)});
        uint32_t c1 = p.pc();
        p.emit({jr(ra), li(v0, 101)});
        uint32_t c2 = p.pc();
        uint32_t c2_b = p.pc() + 4;
        p.emit({li(v0, 200), 0, nop()});
        uint32_t c3 = p.pc();
        uint32_t c3_b = p.pc() + 4;
        p.emit({li(v0, 300), 0, nop()});
        uint32_t c4 = p.pc();
        p.emit(li(v0, 400));  // falls through into join
        uint32_t join = p.pc();
        p.emit({addiu(v0, v0, 1), jr(ra), nop()});
        uint32_t dflt = p.pc();
        p.emit({jr(ra), li(v0, -1)});
        p.patch(br, beqz(v0, boff(br, dflt)));
        p.patch(c2_b, b(boff(c2_b, join)));
        p.patch(c3_b, b(boff(c3_b, join)));
        pad_to(p, table);
        p.emit({c0, c1, c2, c3, c4});
    });
    s.push("tail call via j", [](Program& p, auto& extra) {
        uint32_t tc = p.base() + 0x100, other = p.base() + 0x120;
        extra = {other};
        main_begin(p);
        p.emit({jal(tc), li(a0, 7)});
        save(p, v0, 0);
        main_end(p);
        pad_to(p, tc);
        p.emit({j(other), addiu(a0, a0, 1)});
        pad_to(p, other);
        p.emit({sll(v0, a0, 1), jr(ra), addiu(v0, v0, 1)});
    });
    s.push("tail call via conditional branch", [](Program& p, auto& extra) {
        uint32_t tc = p.base() + 0x100, other = p.base() + 0x120;
        extra = {other};
        main_begin(p);
        p.emit({jal(tc), li(a0, 0)});
        save(p, v0, 0);
        p.emit({jal(tc), li(a0, 3)});
        save(p, v0, 1);
        main_end(p);
        pad_to(p, tc);
        p.emit({beqz(a0, boff(p.pc(), other)), li(v1, 33), li(v0, 5), jr(ra), nop()});
        pad_to(p, other);
        p.emit({addu(v0, v1, v1), jr(ra), nop()});
    });
    s.push("function falling through into the next function", [](Program& p, auto&) {
        uint32_t a_fn = p.base() + 0x100, b_fn = p.base() + 0x108;
        main_begin(p);
        p.emit({li(v0, 0), jal(a_fn), nop()});
        save(p, v0, 0);
        p.emit({jal(b_fn), nop()});
        save(p, v0, 1);
        main_end(p);
        pad_to(p, a_fn);
        p.emit({addiu(v0, v0, 1), nop()});
        p.emit({addiu(v0, v0, 2), jr(ra), nop()});
    });

    // ---- load delay slot ----
    s.push("load delay: use of loaded register sees the old value", [](Program& p, auto&) {
        p.emit({lw(t0, 0, s7), addu(t1, t0, t0), nop()});
        save(p, t0, 0);
        save(p, t1, 1);
        p.emit({lw(t2, 4, s7), sw(t2, 0x500, s7), nop()});
        p.emit({lb(t3, 1, s7), sll(t4, t3, 2), nop(), lh(t5, 2, s7), mult(t5, t5), mflo(t6)});
        p.emit({lw(t7, 8, s7), mthi(t7), nop(), mfhi(s0), lw(s1, 12, s7), sllv(s2, s1, s1), nop()});
        p.emit({lbu(a0, 3, s7), slti(a1, a0, 100), sltiu(a2, a0, 3), lhu(a3, 6, s7), subu(v0, a3, a3), nop()});
        ret(p);
    });
    s.push("load delay: load through the old register as pointer", [](Program& p, auto&) {
        p.emit({addiu(t0, s7, 0x10), lw(t0, 0, s7), lw(t1, 0, t0), nop()});  // second load uses the OLD t0
        save(p, t0, 0);
        save(p, t1, 1);
        // chained: load, load using the loaded reg, then use of the second load
        p.emit({addiu(t2, s7, 0x20), addiu(t4, s7, 0x30), lw(t2, 0, s7), lw(t3, 4, t2), addu(t5, t3, t3), nop()});
        save(p, t2, 2);
        save(p, t3, 3);
        save(p, t5, 4);
        // same register twice: the third instruction sees the first load, the second lands afterwards
        p.emit({addiu(t6, s7, 0x40), lw(t6, 0, s7), lw(t6, 4, t6), addu(t7, t6, t6), nop()});
        save(p, t6, 5);
        save(p, t7, 6);
        ret(p);
    });
    s.push("load delay: load overwritten by alu / second load", [](Program& p, auto&) {
        p.emit({lw(t0, 0, s7), addu(t0, t1, t1), nop()});  // the ALU write wins
        save(p, t0, 0);
        p.emit({lw(t2, 0, s7), lw(t2, 4, s7), addu(t3, t2, t2), nop()});  // sees the FIRST load
        save(p, t2, 1);
        save(p, t3, 2);
        p.emit({lw(t4, 8, s7), lui(t5, 0x1234), addu(t6, t4, t4), nop()});  // t4 landed after lui
        save(p, t6, 3);
        p.emit({lw(t7, 12, s7), nop(), addu(s0, t7, t7), nop()});
        save(p, s0, 4);
        p.emit({lw(zero, 0, s7), addu(s1, zero, zero), lw(s2, 4, s7), or_(s2, s2, s2), nop()});
        save(p, s2, 5);
        ret(p);
    });
    s.push("load delay: load before a branch whose delay slot uses it", [](Program& p, auto&) {
        p.emit({lw(t0, 0, s7), bnez(t3, 2), addu(t1, t0, t0), li(v0, 1), nop()});  // slot sees the loaded value
        save(p, t0, 0);
        save(p, t1, 1);
        p.emit({lw(t4, 4, s7), beqz(zero, 2), addu(t5, t4, t4), li(v1, 1), nop()});
        save(p, t5, 2);
        ret(p);
    });
    s.push("load delay: lwl/lwr forwarding", [](Program& p, auto&) {
        p.emit({lwl(t0, 3, s7), lwr(t0, 0, s7), addu(t1, t0, t0), nop()});
        save(p, t0, 0);
        save(p, t1, 1);
        p.emit({lwr(t2, 5, s7), lwl(t2, 8, s7), sw(t2, 0x500, s7), nop()});
        save(p, t2, 2);
        p.emit({lw(t3, 0x10, s7), lwl(t3, 0x17, s7), addu(t4, t3, t3), nop()});  // lw then lwl: lwl sees the lw result
        save(p, t3, 3);
        save(p, t4, 4);
        p.emit({lwl(t5, 0x1A, s7), lwr(t6, 0x18, s7), addu(t7, t5, t6), nop()});  // different registers
        save(p, t7, 5);
        ret(p);
    });

    // ---- randomized ----
    for (uint64_t seed = 1; seed <= 220; ++seed)
        s.push("random straight-line seed " + std::to_string(seed), [=](Program& p, auto&) { random_straight(p, seed); });
    for (uint64_t seed = 1000; seed < 1040; ++seed)
        s.push("random forward-branch seed " + std::to_string(seed), [=](Program& p, auto&) { random_branchy(p, seed); });
    return std::move(s.programs());
}

}  // namespace tp
