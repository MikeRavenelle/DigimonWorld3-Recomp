#include "psxrecomp/emitter.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace psxrecomp {
namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

std::string hex8(uint32_t v) { return fmt("%08X", v); }

std::string R(uint8_t r) { return r == 0 ? std::string("0u") : fmt("ctx->r[%u]", r); }

std::string label(uint32_t a) { return "L_" + hex8(a); }

// GPRs read by an instruction (used for load-delay hazard detection).
std::set<uint8_t> reads(const Instruction& i) {
    switch (i.op) {
        case Op::SLL: case Op::SRL: case Op::SRA: return {i.rt};
        case Op::MFHI: case Op::MFLO: case Op::LUI: case Op::J: case Op::JAL: case Op::SYSCALL:
        case Op::BREAK: case Op::RFE: case Op::MFC0: case Op::MFC2: case Op::CFC2: case Op::GTE_CMD:
        case Op::Invalid:
            return {};
        case Op::MTHI: case Op::MTLO: case Op::JR: case Op::JALR: case Op::BLTZ: case Op::BGEZ:
        case Op::BLTZAL: case Op::BGEZAL: case Op::BLEZ: case Op::BGTZ: case Op::ADDI: case Op::ADDIU:
        case Op::SLTI: case Op::SLTIU: case Op::ANDI: case Op::ORI: case Op::XORI: case Op::LB: case Op::LH:
        case Op::LW: case Op::LBU: case Op::LHU: case Op::LWC2: case Op::SWC2:
            return {i.rs};
        case Op::LWL: case Op::LWR:  // merge with the old register value
            return {i.rs, i.rt};
        case Op::MTC0: case Op::MTC2: case Op::CTC2: return {i.rt};
        default:  // R-type ALU, mult/div, branches with two operands, stores
            return {i.rs, i.rt};
    }
}

bool is_load(Op op) {
    return op == Op::LB || op == Op::LH || op == Op::LW || op == Op::LBU || op == Op::LHU || op == Op::LWL ||
           op == Op::LWR;
}

class FuncEmitter {
public:
    FuncEmitter(const EmitContext& ec, const Function& f, std::vector<std::string>* warnings,
                std::vector<uint32_t>* resume_out)
        : ec_(ec), f_(f), warnings_(warnings), resume_out_(resume_out), seg_name_(ec.seg->name) {}

    std::string run();

private:
    Instruction at(uint32_t pc) const { return decode(ec_.seg->word(pc), pc); }
    bool in_func(uint32_t a) const { return std::binary_search(f_.insns.begin(), f_.insns.end(), a); }
    bool is_local_target(uint32_t t) const {
        return t >= f_.start && in_func(t) && !(t != f_.start && ec_.local->count(t));
    }
    void warn(const std::string& w) {
        if (warnings_) warnings_->push_back(seg_name_ + " " + func_name(seg_name_, f_.start) + ": " + w);
    }

    std::string call_expr(uint32_t target) const;
    // A call that returns here: records the frame so a save state can resume at `link`.
    std::string call_site(const std::string& call, uint32_t link) const;
    std::string stmt(const Instruction& i);  // non-control instruction
    std::string load_value(const Instruction& i);
    std::string addr(const Instruction& i) const;
    std::string branch_cond(const Instruction& i) const;
    // Goto a local label, or tail-call. Adds an event check on backward jumps.
    std::string jump_to(uint32_t from, uint32_t t) const;

    void line(const std::string& s) { out_ += "    " + s + "\n"; }
    void flush_cycles() {
        if (pending_ && ec_.cycle_counting) line(fmt("ctx->cycles += %u;", pending_));
        pending_ = 0;
    }
    void collect_labels();

    const EmitContext& ec_;
    const Function& f_;
    std::vector<std::string>* warnings_;
    std::vector<uint32_t>* resume_out_;
    std::string seg_name_;
    std::string out_;
    std::set<uint32_t> labels_;
    // Labels a restored call chain can re-enter this function at (return points of calls and backward-jump
    // targets), and return points that lie outside the function's own code.
    mutable std::set<uint32_t> resume_, tail_links_;
    unsigned pending_ = 0;
};

std::string FuncEmitter::call_expr(uint32_t t) const {
    if (ec_.local->count(t)) return func_name(seg_name_, t) + "(ctx);";
    if (ec_.main && ec_.main->count(t)) return func_name(ec_.main_name, t) + "(ctx);";
    return fmt("dw3_call(ctx, 0x%08Xu);", t);
}

