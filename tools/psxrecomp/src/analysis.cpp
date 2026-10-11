#include "psxrecomp/analysis.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <optional>

namespace psxrecomp {

uint32_t CodeSegment::word(uint32_t addr) const {
    if (!contains(addr) || addr + 4 > end()) return 0;
    std::size_t o = addr - base;
    return static_cast<uint32_t>(bytes[o]) | (static_cast<uint32_t>(bytes[o + 1]) << 8) |
           (static_cast<uint32_t>(bytes[o + 2]) << 16) | (static_cast<uint32_t>(bytes[o + 3]) << 24);
}

bool match_bios_stub(const CodeSegment& seg, uint32_t start, uint8_t& table, uint8_t& fn) {
    // addiu t2,zero,0xA0 ; jr t2 ; addiu t1,zero,N
    uint32_t w0 = seg.word(start), w1 = seg.word(start + 4), w2 = seg.word(start + 8);
    if ((w0 & 0xFFFFFF00u) != 0x240A0000u) return false;
    uint32_t t = w0 & 0xFF;
    if (t != 0xA0 && t != 0xB0 && t != 0xC0) return false;
    if (w1 != 0x01400008u) return false;              // jr t2
    if ((w2 & 0xFFFFFF00u) != 0x24090000u) return false;  // addiu t1,zero,imm8
    table = static_cast<uint8_t>(t);
    fn = static_cast<uint8_t>(w2 & 0xFF);
    return true;
}

namespace {

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

bool is_prologue(uint32_t w) { return (w >> 16) == 0x27BDu && (w & 0x8000u); }  // addiu sp,sp,-N

class Analyzer {
public:
    Analyzer(const CodeSegment& seg, const AnalysisConfig& cfg)
        : seg_(seg), cfg_(cfg), code_end_(cfg.code_end ? cfg.code_end : seg.end()) {}

    AnalysisResult run();

private:
    Function trace(uint32_t start) const;
    std::optional<uint32_t> const_base(uint32_t reg, uint32_t before, int depth) const;
    std::optional<JumpTable> recover_jump_table(const Function& f, uint32_t jr_pc, uint8_t reg) const;
    bool add_start(uint32_t addr, uint32_t source);
    // Addresses at/after code_end belong to other segments at runtime (e.g. overlay slots inside the exe image).
    bool in_code(uint32_t addr) const { return addr >= seg_.base && addr < code_end_; }
    void scan_data_pointers(AnalysisResult& res);
    void scan_jal_words();
    void gap_fill();

