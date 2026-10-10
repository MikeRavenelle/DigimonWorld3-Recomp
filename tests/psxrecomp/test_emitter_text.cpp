// Emitter output checks that do not execute the generated code (see test_emitter.cpp for that).

#include <gtest/gtest.h>

#include "mips_asm.h"
#include "psxrecomp/analysis.h"
#include "psxrecomp/emitter.h"

using namespace psxrecomp;
using namespace masm;

namespace {

constexpr uint32_t kBase = 0x80010000;

// Analyses `p` with a single entry at its base and returns the emitted entry function.
std::string emit_entry(const Program& p, std::vector<std::string>* warnings = nullptr, bool cycles = true) {
    static CodeSegment seg;  // EmitContext holds pointers; keep them alive for the call only
    seg = p.segment("TXT");
    AnalysisConfig cfg;
    cfg.entries = {p.base()};
    cfg.scan_data_pointers = false;
    cfg.scan_jal_words = false;
    cfg.gap_fill = false;
    AnalysisResult r = analyze(seg, cfg);
    EmitContext ec;
    ec.seg = &seg;
    ec.local = &r.functions;
    ec.cycle_counting = cycles;
    return emit_function(ec, r.functions.at(p.base()), warnings);
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

TEST(EmitterText, NopEmitsNothing) {
    Program p(kBase);
    p.emit({nop(), nop(), addiu(v0, zero, 1), nop(), jr(ra), nop()});
    std::string c = emit_entry(p, nullptr, false);
    EXPECT_EQ(c, "void f_TXT_80010000(RecompContext* ctx) {\n    ctx->r[2] = 0x00000001u;\n    return;\n}\n");
}

TEST(EmitterText, WritesToZeroEmitNothing) {
    Program p(kBase);
    p.emit({addu(zero, t0, t1), addiu(zero, zero, 5), lui(zero, 0x1234), sll(zero, t0, 2), mflo(zero), slt(zero, t0, t1),
            jr(ra), nop()});
    std::string c = emit_entry(p, nullptr, false);
    EXPECT_FALSE(contains(c, "ctx->r[0]"));
    EXPECT_FALSE(contains(c, "0x12340000"));
}

TEST(EmitterText, LoadIntoZeroKeepsTheAccess) {
    Program p(kBase);
    p.emit({lw(zero, 0, a0), jr(ra), nop()});
    std::string c = emit_entry(p, nullptr, false);
    EXPECT_TRUE(contains(c, "(void)dw3_read32(ctx, ctx->r[4])"));
    EXPECT_FALSE(contains(c, "ctx->r[0]"));
}

TEST(EmitterText, LabelsOnlyWhereNeeded) {
    Program straight(kBase);
    straight.emit({addiu(v0, zero, 1), addiu(v1, zero, 2), jr(ra), nop()});
    EXPECT_FALSE(contains(emit_entry(straight), "L_"));

    Program branchy(kBase);
    branchy.emit({beqz(a0, 2), nop(), addiu(v0, zero, 1), addiu(v1, zero, 2), jr(ra), nop()});
    std::string c = emit_entry(branchy);
    EXPECT_TRUE(contains(c, "goto L_8001000C;"));
    EXPECT_TRUE(contains(c, "L_8001000C:;"));
    EXPECT_FALSE(contains(c, "L_80010008"));  // fallthrough instruction is not a target
}

TEST(EmitterText, BackwardJumpChecksEvents) {
    Program p(kBase);
    p.emit({addiu(t0, t0, -1), bnez(t0, -2), nop(), jr(ra), nop()});
    EXPECT_TRUE(contains(emit_entry(p, nullptr, true), "DW3_CHECK_EVENTS(ctx); goto L_80010000;"));
    std::string off = emit_entry(p, nullptr, false);
    EXPECT_FALSE(contains(off, "DW3_CHECK_EVENTS"));
    EXPECT_FALSE(contains(off, "ctx->cycles"));
}

TEST(EmitterText, CycleCountingFlushesAtBlockEnds) {
    Program p(kBase);
    p.emit({addiu(v0, zero, 1), addiu(v1, zero, 2), jr(ra), nop()});
    // 2 ALU + jr + delay slot = 4 cycles, flushed before the return.
    EXPECT_TRUE(contains(emit_entry(p), "ctx->cycles += 4;"));
}

TEST(EmitterText, JalWritesLinkBeforeDelaySlotAndCalls) {
    Program p(kBase);
    p.emit({jal(kBase + 0x40), move(s0, ra), jr(ra), nop()});
    std::string c = emit_entry(p, nullptr, false);
    size_t link = c.find("ctx->r[31] = 0x80010008u;");
    size_t ds = c.find("ctx->r[16] = ctx->r[31] + ctx->r[0]") == std::string::npos ? c.find("ctx->r[16]") : 0;
    size_t call = c.find("dw3_call(ctx, 0x80010040u);");
    EXPECT_NE(link, std::string::npos);
    EXPECT_NE(call, std::string::npos);
    EXPECT_LT(link, ds);
    EXPECT_LT(ds, call);
}

TEST(EmitterText, LoadDelayHazardIsReportedAndModelled) {
    Program p(kBase);
    p.emit({lw(t0, 0, a0), addu(t1, t0, t0), jr(ra), nop()});
    std::vector<std::string> warnings;
    std::string c = emit_entry(p, &warnings, false);
    EXPECT_TRUE(contains(c, "uint32_t ld = dw3_read32"));
    EXPECT_TRUE(contains(c, "ctx->r[8] = ld;"));
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_TRUE(contains(warnings[0], "load-delay hazard at 80010000"));
}

TEST(EmitterText, NoHazardWhenNextInstructionDoesNotReadTheRegister) {
    Program p(kBase);
    p.emit({lw(t0, 0, a0), addu(t1, t2, t3), addu(t4, t0, t0), jr(ra), nop()});
    std::vector<std::string> warnings;
    std::string c = emit_entry(p, &warnings, false);
    EXPECT_FALSE(contains(c, "uint32_t ld"));
    EXPECT_TRUE(warnings.empty());
}

TEST(EmitterText, LwlLwrForwardingIsNotAHazard) {
    Program p(kBase);
    p.emit({lwl(t0, 3, a0), lwr(t0, 0, a0), nop(), jr(ra), nop()});
    std::vector<std::string> warnings;
    emit_entry(p, &warnings, false);
    EXPECT_TRUE(warnings.empty());
}

TEST(EmitterText, BiosStubCallsBiosDispatch) {
    // li t2,0xA0 ; jr t2 ; li t1,0x3C
    Program p(kBase);
    p.emit({li(t2, 0xA0), jr(t2), li(t1, 0x3C)});
    std::string c = emit_entry(p);
    EXPECT_TRUE(contains(c, "dw3_bios_call(ctx, 0xA0u, 0x3Cu);"));
    EXPECT_TRUE(contains(c, "ctx->r[10] = 0xA0u;"));
    EXPECT_TRUE(contains(c, "ctx->r[9] = 0x3Cu;"));
    EXPECT_FALSE(contains(c, "return;"));  // falls off the end of the C function: the stub body is the call
}

TEST(EmitterText, ExternalCallGoesThroughLookup) {
    Program p(kBase);
    p.emit({jal(0x80200000), nop(), jr(ra), nop()});
    EXPECT_TRUE(contains(emit_entry(p, nullptr, false), "dw3_call(ctx, 0x80200000u);"));
}

TEST(EmitterText, IndirectJumpAndCallUseLookup) {
    Program p(kBase);
    p.emit({jalr(t9), nop(), jr(t0), nop()});
    std::string c = emit_entry(p, nullptr, false);
    EXPECT_TRUE(contains(c, "dw3_call(ctx, t); }"));
    EXPECT_TRUE(contains(c, "dw3_call(ctx, t); return; }"));
}

TEST(EmitterText, ChainedLoadsKeepBothDelaySlots) {
    // lw t0 ; lw t1,0(t0) ; addu t2,t1,t1 : the addu must see the OLD t1, and the second load the OLD t0.
    Program p(kBase);
    p.emit({lw(t0, 0, a0), lw(t1, 0, t0), addu(t2, t1, t1), jr(ra), nop()});
    std::string c = emit_entry(p, nullptr, false);
    EXPECT_TRUE(contains(c, "uint32_t ld2 = "));
    EXPECT_LT(c.find("ctx->r[10] = ctx->r[9] + ctx->r[9];"), c.find("ctx->r[9] = ld2;"));
    EXPECT_LT(c.find("uint32_t ld2 = dw3_read32(ctx, ctx->r[8])"), c.find("ctx->r[8] = ld;"));
}