std::string FuncEmitter::call_site(const std::string& call, uint32_t link) const {
    if (is_local_target(link)) resume_.insert(link);
    else tail_links_.insert(link);
    return call;
}

std::string FuncEmitter::jump_to(uint32_t from, uint32_t t) const {
    if (is_local_target(t)) {
        std::string g = "goto " + label(t) + ";";
        if (t <= from && ec_.cycle_counting) {
            resume_.insert(t);
            return fmt("DW3_CHECK_EVENTS(ctx, 0x%08Xu); ", t) + g;
        }
        return g;
    }
    return call_expr(t) + " return;";
}

std::string FuncEmitter::addr(const Instruction& i) const {
    if (i.rs == 0) return fmt("0x%08Xu", static_cast<uint32_t>(i.simm));
    if (i.simm == 0) return R(i.rs);
    return R(i.rs) + fmt(" + (uint32_t)%d", i.simm);
}

std::string FuncEmitter::load_value(const Instruction& i) {
    std::string a = addr(i);
    switch (i.op) {
        case Op::LB: return "(uint32_t)(int32_t)(int8_t)dw3_read8(ctx, " + a + ")";
        case Op::LBU: return "(uint32_t)dw3_read8(ctx, " + a + ")";
        case Op::LH: return "(uint32_t)(int32_t)(int16_t)dw3_read16(ctx, " + a + ")";
        case Op::LHU: return "(uint32_t)dw3_read16(ctx, " + a + ")";
        case Op::LW: return "dw3_read32(ctx, " + a + ")";
        case Op::LWL: return "dw3_lwl(ctx, " + a + ", " + R(i.rt) + ")";
        case Op::LWR: return "dw3_lwr(ctx, " + a + ", " + R(i.rt) + ")";
        default: return "0u";
    }
}

