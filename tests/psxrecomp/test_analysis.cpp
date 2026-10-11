#include <gtest/gtest.h>

#include <algorithm>

#include "mips_asm.h"
#include "psxrecomp/analysis.h"

using namespace psxrecomp;
using namespace masm;

namespace {

constexpr uint32_t kBase = 0x80010000;

AnalysisConfig entry_only(uint32_t entry) {
    AnalysisConfig c;
    c.entries = {entry};
    return c;
}

const Function& get(const AnalysisResult& r, uint32_t addr) {
    auto it = r.functions.find(addr);
    if (it == r.functions.end()) throw std::runtime_error("missing function");
    return it->second;
}

}  // namespace

TEST(Analysis, CallerAndCallee) {
    Program p(kBase);
    uint32_t callee = kBase + 0x20;
    p.emit({addiu(sp, sp, -24), sw(ra, 16, sp), jal(callee), nop(), lw(ra, 16, sp), jr(ra), addiu(sp, sp, 24), nop()});
    p.emit({jr(ra), li(v0, 1)});  // callee at +0x20
    auto r = analyze(p.segment(), entry_only(kBase));
    ASSERT_EQ(r.functions.size(), 2u);
    EXPECT_TRUE(get(r, kBase).callees.count(callee));
    EXPECT_EQ(get(r, kBase).end, kBase + 0x1C);
    EXPECT_EQ(get(r, callee).end, callee + 8);
    EXPECT_TRUE(get(r, callee).source & kSourceJal);
}