    const CodeSegment& seg_;
    const AnalysisConfig& cfg_;
    uint32_t code_end_;
    std::map<uint32_t, uint32_t> starts_;  // start -> source mask
    std::deque<uint32_t> queue_;
    std::map<uint32_t, Function> funcs_;
};

bool Analyzer::add_start(uint32_t addr, uint32_t source) {
    if (!in_code(addr) || (addr & 3) || cfg_.not_functions.count(addr)) return false;
    auto [it, inserted] = starts_.try_emplace(addr, 0);
    it->second |= source;
    if (inserted) queue_.push_back(addr);
    return inserted;
}

// Resolves the constant part of `reg` as computed by lui/addiu/ori/addu chains before `before`.
std::optional<uint32_t> Analyzer::const_base(uint32_t reg, uint32_t before, int depth) const {
    if (reg == 0) return 0u;
    if (depth > 6) return std::nullopt;
    for (uint32_t pc = before - 4, n = 0; n < 256 && pc >= seg_.base; pc -= 4, ++n) {
        Instruction i = decode(seg_.word(pc), pc);
        if (i.dest_gpr() != reg) continue;
        switch (i.op) {
            case Op::LUI: return i.uimm << 16;
            case Op::ADDIU:
            case Op::ADDI:
                if (auto b = const_base(i.rs, pc, depth + 1)) return *b + static_cast<uint32_t>(i.simm);
                return std::nullopt;
            case Op::ORI:
                if (auto b = const_base(i.rs, pc, depth + 1)) return *b | i.uimm;
                return std::nullopt;
            case Op::ADDU:
            case Op::ADD:
                if (auto b = const_base(i.rs, pc, depth + 1)) return b;
                return const_base(i.rt, pc, depth + 1);
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<JumpTable> Analyzer::recover_jump_table(const Function& f, uint32_t jr_pc, uint8_t reg) const {
    // Find the load that produced the jump register.
    uint32_t lw_pc = 0;
    Instruction lw;
    for (uint32_t pc = jr_pc - 4, n = 0; n < 16 && pc >= f.start; pc -= 4, ++n) {
        Instruction i = decode(seg_.word(pc), pc);
        if (i.dest_gpr() == reg) {
            if (i.op != Op::LW) return std::nullopt;
            lw = i;
            lw_pc = pc;
            break;
        }
    }
    if (!lw_pc) return std::nullopt;
    auto base = const_base(lw.rs, lw_pc, 0);
    if (!base) return std::nullopt;
    JumpTable jt;
    jt.jr_addr = jr_pc;
    jt.table_addr = *base + static_cast<uint32_t>(lw.simm);
    if (!seg_.contains(jt.table_addr) || (jt.table_addr & 3)) return std::nullopt;

    // Bound from a preceding "sltiu rX, idx, N".
    uint32_t count = 0;
    for (uint32_t pc = lw_pc - 4, n = 0; n < 32 && pc >= f.start; pc -= 4, ++n) {
        Instruction i = decode(seg_.word(pc), pc);
        if (i.op == Op::SLTIU) {
            count = i.uimm;
            break;
        }
    }
    bool bounded = count != 0;
    if (!bounded) count = 512;
    // Unbounded tables are cut at the next known function so adjacent tables are not swallowed.
    auto next = starts_.upper_bound(f.start);
    uint32_t limit = next != starts_.end() ? next->first : code_end_;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t a = jt.table_addr + k * 4;
        if (!seg_.contains(a)) break;
        uint32_t t = seg_.word(a);
        if (!in_code(t) || (t & 3) || t < f.start || (!bounded && t >= limit) ||
            decode(seg_.word(t), t).op == Op::Invalid) {
            if (bounded) return std::nullopt;  // bounded table with a bad entry: not a table
            break;
        }
        jt.targets.push_back(t);
    }
    if (jt.targets.empty()) return std::nullopt;
    return jt;
}

Function Analyzer::trace(uint32_t start) const {
    Function f;
    f.start = start;
    if (match_bios_stub(seg_, start, f.bios_table, f.bios_fn)) {
        f.kind = FuncKind::BiosStub;
        f.end = start + 12;
        f.insns = {start, start + 4, start + 8};
        return f;
    }
    std::set<uint32_t> visited;
    std::set<uint32_t> flowed;  // addresses walked as ordinary instructions (not just as a delay slot)
    std::deque<uint32_t> work{start};
    uint32_t max_pc = start;

    auto is_other_start = [&](uint32_t a) { return a != start && starts_.count(a); };
    auto add_target = [&](uint32_t t) {
        if (!in_code(t) || t < start || is_other_start(t)) f.tail_calls.insert(t);
        else work.push_back(t);
    };
    auto visit = [&](uint32_t pc) {
        visited.insert(pc);
        max_pc = std::max(max_pc, pc);
    };

    while (!work.empty()) {
        uint32_t pc = work.front();
        work.pop_front();
        while (true) {
            if (flowed.count(pc)) break;
            if (!in_code(pc)) {
                f.errors.push_back("runs off segment at " + hex(pc));
                break;
            }
            if (pc != start && is_other_start(pc)) {  // fell through into the next function
                f.tail_calls.insert(pc);
                break;
            }
            Instruction i = decode(seg_.word(pc), pc);
            if (i.op == Op::Invalid) {
                f.errors.push_back("invalid instruction at " + hex(pc));
                break;
            }
            visit(pc);
            flowed.insert(pc);
            if (i.has_delay_slot()) {
                Instruction ds = decode(seg_.word(pc + 4), pc + 4);
                if (ds.op == Op::Invalid) {
                    f.errors.push_back("invalid delay slot at " + hex(pc + 4));
                    break;
                }
                if (ds.has_delay_slot()) f.errors.push_back("branch in delay slot at " + hex(pc + 4));
                visit(pc + 4);
            }
            bool stop = false;
            switch (i.klass()) {
                case InsnClass::Branch:
                    if (i.is_call()) {
                        f.callees.insert(i.target);
                        // bgezal zero == bal: unconditional call, still returns here.
                        pc += 8;
                    } else {
                        add_target(i.target);
                        bool always = (i.op == Op::BEQ && i.rs == i.rt) || (i.op == Op::BGEZ && i.rs == 0) ||
                                      (i.op == Op::BLEZ && i.rs == 0);
                        if (always) stop = true;
                        else pc += 8;
                    }
                    break;
                case InsnClass::Jump:
                    if (i.op == Op::JAL) {
                        f.callees.insert(i.target);
                        pc += 8;
                    } else {
                        add_target(i.target);
                        stop = true;
                    }
                    break;
                case InsnClass::JumpReg:
                    if (i.op == Op::JALR) {
                        f.indirect_calls.push_back(pc);
                        pc += 8;
                    } else {
                        if (i.rs != 31) {
                            if (auto jt = recover_jump_table(f, pc, i.rs)) {
                                for (uint32_t t : jt->targets) add_target(t);
                                f.jump_tables.push_back(std::move(*jt));
                            } else {
                                f.indirect_jumps.push_back(pc);
                            }
                        }
                        stop = true;
                    }
                    break;
                case InsnClass::System:
                    if (i.op == Op::BREAK) stop = true;
                    else pc += 4;
                    break;
                default: pc += 4; break;
            }
            if (stop) break;
        }
    }
    f.end = max_pc + 4;
    f.insns.assign(visited.begin(), visited.end());
    return f;
}

void Analyzer::scan_data_pointers(AnalysisResult& res) {
    // Words outside traced code that point at a function prologue inside the code range.
    std::vector<std::pair<uint32_t, uint32_t>> code_ranges;
    for (const auto& [s, f] : funcs_) code_ranges.emplace_back(f.start, f.end);
    auto in_code = [&](uint32_t a) {
        auto it = std::upper_bound(code_ranges.begin(), code_ranges.end(), std::make_pair(a, UINT32_MAX));
        return it != code_ranges.begin() && a < std::prev(it)->second;
    };
    for (uint32_t a = seg_.base; a + 4 <= seg_.end(); a += 4) {
        if (in_code(a)) continue;
        uint32_t t = seg_.word(a);
        if (t < seg_.base || t >= code_end_ || (t & 3)) continue;
        if (!is_prologue(seg_.word(t))) continue;
        add_start(t, kSourceDataPointer);
    }
    (void)res;
}

void Analyzer::scan_jal_words() {
    for (uint32_t a = seg_.base; a + 4 <= code_end_; a += 4) {
        uint32_t w = seg_.word(a);
        if ((w >> 26) != 3) continue;
        uint32_t t = ((a + 4) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2);
        if (t >= seg_.base && t < code_end_ && is_prologue(seg_.word(t))) add_start(t, kSourceJal);
    }
}

void Analyzer::gap_fill() {
    std::vector<std::pair<uint32_t, uint32_t>> gaps;  // [start, end) not covered by any function
    uint32_t cur = seg_.base;
    for (const auto& [s, f] : funcs_) {
        if (s > cur) gaps.emplace_back(cur, s);
        cur = std::max(cur, f.end);
    }
    if (cur < code_end_) gaps.emplace_back(cur, code_end_);

    uint32_t covered_until = 0;  // end of the last function accepted in this pass
    auto try_accept = [&](uint32_t start, uint32_t gap_end, uint32_t must_cover) {
        if (starts_.count(start) || cfg_.not_functions.count(start)) return false;
        Function f = trace(start);
        if (!f.errors.empty() || f.end - f.start < 8 || f.end > gap_end) return false;
        if (must_cover && (must_cover < f.start || must_cover >= f.end)) return false;
        add_start(start, kSourceGap);
        covered_until = std::max(covered_until, f.end);
        return true;
    };

    for (auto [gap_start, gap_end] : gaps) {
        // 1. Forward fill: compiled functions are contiguous, so code usually resumes right after the
        //    previous function (skipping alignment padding). Not applied to a leading gap (often rodata).
        uint32_t pc = gap_start;
        if (gap_start != seg_.base) {
            while (pc < gap_end) {
                while (pc < gap_end && seg_.word(pc) == 0) pc += 4;
                if (pc >= gap_end || !try_accept(pc, gap_end, 0)) break;
                pc = trace(pc).end;
            }
        }
        // 2. Return-anchored: every uncovered "jr ra" belongs to some function. Walk back to the nearest
        //    prologue after the previous return, or to just after that return for leaf functions.
        uint32_t boundary = gap_start;
        for (uint32_t r = gap_start; r + 8 <= gap_end; r += 4) {
            if (seg_.word(r) != 0x03E00008u) continue;
            if (r < covered_until) {  // a mid-function return of something already accepted
                boundary = r + 8;
                continue;
            }
            uint32_t cand = 0;
            for (uint32_t a = r; a >= boundary && a >= seg_.base; a -= 4) {
                if (is_prologue(seg_.word(a))) {
                    cand = a;
                    break;
                }
                if (a == seg_.base) break;
            }
            bool ok = cand && try_accept(cand, gap_end, r);
            if (!ok) {
                // Leaf function: start after the last non-instruction word (data) since the boundary.
                uint32_t leaf = boundary;
                for (uint32_t a = boundary; a < r; a += 4)
                    if (decode(seg_.word(a), a).op == Op::Invalid) leaf = a + 4;
                while (leaf < r && seg_.word(leaf) == 0) leaf += 4;
                if (leaf != cand) try_accept(leaf, gap_end, r);
            }
            boundary = r + 8;
        }
    }
}

AnalysisResult Analyzer::run() {
    AnalysisResult res;
    for (uint32_t e : cfg_.entries) add_start(e, kSourceEntry);
    for (uint32_t e : cfg_.extra_functions) add_start(e, kSourceHint);
    if (cfg_.scan_jal_words) scan_jal_words();

    for (int pass = 0; pass < 16; ++pass) {
        // Trace until no new direct-call / tail-call starts appear.
        while (!queue_.empty()) {
            uint32_t s = queue_.front();
            queue_.pop_front();
            Function f = trace(s);
            for (uint32_t c : f.callees) add_start(c, kSourceJal);
            for (uint32_t t : f.tail_calls) add_start(t, kSourceTailCall);
            funcs_[s] = std::move(f);
        }
        std::size_t before = starts_.size();
        if (cfg_.scan_data_pointers) scan_data_pointers(res);
        if (queue_.empty() && cfg_.gap_fill) gap_fill();
        if (starts_.size() == before && queue_.empty()) break;
    }

    // Final pass: re-trace everything with the complete start set so bodies stop at other functions.
    for (auto& [s, f] : funcs_) {
        f = trace(s);
        f.source = starts_[s];
        for (uint32_t c : f.callees)
            if (!in_code(c)) res.external_calls.insert(c);
        for (uint32_t t : f.tail_calls)
            if (!in_code(t)) res.external_calls.insert(t);
    }
    res.functions = std::move(funcs_);

    const Function* prev = nullptr;
    for (const auto& [s, f] : res.functions) {
        if (prev && prev->end > f.start)
            res.warnings.push_back("function " + hex(prev->start) + " overlaps " + hex(f.start));
        prev = &f;
    }
    return res;
}

}  // namespace

AnalysisResult analyze(const CodeSegment& seg, const AnalysisConfig& cfg) { return Analyzer(seg, cfg).run(); }

}  // namespace psxrecomp