std::string FuncEmitter::stmt(const Instruction& i) {
    auto set = [&](uint8_t r, const std::string& v) -> std::string {
        if (r == 0) return "";  // writes to $zero are discarded (and ALU has no side effects)
        return R(r) + " = " + v + ";";
    };
    const std::string rs = R(i.rs), rt = R(i.rt);
    switch (i.op) {
        case Op::SLL:
            if (i.raw == 0) return "";  // nop
            return set(i.rd, rt + fmt(" << %u", i.shamt));
        case Op::SRL: return set(i.rd, rt + fmt(" >> %u", i.shamt));
        case Op::SRA: return set(i.rd, "(uint32_t)((int32_t)" + rt + fmt(" >> %u)", i.shamt));
        case Op::SLLV: return set(i.rd, rt + " << (" + rs + " & 31)");
        case Op::SRLV: return set(i.rd, rt + " >> (" + rs + " & 31)");
        case Op::SRAV: return set(i.rd, "(uint32_t)((int32_t)" + rt + " >> (" + rs + " & 31))");
        case Op::MFHI: return set(i.rd, "ctx->hi");
        case Op::MFLO: return set(i.rd, "ctx->lo");
        case Op::MTHI: return "ctx->hi = " + rs + ";";
        case Op::MTLO: return "ctx->lo = " + rs + ";";
        case Op::MULT: return "dw3_mult(ctx, " + rs + ", " + rt + ");";
        case Op::MULTU: return "dw3_multu(ctx, " + rs + ", " + rt + ");";
        case Op::DIV: return "dw3_div(ctx, " + rs + ", " + rt + ");";
        case Op::DIVU: return "dw3_divu(ctx, " + rs + ", " + rt + ");";
        // Overflow traps of ADD/ADDI/SUB are not modelled (no PS1 game relies on them).
        case Op::ADD: case Op::ADDU: return set(i.rd, rs + " + " + rt);
        case Op::SUB: case Op::SUBU: return set(i.rd, rs + " - " + rt);
        case Op::AND: return set(i.rd, rs + " & " + rt);
        case Op::OR: return set(i.rd, rs + " | " + rt);
        case Op::XOR: return set(i.rd, rs + " ^ " + rt);
        case Op::NOR: return set(i.rd, "~(" + rs + " | " + rt + ")");
        case Op::SLT: return set(i.rd, "((int32_t)" + rs + " < (int32_t)" + rt + ") ? 1u : 0u");
        case Op::SLTU: return set(i.rd, "(" + rs + " < " + rt + ") ? 1u : 0u");
        case Op::ADDI: case Op::ADDIU:
            if (i.rs == 0) return set(i.rt, fmt("0x%08Xu", static_cast<uint32_t>(i.simm)));
            return set(i.rt, rs + fmt(" + (uint32_t)%d", i.simm));
        case Op::SLTI: return set(i.rt, "((int32_t)" + rs + fmt(" < %d) ? 1u : 0u", i.simm));
        case Op::SLTIU: return set(i.rt, "(" + rs + fmt(" < 0x%08Xu) ? 1u : 0u", static_cast<uint32_t>(i.simm)));
        case Op::ANDI: return set(i.rt, rs + fmt(" & 0x%04Xu", i.uimm));
        case Op::ORI: return set(i.rt, rs + fmt(" | 0x%04Xu", i.uimm));
        case Op::XORI: return set(i.rt, rs + fmt(" ^ 0x%04Xu", i.uimm));
        case Op::LUI: return set(i.rt, fmt("0x%08Xu", i.uimm << 16));
        case Op::MFC0: return set(i.rt, fmt("ctx->cop0[%u]", i.rd));
        case Op::MTC0: return fmt("dw3_mtc0(ctx, %u, ", i.rd) + rt + ");";
        case Op::RFE: return "dw3_rfe(ctx);";
        case Op::MFC2: return i.rt ? set(i.rt, fmt("dw3_gte_read_data(ctx, %u)", i.rd)) : fmt("(void)dw3_gte_read_data(ctx, %u);", i.rd);
        case Op::CFC2: return i.rt ? set(i.rt, fmt("dw3_gte_read_ctrl(ctx, %u)", i.rd)) : "";
        case Op::MTC2: return fmt("dw3_gte_write_data(ctx, %u, ", i.rd) + rt + ");";
        case Op::CTC2: return fmt("dw3_gte_write_ctrl(ctx, %u, ", i.rd) + rt + ");";
        case Op::GTE_CMD: return fmt("dw3_gte_cmd(ctx, 0x%07Xu);", i.gte_cmd);
        case Op::LB: case Op::LBU: case Op::LH: case Op::LHU: case Op::LW: case Op::LWL: case Op::LWR:
            if (i.rt == 0) return "(void)" + load_value(i) + ";";  // keep I/O side effects
            return set(i.rt, load_value(i));
        case Op::SB: return "dw3_write8(ctx, " + addr(i) + ", (uint8_t)" + rt + ");";
        case Op::SH: return "dw3_write16(ctx, " + addr(i) + ", (uint16_t)" + rt + ");";
        case Op::SW: return "dw3_write32(ctx, " + addr(i) + ", " + rt + ");";
        case Op::SWL: return "dw3_swl(ctx, " + addr(i) + ", " + rt + ");";
        case Op::SWR: return "dw3_swr(ctx, " + addr(i) + ", " + rt + ");";
        case Op::LWC2: return fmt("dw3_gte_write_data(ctx, %u, dw3_read32(ctx, ", i.rt) + addr(i) + "));";
        case Op::SWC2: return "dw3_write32(ctx, " + addr(i) + fmt(", dw3_gte_read_data(ctx, %u));", i.rt);
        case Op::SYSCALL: return fmt("dw3_syscall(ctx, 0x%08Xu);", i.pc);
        default: return fmt("/* unhandled %s */", disassemble(i).c_str());
    }
}

std::string FuncEmitter::branch_cond(const Instruction& i) const {
    const std::string rs = R(i.rs), rt = R(i.rt);
    switch (i.op) {
        case Op::BEQ: return rs + " == " + rt;
        case Op::BNE: return rs + " != " + rt;
        case Op::BLEZ: return "(int32_t)" + rs + " <= 0";
        case Op::BGTZ: return "(int32_t)" + rs + " > 0";
        case Op::BLTZ: case Op::BLTZAL: return "(int32_t)" + rs + " < 0";
        case Op::BGEZ: case Op::BGEZAL: return "(int32_t)" + rs + " >= 0";
        default: return "0";
    }
}

void FuncEmitter::collect_labels() {
    auto add = [&](uint32_t t) {
        if (is_local_target(t)) labels_.insert(t);
    };
    for (uint32_t pc : f_.insns) {
        Instruction i = at(pc);
        if (i.klass() == InsnClass::Branch && !i.is_call()) add(i.target);
        if (i.op == Op::J) add(i.target);
        if (i.is_call()) add(pc + 8);  // return point: a restored call chain re-enters here
    }
    for (const auto& jt : f_.jump_tables)
        for (uint32_t t : jt.targets) add(t);
    for (const auto& pj : f_.pointer_jumps)
        if (!pj.via_slot) add(pj.addr);
    // A labelled delay slot is emitted standalone after its branch, so the branch's fallthrough
    // must jump over it to pc+8.
    for (uint32_t pc : f_.insns) {
        Instruction i = at(pc);
        if (i.has_delay_slot() && labels_.count(pc + 4) && in_func(pc + 8)) labels_.insert(pc + 8);
    }
}