TEST(Analysis, BranchesStayInsideFunction) {
    Program p(kBase);
    // loop: v0 += 1 until v0 == a0
    p.emit({move(v0, zero), addiu(v0, v0, 1), bne(v0, a0, -2), nop(), jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    ASSERT_EQ(r.functions.size(), 1u);
    EXPECT_EQ(get(r, kBase).end, kBase + 24);
    EXPECT_TRUE(get(r, kBase).errors.empty());
}

TEST(Analysis, ForwardBranchSkipsDeadCode) {
    Program p(kBase);
    p.emit({beqz(a0, 2), nop(), li(v0, 1), li(v0, 2), jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_EQ(get(r, kBase).end, kBase + 24);
}

TEST(Analysis, BranchIntoDelaySlotContinuesPastIt) {
    Program p(kBase);
    // The bltz lands on the delay slot of the unconditional branch; from there execution runs on into
    // the word after the slot, which no other path reaches.
    p.emit({bltz(a0, 3), nop(), li(v0, 1), b(3), li(v1, 2), li(v0, 7), nop(), jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    const Function& f = get(r, kBase);
    EXPECT_TRUE(f.errors.empty());
    EXPECT_TRUE(f.tail_calls.empty());
    EXPECT_TRUE(std::find(f.insns.begin(), f.insns.end(), kBase + 20) != f.insns.end());
    EXPECT_EQ(f.end, kBase + 36);
}

TEST(Analysis, UnconditionalBranchEndsPath) {
    Program p(kBase);
    // b +2 skips over two words of garbage that must not be decoded.
    p.emit({b(3), nop(), 0xFC000000u, 0xFC000000u, jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_TRUE(get(r, kBase).errors.empty());
}

TEST(Analysis, TailCallToKnownFunction) {
    Program p(kBase);
    uint32_t other = kBase + 0x10;
    p.emit({li(a0, 5), j(other), nop(), nop()});
    p.emit({addiu(sp, sp, -8), jr(ra), addiu(sp, sp, 8)});
    AnalysisConfig c = entry_only(kBase);
    c.extra_functions = {other};
    auto r = analyze(p.segment(), c);
    EXPECT_TRUE(get(r, kBase).tail_calls.count(other));
    EXPECT_EQ(get(r, kBase).end, kBase + 12);
}

TEST(Analysis, JumpBackwardBeforeStartIsTailCall) {
    Program p(kBase);
    p.emit({jr(ra), nop()});  // target at kBase
    uint32_t f2 = p.pc();
    p.emit({j(kBase), nop()});
    AnalysisConfig c;
    c.entries = {f2};
    auto r = analyze(p.segment(), c);
    EXPECT_TRUE(get(r, f2).tail_calls.count(kBase));
    EXPECT_TRUE(r.functions.count(kBase));
}

TEST(Analysis, JumpTableWithBound) {
    Program p(kBase);
    uint32_t table = kBase + 0x100;
    // switch (a0) over 3 cases
    p.emit({sltiu(v0, a0, 3), beqz(v0, 11), sll(v0, a0, 2), lui(at, table >> 16), addu(at, at, v0),
            lw(v0, static_cast<int16_t>(table & 0xFFFF), at), nop(), jr(v0), nop()});
    uint32_t c0 = p.pc();
    p.emit({b(5), li(v0, 10)});
    uint32_t c1 = p.pc();
    p.emit({b(3), li(v0, 11)});
    uint32_t c2 = p.pc();
    p.emit({li(v0, 12), nop()});
    p.emit({jr(ra), nop()});
    while (p.pc() < table) p.emit(nop());
    p.emit({c0, c1, c2, 0xDEADBEEFu});
    auto r = analyze(p.segment(), entry_only(kBase));
    const auto& f = get(r, kBase);
    ASSERT_EQ(f.jump_tables.size(), 1u);
    EXPECT_EQ(f.jump_tables[0].table_addr, table);
    EXPECT_EQ(f.jump_tables[0].targets, (std::vector<uint32_t>{c0, c1, c2}));
    EXPECT_TRUE(f.indirect_jumps.empty());
    EXPECT_TRUE(f.errors.empty());
}

TEST(Analysis, JumpTableViaAddiuBase) {
    Program p(kBase);
    uint32_t table = kBase + 0x80;
    p.emit({sltiu(v0, a0, 2), beqz(v0, 10), lui(v1, table >> 16), addiu(v1, v1, static_cast<int16_t>(table & 0xFFFF)),
            sll(v0, a0, 2), addu(v0, v0, v1), lw(v0, 0, v0), nop(), jr(v0), nop()});
    uint32_t c0 = p.pc();
    p.emit({jr(ra), li(v0, 1)});
    uint32_t c1 = p.pc();
    p.emit({jr(ra), li(v0, 2)});
    while (p.pc() < table) p.emit(nop());
    p.emit({c0, c1});
    auto r = analyze(p.segment(), entry_only(kBase));
    const auto& f = get(r, kBase);
    ASSERT_EQ(f.jump_tables.size(), 1u);
    EXPECT_EQ(f.jump_tables[0].targets.size(), 2u);
}

TEST(Analysis, UnboundedTableCutAtNextFunction) {
    Program p(kBase);
    uint32_t table = kBase + 0x100;
    // No sltiu bound before the table load.
    p.emit({sll(v0, a0, 2), lui(at, table >> 16), addu(at, at, v0), lw(v0, static_cast<int16_t>(table & 0xFFFF), at),
            nop(), jr(v0), nop()});
    uint32_t c0 = p.pc();
    p.emit({jr(ra), li(v0, 1)});
    uint32_t next_fn = p.pc();
    p.emit({addiu(sp, sp, -8), jr(ra), addiu(sp, sp, 8)});
    while (p.pc() < table) p.emit(nop());
    p.emit({c0, next_fn, c0});  // second entry belongs to an adjacent table
    AnalysisConfig c = entry_only(kBase);
    c.extra_functions = {next_fn};
    auto r = analyze(p.segment(), c);
    const auto& f = get(r, kBase);
    ASSERT_EQ(f.jump_tables.size(), 1u);
    EXPECT_EQ(f.jump_tables[0].targets, std::vector<uint32_t>{c0});
}

TEST(Analysis, UnresolvedJrIsRecorded) {
    Program p(kBase);
    p.emit({lw(t1, 0, a0), nop(), jr(t1), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_EQ(get(r, kBase).indirect_jumps, std::vector<uint32_t>{kBase + 8});
}

TEST(Analysis, JumpTableWithBaseSpilledToTheStack) {
    Program p(kBase);
    uint32_t table = kBase + 0x100;
    // The entry address is computed early, stored in a stack slot and reloaded just before the load.
    p.emit({addiu(sp, sp, -32), lui(v0, table >> 16), addiu(v0, v0, static_cast<int16_t>(table & 0xFFFF)),
            sll(v1, a0, 2), addu(v1, v1, v0), sw(v1, 24, sp), sltiu(v0, a0, 2), beqz(v0, 8), nop(),
            lw(t0, 24, sp), nop(), lw(v0, 0, t0), nop(), jr(v0), nop()});
    uint32_t c0 = p.pc();
    p.emit({jr(ra), li(v0, 1)});
    uint32_t c1 = p.pc();
    p.emit({jr(ra), li(v0, 2)});
    while (p.pc() < table) p.emit(nop());
    p.emit({c0, c1});
    auto r = analyze(p.segment(), entry_only(kBase));
    const auto& f = get(r, kBase);
    ASSERT_EQ(f.jump_tables.size(), 1u);
    EXPECT_EQ(f.jump_tables[0].table_addr, table);
    EXPECT_EQ(f.jump_tables[0].targets, (std::vector<uint32_t>{c0, c1}));
    EXPECT_TRUE(f.indirect_jumps.empty());
}

TEST(Analysis, JrThroughFixedMemoryCellIsAPointerJump) {
    Program p(kBase);
    uint32_t cell = 0x80082000;
    p.emit({lui(t1, cell >> 16), lw(t1, static_cast<int16_t>(cell & 0xFFFF), t1), nop(), jr(t1), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    const auto& f = get(r, kBase);
    ASSERT_EQ(f.pointer_jumps.size(), 1u);
    EXPECT_EQ(f.pointer_jumps[0].jr_addr, kBase + 12);
    EXPECT_EQ(f.pointer_jumps[0].addr, cell);
    EXPECT_TRUE(f.pointer_jumps[0].via_slot);
    EXPECT_TRUE(f.indirect_jumps.empty());
}

TEST(Analysis, JrToConstantOutsideTheSegmentIsAnExternalTailCall) {
    Program p(kBase);
    p.emit({lui(v0, 0xA001), addiu(v0, v0, -8276), jr(v0), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    const auto& f = get(r, kBase);
    ASSERT_EQ(f.pointer_jumps.size(), 1u);
    EXPECT_FALSE(f.pointer_jumps[0].via_slot);
    EXPECT_EQ(f.pointer_jumps[0].addr, 0xA000DFACu);
    EXPECT_TRUE(f.indirect_jumps.empty());
    EXPECT_TRUE(r.external_calls.count(0xA000DFACu));
}

TEST(Analysis, JrToConstantInsideTheFunctionIsALocalJump) {
    Program p(kBase);
    uint32_t target = kBase + 0x20;
    p.emit({lui(v0, target >> 16), ori(v0, v0, target & 0xFFFF), jr(v0), nop()});
    while (p.pc() < target) p.emit(nop());
    p.emit({li(v0, 3), jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    const auto& f = get(r, kBase);
    ASSERT_EQ(f.pointer_jumps.size(), 1u);
    EXPECT_EQ(f.pointer_jumps[0].addr, target);
    EXPECT_TRUE(f.tail_calls.empty());
    EXPECT_TRUE(std::find(f.insns.begin(), f.insns.end(), target) != f.insns.end());
}

TEST(Analysis, JrToConstantBeforeTheFunctionIsATailCall) {
    Program p(kBase);
    p.emit({jr(ra), nop()});
    uint32_t f2 = p.pc();
    p.emit({lui(v0, kBase >> 16), ori(v0, v0, kBase & 0xFFFF), jr(v0), nop()});
    AnalysisConfig c;
    c.entries = {f2};
    auto r = analyze(p.segment(), c);
    EXPECT_TRUE(get(r, f2).tail_calls.count(kBase));
    EXPECT_TRUE(r.functions.count(kBase));
}

TEST(Analysis, JrWithStrayBitsIsNotCode) {
    Program p(kBase);
    // 0x00010708 decodes as "jr zero" with junk in rt/rd; it is data, so no function may be built from it.
    p.emit({jr(ra), nop(), 0x00010708u, 0x000901E0u, jr(ra), nop()});
    AnalysisConfig c = entry_only(kBase);
    auto r = analyze(p.segment(), c);
    EXPECT_FALSE(r.functions.count(kBase + 8));
}

TEST(Analysis, JalrRecordedAndContinues) {
    Program p(kBase);
    p.emit({addiu(sp, sp, -24), sw(ra, 16, sp), jalr(v0), nop(), lw(ra, 16, sp), jr(ra), addiu(sp, sp, 24)});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_EQ(get(r, kBase).indirect_calls, std::vector<uint32_t>{kBase + 8});
    EXPECT_EQ(get(r, kBase).end, kBase + 28);
}

TEST(Analysis, BiosStubDetected) {
    Program p(kBase);
    uint32_t stub = kBase + 0x10;
    p.emit({jal(stub), nop(), jr(ra), nop()});
    p.emit({li(t2, 0xB0), jr(t2), li(t1, 0x3D)});
    auto r = analyze(p.segment(), entry_only(kBase));
    const auto& f = get(r, stub);
    EXPECT_EQ(f.kind, FuncKind::BiosStub);
    EXPECT_EQ(f.bios_table, 0xB0);
    EXPECT_EQ(f.bios_fn, 0x3D);
    EXPECT_EQ(f.end, stub + 12);
}

TEST(Analysis, MatchBiosStubRejectsNonStub) {
    Program p(kBase);
    p.emit({li(t2, 0xD0), jr(t2), li(t1, 1)});
    uint8_t t = 0, fn = 0;
    EXPECT_FALSE(match_bios_stub(p.segment(), kBase, t, fn));
}

TEST(Analysis, DataPointerToPrologueDiscovered) {
    Program p(kBase);
    p.emit({jr(ra), nop()});
    uint32_t hidden = p.pc();
    p.emit({addiu(sp, sp, -16), jr(ra), addiu(sp, sp, 16)});
    p.emit({0x11111111u, hidden});  // data: a function-pointer table
    AnalysisConfig c = entry_only(kBase);
    c.gap_fill = false;
    auto r = analyze(p.segment(), c);
    ASSERT_TRUE(r.functions.count(hidden));
    EXPECT_TRUE(get(r, hidden).source & kSourceDataPointer);
}

TEST(Analysis, DataPointerToNonPrologueIgnored) {
    Program p(kBase);
    p.emit({jr(ra), nop()});
    uint32_t label = p.pc();
    p.emit({li(v0, 1), jr(ra), nop()});
    p.emit({label});
    AnalysisConfig c = entry_only(kBase);
    c.gap_fill = false;
    auto r = analyze(p.segment(), c);
    EXPECT_FALSE(r.functions.count(label));
}

TEST(Analysis, ForwardGapFillFindsLeafAfterReturn) {
    Program p(kBase);
    p.emit({jr(ra), nop(), nop(), nop()});  // padding
    uint32_t leaf = p.pc();
    p.emit({andi(v0, a0, 0xFF), jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    ASSERT_TRUE(r.functions.count(leaf));
    EXPECT_TRUE(get(r, leaf).source & kSourceGap);
}

TEST(Analysis, GapFillStopsAtData) {
    Program p(kBase);
    p.emit({jr(ra), nop()});
    p.emit({0xFC000000u, 0xFC000001u});  // invalid opcodes = data
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_EQ(r.functions.size(), 1u);
}

TEST(Analysis, LeadingRodataThenCodeFoundByReturnAnchor) {
    Program p(kBase);
    p.emit({0x80012345u, 0xFC000000u, 0x00001234u});  // rodata at segment start
    uint32_t fn = p.pc();
    p.emit({addiu(sp, sp, -8), li(v0, 3), jr(ra), addiu(sp, sp, 8)});
    AnalysisConfig c;  // no entries at all (overlay case)
    auto r = analyze(p.segment(), c);
    ASSERT_TRUE(r.functions.count(fn));
    EXPECT_EQ(get(r, fn).end, fn + 16);
}

TEST(Analysis, MidFunctionReturnsDoNotSplitFunction) {
    Program p(kBase);
    p.emit({0xFC000000u});  // leading data so nothing is seeded at base
    uint32_t fn = p.pc();
    p.emit({beqz(a0, 3), nop(), jr(ra), li(v0, 1),   // early return
            bgez(a0, 3), nop(), jr(ra), li(v0, 2),   // another one
            jr(ra), li(v0, 3)});
    AnalysisConfig c;
    auto r = analyze(p.segment(), c);
    EXPECT_EQ(r.functions.size(), 1u);
    ASSERT_TRUE(r.functions.count(fn));
    EXPECT_EQ(get(r, fn).end, fn + 40);
    EXPECT_TRUE(r.warnings.empty());
}

TEST(Analysis, JalScanSeedsOverlayFunctions) {
    Program p(kBase);
    p.emit({0xFC000000u});
    uint32_t caller = p.pc();
    uint32_t callee = caller + 0x10;
    p.emit({jal(callee), nop(), jr(ra), nop()});
    p.emit({addiu(sp, sp, -8), jr(ra), addiu(sp, sp, 8)});
    AnalysisConfig c;
    c.gap_fill = false;
    auto r = analyze(p.segment(), c);
    EXPECT_TRUE(r.functions.count(callee));
}

TEST(Analysis, CodeEndMakesCallsExternal) {
    Program p(kBase);
    uint32_t beyond = kBase + 0x40;
    p.emit({jal(beyond), nop(), jr(ra), nop()});
    while (p.pc() < beyond) p.emit(nop());
    p.emit({addiu(sp, sp, -8), jr(ra), addiu(sp, sp, 8)});  // overlay-slot leftovers in the exe image
    AnalysisConfig c = entry_only(kBase);
    c.code_end = kBase + 0x20;
    auto r = analyze(p.segment(), c);
    EXPECT_FALSE(r.functions.count(beyond));
    EXPECT_TRUE(r.external_calls.count(beyond));
}

TEST(Analysis, ExternalCallOutsideSegment) {
    Program p(kBase);
    p.emit({jal(0x80082448), nop(), jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_TRUE(r.external_calls.count(0x80082448));
}

TEST(Analysis, InvalidInstructionIsError) {
    Program p(kBase);
    p.emit({li(v0, 1), 0xFC000000u, jr(ra), nop()});
    AnalysisConfig c = entry_only(kBase);
    c.gap_fill = false;
    auto r = analyze(p.segment(), c);
    EXPECT_FALSE(get(r, kBase).errors.empty());
}

TEST(Analysis, BranchInDelaySlotIsError) {
    Program p(kBase);
    p.emit({beqz(a0, 2), b(2), nop(), nop(), jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_FALSE(get(r, kBase).errors.empty());
}

TEST(Analysis, RunOffSegmentIsError) {
    Program p(kBase);
    p.emit({li(v0, 1), li(v0, 2)});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_FALSE(get(r, kBase).errors.empty());
}

TEST(Analysis, NotFunctionsHintRejectsStart) {
    Program p(kBase);
    uint32_t callee = kBase + 0x10;
    p.emit({jal(callee), nop(), jr(ra), nop()});
    p.emit({jr(ra), nop()});
    AnalysisConfig c = entry_only(kBase);
    c.not_functions = {callee};
    auto r = analyze(p.segment(), c);
    EXPECT_FALSE(r.functions.count(callee));
}

TEST(Analysis, FallthroughIntoNextFunctionIsTailCall) {
    Program p(kBase);
    uint32_t second = kBase + 8;
    p.emit({li(a0, 1), li(a1, 2)});  // falls through (noreturn-style)
    p.emit({addiu(sp, sp, -8), jr(ra), addiu(sp, sp, 8)});
    AnalysisConfig c = entry_only(kBase);
    c.extra_functions = {second};
    auto r = analyze(p.segment(), c);
    EXPECT_TRUE(get(r, kBase).tail_calls.count(second));
    EXPECT_EQ(get(r, kBase).end, second);
}

TEST(Analysis, BalIsCall) {
    Program p(kBase);
    uint32_t target = kBase + 0x10;
    p.emit({i_type(0x01, 0, 0x11, 3), nop(), jr(ra), nop()});  // bgezal zero (bal) +3
    p.emit({jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_TRUE(get(r, kBase).callees.count(target));
    EXPECT_TRUE(r.functions.count(target));
}

TEST(Analysis, BreakEndsPath) {
    Program p(kBase);
    p.emit({jal(kBase + 0x10), nop(), break_(), 0xFC000000u});
    p.emit({jr(ra), nop()});
    auto r = analyze(p.segment(), entry_only(kBase));
    EXPECT_TRUE(get(r, kBase).errors.empty());
}

TEST(Analysis, SegmentWordOutOfRange) {
    Program p(kBase);
    p.emit(0x12345678u);
    auto s = p.segment();
    EXPECT_EQ(s.word(kBase), 0x12345678u);
    EXPECT_EQ(s.word(kBase + 4), 0u);
    EXPECT_EQ(s.word(kBase - 4), 0u);
}