std::string FuncEmitter::run() {
    out_ = "void " + func_name(seg_name_, f_.start) + "(RecompContext* ctx) {\n";
    if (f_.kind == FuncKind::BiosStub) {
        line(fmt("ctx->r[10] = 0x%02Xu;", f_.bios_table));
        line(fmt("ctx->r[9] = 0x%02Xu;", f_.bios_fn));
        line(fmt("dw3_bios_call(ctx, 0x%02Xu, 0x%02Xu);", f_.bios_table, f_.bios_fn));
        return out_ + "}\n";
    }
    collect_labels();
    const auto& ins = f_.insns;
    std::size_t k = 0;
    bool terminal = false;
    uint32_t expect = f_.start;  // address control falls through to
    while (k < ins.size()) {
        uint32_t pc = ins[k];
        if (!terminal && pc != expect) {
            // Fell out of this function's code into another function.
            flush_cycles();
            line(call_expr(expect) + " return;");
        }
        if (labels_.count(pc)) {
            flush_cycles();
            out_ += label(pc) + ":;\n";
        }
        Instruction i = at(pc);
        terminal = false;
        if (!i.has_delay_slot()) {
            ++pending_;
            if (i.op == Op::BREAK) {
                flush_cycles();
                line(fmt("dw3_break(ctx, 0x%08Xu); return;", pc));
                terminal = true;
                ++k;
                continue;
            }
            // Load delay: the next instruction still sees the old register value.
            if (is_load(i.op) && i.rt != 0 && k + 1 < ins.size() && ins[k + 1] == pc + 4 && !labels_.count(pc + 4)) {
                Instruction n = at(pc + 4);
                bool forwards = (n.op == Op::LWL || n.op == Op::LWR) && n.rt == i.rt;
                if (!n.has_delay_slot() && !forwards && reads(n).count(i.rt)) {
                    warn(fmt("load-delay hazard at %08X", pc));
                    // Chained loads (lw a; lw b,(a); use b): `n` is itself a load whose result the third
                    // instruction must not see yet, so all three are emitted together.
                    if (is_load(n.op) && n.rt != 0 && k + 2 < ins.size() && ins[k + 2] == pc + 8 &&
                        !labels_.count(pc + 8)) {
                        Instruction m = at(pc + 8);
                        bool m_forwards = (m.op == Op::LWL || m.op == Op::LWR) && m.rt == n.rt;
                        if (!m.has_delay_slot() && !is_load(m.op) && !m_forwards && reads(m).count(n.rt)) {
                            pending_ += 2;
                            std::string ms = stmt(m);
                            line("{ uint32_t ld = " + load_value(i) + ";");
                            line("  uint32_t ld2 = " + load_value(n) + ";");
                            line("  " + R(i.rt) + " = ld;");
                            if (!ms.empty()) line("  " + ms);
                            if (m.dest_gpr() != n.rt) line("  " + R(n.rt) + " = ld2; }");
                            else line("}");
                            k += 3;
                            expect = pc + 12;
                            continue;
                        }
                    }
                    ++pending_;
                    std::string ns = stmt(n);
                    line("{ uint32_t ld = " + load_value(i) + ";");
                    if (!ns.empty()) line("  " + ns);
                    if (n.dest_gpr() != i.rt) line("  " + R(i.rt) + " = ld; }");
                    else line("}");
                    k += 2;
                    expect = pc + 8;
                    continue;
                }
                if (n.has_delay_slot() && reads(n).count(i.rt))
                    warn(fmt("load-delay hazard into branch at %08X (not modelled)", pc));
            }
            std::string s = stmt(i);
            if (!s.empty()) line(s);
            ++k;
            expect = pc + 4;
            continue;
        }

        // Control transfer with delay slot.
        Instruction ds = at(pc + 4);
        std::string dss = stmt(ds);
        pending_ += 2;
        flush_cycles();
        uint32_t link = pc + 8;
        switch (i.op) {
            case Op::J:
                if (!dss.empty()) line(dss);
                line(jump_to(pc, i.target));
                terminal = true;
                break;
            case Op::JAL:
                line(R(31) + fmt(" = 0x%08Xu;", link));
                if (!dss.empty()) line(dss);
                line(call_site(call_expr(i.target), link));
                break;
            case Op::JR:
                if (i.rs == 31) {
                    // The return target is read from $ra when a save state resumes into the caller, so the delay
                    // slot must leave $ra alone (the native return does not look at it).
                    if (ds.dest_gpr() == 31)
                        throw std::runtime_error(fmt("%s: jr $ra at %08X has a delay slot that writes $ra",
                                                     seg_name_.c_str(), pc));
                    if (!dss.empty()) line(dss);
                    line("return;");
                } else {
                    const JumpTable* jt = nullptr;
                    for (const auto& t : f_.jump_tables)
                        if (t.jr_addr == pc) jt = &t;
                    const PointerJump* cj = nullptr;  // jump to a fixed address: no run-time lookup needed
                    for (const auto& pj : f_.pointer_jumps)
                        if (pj.jr_addr == pc && !pj.via_slot) cj = &pj;
                    if (cj) {
                        if (!dss.empty()) line(dss);
                        line(jump_to(pc, cj->addr));
                        terminal = true;
                        break;
                    }
                    line("{ uint32_t t = " + R(i.rs) + ";");
                    if (!dss.empty()) line("  " + dss);
                    if (jt) {
                        line("  switch (t) {");
                        std::set<uint32_t> seen;
                        for (uint32_t tg : jt->targets)
                            if (seen.insert(tg).second) line(fmt("  case 0x%08Xu: ", tg) + jump_to(pc, tg));
                        line(fmt("  default: dw3_bad_jump(ctx, 0x%08Xu, t); return;", pc));
                        line("  } }");
                    } else {
                        line("  dw3_call(ctx, t); return; }");
                    }
                }
                terminal = true;
                break;
            case Op::JALR:
                line("{ uint32_t t = " + R(i.rs) + ";");
                if (i.rd) line("  " + R(i.rd) + fmt(" = 0x%08Xu;", link));
                if (!dss.empty()) line("  " + dss);
                line("  " + call_site("dw3_call(ctx, t);", link) + " }");
                break;
            case Op::BLTZAL: case Op::BGEZAL: {
                bool always = i.op == Op::BGEZAL && i.rs == 0;
                if (always) {
                    line(R(31) + fmt(" = 0x%08Xu;", link));
                    if (!dss.empty()) line(dss);
                    line(call_site(call_expr(i.target), link));
                } else {
                    line("{ int c = " + branch_cond(i) + ";");
                    line("  " + R(31) + fmt(" = 0x%08Xu;", link));
                    if (!dss.empty()) line("  " + dss);
                    line("  if (c) { " + call_site(call_expr(i.target), link) + " } }");
                }
                break;
            }
            default: {  // conditional / unconditional branches
                bool always = (i.op == Op::BEQ && i.rs == i.rt) || (i.op == Op::BGEZ && i.rs == 0) ||
                              (i.op == Op::BLEZ && i.rs == 0);
                if (always) {
                    if (!dss.empty()) line(dss);
                    line(jump_to(pc, i.target));
                    terminal = true;
                } else {
                    line("{ int c = " + branch_cond(i) + ";");
                    if (!dss.empty()) line("  " + dss);
                    line("  if (c) { " + jump_to(pc, i.target) + " } }");
                }
                break;
            }
        }
        if (ds.has_delay_slot()) warn(fmt("branch in delay slot at %08X", pc + 4));
        k += 2;  // branch + delay slot
        expect = link;
        if (labels_.count(pc + 4)) {
            // Delay slot is also a branch target: emit it standalone, reached only via its label.
            if (!terminal) line("goto " + label(link) + ";");
            out_ += label(pc + 4) + ":;\n";
            ++pending_;
            if (!dss.empty()) line(dss);
            terminal = false;
        }
    }
    if (!terminal) {
        flush_cycles();
        line(call_expr(expect) + " return;");
    }
    // Return points outside this function's own code continue by calling on, like a fallthrough out of it.
    for (uint32_t t : tail_links_) {
        out_ += label(t) + ":;\n";
        line(call_expr(t) + " return;");
    }
    if (resume_out_) {
        resume_out_->assign(resume_.begin(), resume_.end());
        resume_out_->insert(resume_out_->end(), tail_links_.begin(), tail_links_.end());
    }
    if (resume_.empty() && tail_links_.empty()) return out_ + "}\n";
    // Entry dispatch used when a save state is restored (ctx->resume names the label to continue at).
    std::string dispatch = "    if (DW3_UNLIKELY(ctx->resume)) {\n        uint32_t rp = ctx->resume;\n        ctx->resume = 0;\n        switch (rp) {\n";
    for (uint32_t t : resume_) dispatch += fmt("        case 0x%08Xu: goto ", t) + label(t) + ";\n";
    for (uint32_t t : tail_links_) dispatch += fmt("        case 0x%08Xu: goto ", t) + label(t) + ";\n";
    dispatch += fmt("        default: ctx->resume = rp; dw3_bad_resume(ctx, 0x%08Xu); return;\n        }\n    }\n", f_.start);
    out_.insert(out_.find('\n') + 1, dispatch);
    return out_ + "}\n";
}

}  // namespace

std::string func_name(const std::string& seg, uint32_t addr) { return "f_" + seg + "_" + hex8(addr); }

std::string emit_function(const EmitContext& ec, const Function& f, std::vector<std::string>* warnings,
                          std::vector<uint32_t>* resume_points) {
    return FuncEmitter(ec, f, warnings, resume_points).run();
}

std::vector<EmittedFile> emit_segment(const EmitContext& ec, std::size_t funcs_per_file,
                                      std::vector<std::string>* warnings) {
    std::vector<EmittedFile> files;
    std::vector<const Function*> all;
    std::vector<std::pair<uint32_t, uint32_t>> resume;  // (label, start of the function that has it)
    for (const auto& [s, f] : *ec.local) all.push_back(&f);
    if (funcs_per_file == 0) funcs_per_file = all.size() ? all.size() : 1;
    for (std::size_t first = 0; first < all.size(); first += funcs_per_file) {
        std::size_t last = std::min(all.size(), first + funcs_per_file);
        std::string body;
        for (std::size_t k = first; k < last; ++k) {
            std::vector<uint32_t> points;
            body += "\n" + emit_function(ec, *all[k], warnings, &points);
            for (uint32_t p : points) resume.emplace_back(p, all[k]->start);
        }
        // Prototypes for every function referenced in this file.
        std::set<std::string> protos;
        for (std::size_t pos = body.find("f_"); pos != std::string::npos; pos = body.find("f_", pos + 2)) {
            if (pos > 0 && (std::isalnum(static_cast<unsigned char>(body[pos - 1])) || body[pos - 1] == '_')) continue;
            std::size_t end = body.find('(', pos);
            if (end == std::string::npos) break;
            protos.insert(body.substr(pos, end - pos));
        }
        std::string text = "/* Generated by psxrecomp from " + ec.seg->name + ". Do not edit. */\n";
        text += "#include \"dw3/recomp.h\"\n\n";
        for (const auto& p : protos) text += "void " + p + "(RecompContext* ctx);\n";
        text += body;
        files.push_back({ec.seg->name + "_" + std::to_string(files.size()) + ".c", std::move(text)});
    }
    // Where a save state can resume in this segment. A label several functions share (they overlap) goes to the
    // first one; the code after it is the same in all of them.
    std::stable_sort(resume.begin(), resume.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    resume.erase(std::unique(resume.begin(), resume.end(),
                             [](const auto& a, const auto& b) { return a.first == b.first; }),
                 resume.end());
    std::string rtext = "/* Generated by psxrecomp from " + ec.seg->name + ". Do not edit. */\n#include \"dw3/recomp.h\"\n\n";
    for (const auto& [label, start] : resume) {
        (void)label;
        rtext += "void " + func_name(ec.seg->name, start) + "(RecompContext* ctx);\n";
    }
    rtext += "\nconst RecompResumeEntry resume_" + ec.seg->name + "[] = {\n";
    for (const auto& [label, start] : resume) rtext += fmt("    {0x%08Xu, ", label) + func_name(ec.seg->name, start) + "},\n";
    rtext += "    {0, 0},  /* end */\n};\n";
    files.push_back({ec.seg->name + "_resume.c", std::move(rtext)});
    return files;
}

std::vector<std::pair<uint32_t, uint32_t>> select_probes(const CodeSegment& seg,
                                                         const std::map<uint32_t, Function>& funcs,
                                                         std::size_t max_funcs) {
    // First two instruction words of up to `max_funcs` functions spread evenly across the segment.
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (funcs.empty()) return out;
    std::vector<const Function*> all;
    for (const auto& [a, f] : funcs) all.push_back(&f);
    std::size_t step = std::max<std::size_t>(1, all.size() / max_funcs);
    for (std::size_t k = 0; k < all.size() && out.size() < max_funcs * 2; k += step) {
        for (uint32_t a : {all[k]->start, all[k]->start + 4})
            if (a < all[k]->end) out.emplace_back(a, seg.word(a));
    }
    return out;
}

uint32_t segment_checksum(const CodeSegment& seg) {
    uint32_t h = 2166136261u;
    for (uint8_t b : seg.bytes) h = (h ^ b) * 16777619u;
    return h;
}

namespace {

struct CodeRange {
    uint32_t start, end;
};

// Function bodies of a segment, merged into sorted contiguous ranges.
std::vector<CodeRange> code_ranges(const TableSegment& ts) {
    std::vector<CodeRange> out;
    for (const auto& [a, f] : *ts.funcs) {
        uint32_t lo = std::max(f.start, ts.seg->base), hi = std::min(f.end, ts.seg->end());
        if (lo >= hi) continue;
        if (!out.empty() && lo <= out.back().end) out.back().end = std::max(out.back().end, hi);
        else out.push_back({lo, hi});
    }
    return out;
}

uint32_t code_hash(const CodeSegment& seg, const std::vector<CodeRange>& ranges) {
    uint32_t h = 2166136261u;
    for (const auto& r : ranges)
        for (uint32_t a = r.start; a < r.end; ++a) h = (h ^ seg.bytes[a - seg.base]) * 16777619u;
    return h;
}

// Code words that tell `segs[idx]` apart from every other overlay sharing its address range, chosen greedily
// so that the fewest words separate the most rivals.
std::vector<std::pair<uint32_t, uint32_t>> discriminators(const std::vector<TableSegment>& segs, std::size_t idx,
                                                          std::size_t max_words) {
    const TableSegment& me = segs[idx];
    std::vector<std::size_t> rivals;
    for (std::size_t j = 1; j < segs.size(); ++j) {
        if (j == idx) continue;
        if (segs[j].seg->base < me.seg->end() && me.seg->base < segs[j].seg->end()) rivals.push_back(j);
    }
    std::map<uint32_t, std::vector<std::size_t>> separates;  // code address -> rivals it separates from `me`
    for (const auto& r : code_ranges(me)) {
        for (uint32_t a = r.start; a + 4 <= r.end; a += 4) {
            for (std::size_t k = 0; k < rivals.size(); ++k) {
                const CodeSegment& o = *segs[rivals[k]].seg;
                if (a + 4 <= o.end() && a >= o.base && o.word(a) != me.seg->word(a)) separates[a].push_back(k);
            }
        }
    }
    std::vector<bool> done(rivals.size(), false);
    std::vector<std::pair<uint32_t, uint32_t>> out;
    while (out.size() < max_words) {
        uint32_t best = 0;
        std::size_t best_n = 0;
        for (const auto& [a, ks] : separates) {
            std::size_t n = 0;
            for (std::size_t k : ks) n += !done[k];
            if (n > best_n) best_n = n, best = a;
        }
        if (best_n == 0) break;
        for (std::size_t k : separates[best]) done[k] = true;
        out.emplace_back(best, me.seg->word(best));
    }
    return out;
}

}  // namespace

SlotRelations analyze_slots(const std::vector<TableSegment>& segs) {
    SlotRelations rel;
    rel.stale_if.resize(segs.size());
    std::vector<std::vector<CodeRange>> ranges(segs.size());
    for (std::size_t k = 1; k < segs.size(); ++k) ranges[k] = code_ranges(segs[k]);
    auto overlap = [&](std::size_t a, std::size_t b) {
        return segs[a].seg->base < segs[b].seg->end() && segs[b].seg->base < segs[a].seg->end();
    };
    // True if a code word of `a` that lies inside `b`'s image differs from `b`'s word there. While `a` is
    // resident, RAM at that word holds `a`'s code, so `b`'s image is not what the slot contains.
    auto code_differs = [&](std::size_t a, std::size_t b) {
        const CodeSegment &sa = *segs[a].seg, &sb = *segs[b].seg;
        for (const auto& r : ranges[a])
            for (uint32_t w = r.start; w + 4 <= r.end; w += 4)
                if (w + 4 <= sb.end() && sb.contains(w) && sa.word(w) != sb.word(w)) return true;
        return false;
    };
    auto entries_in = [&](std::size_t sub, std::size_t super) {
        for (const auto& [a, f] : *segs[sub].funcs)
            if (!segs[super].funcs->count(a)) return false;
        return true;
    };
    for (std::size_t i = 1; i < segs.size(); ++i) {
        if (ranges[i].empty()) continue;  // no code: never has to be recognised
        for (std::size_t j = 1; j < segs.size(); ++j) {
            if (i == j || ranges[j].empty() || !overlap(i, j) || code_differs(i, j)) continue;
            // No code word of i tells it from j. Either j's code tells j from i, so j in RAM proves that i is
            // only the leftover tail of an earlier, larger load, or the two share their code where they overlap.
            if (code_differs(j, i)) {
                rel.stale_if[i].push_back(static_cast<uint32_t>(j));
                continue;
            }
            // Shared code: the runtime picks the larger image, so the smaller one must not have functions the
            // larger lacks. Each pair is judged once, from the side that loses.
            std::size_t si = segs[i].seg->bytes.size(), sj = segs[j].seg->bytes.size();
            bool i_wins = si > sj || (si == sj && i < j);
            if (!i_wins && !entries_in(i, j))
                rel.ambiguous.push_back(fmt("%s and %s share their code but %s has functions the larger image lacks",
                                            segs[i].seg->name.c_str(), segs[j].seg->name.c_str(),
                                            segs[i].seg->name.c_str()));
        }
    }
    return rel;
}

EmittedFile emit_tables(const std::vector<TableSegment>& segs, std::vector<std::string>* warnings) {
    std::string protos, arrays, table;
    SlotRelations rel = analyze_slots(segs);
    if (warnings)
        for (const auto& a : rel.ambiguous) warnings->push_back(a);
    for (std::size_t si = 0; si < segs.size(); ++si) {
        const auto& ts = segs[si];
        const std::string& n = ts.seg->name;
        arrays += "static const RecompFuncEntry funcs_" + n + "[] = {\n";
        for (const auto& [a, f] : *ts.funcs) {
            protos += "void " + func_name(n, a) + "(RecompContext* ctx);\n";
            arrays += fmt("    {0x%08Xu, ", a) + func_name(n, a) + "},\n";
        }
        if (ts.funcs->empty()) arrays += "    {0, 0},\n";
        arrays += "};\n";
        auto probes = select_probes(*ts.seg, *ts.funcs);
        if (si > 0) {  // overlays: add words that separate this overlay from the others in its slot
            for (const auto& p : discriminators(segs, si, 48))
                if (std::none_of(probes.begin(), probes.end(), [&](const auto& q) { return q.first == p.first; }))
                    probes.push_back(p);
            std::sort(probes.begin(), probes.end());
        }
        arrays += "static const RecompProbe probes_" + n + "[] = {\n";
        for (const auto& [a, w] : probes) arrays += fmt("    {0x%08Xu, 0x%08Xu},\n", a, w);
        if (probes.empty()) arrays += "    {0, 0},\n";
        arrays += "};\n";
        auto ranges = code_ranges(ts);
        arrays += "static const RecompRange ranges_" + n + "[] = {\n";
        for (const auto& r : ranges) arrays += fmt("    {0x%08Xu, 0x%08Xu},\n", r.start, r.end);
        if (ranges.empty()) arrays += "    {0, 0},\n";
        arrays += "};\n";
        protos += "extern const RecompResumeEntry resume_" + n + "[];\n";
        arrays += "static const uint16_t stale_" + n + "[] = {";
        for (uint32_t j : rel.stale_if[si]) arrays += fmt("%uu, ", j);
        if (rel.stale_if[si].empty()) arrays += "0";
        arrays += "};\n";
        table += fmt("    {\"%s\", 0x%08Xu, %zuu, 0x%08Xu, funcs_%s, %zuu, probes_%s, %zuu, ranges_%s, %zuu, 0x%08Xu, "
                     "stale_%s, %zuu, resume_%s},\n",
                     n.c_str(), ts.seg->base, ts.extent ? static_cast<std::size_t>(ts.extent) : ts.seg->bytes.size(),
                     segment_checksum(*ts.seg), n.c_str(), ts.funcs->size(), n.c_str(), probes.size(), n.c_str(),
                     ranges.size(), code_hash(*ts.seg, ranges), n.c_str(), rel.stale_if[si].size(), n.c_str());
    }
    std::string text = "/* Generated by psxrecomp. Do not edit. */\n#include \"dw3/recomp.h\"\n\n" + protos + "\n" +
                       arrays + "\nconst RecompSegment dw3_segments[] = {\n" + table + "};\n" +
                       fmt("const uint32_t dw3_segment_count = %zuu;\n", segs.size());
    return {"dw3_tables.c", std::move(text)};
}

}  // namespace psxrecomp
