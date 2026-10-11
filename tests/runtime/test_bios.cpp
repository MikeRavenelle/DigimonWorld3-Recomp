// Native BIOS: exe loading, events, interrupt path (chains, root counters, pads, custom exit), syscalls, A0/B0/C0.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "dw3/bios.h"
#include "dw3/machine.h"

namespace {

using dw3::Bios;

// Fake "recompiled" game functions: slot N lives at kFnBase + 0x100 * N and forwards to g_impl[N].
constexpr uint32_t kFnBase = 0x80010000;
constexpr int kSlots = 8;
std::function<void(RecompContext*)> g_impl[kSlots];

template <int N>
void slot_fn(RecompContext* c) {
    if (g_impl[N]) g_impl[N](c);
}

constexpr uint32_t fn_addr(int n) { return kFnBase + 0x100u * static_cast<uint32_t>(n); }

#define SLOT(n) {fn_addr(n), slot_fn<n>}
const RecompFuncEntry kMainFuncs[] = {SLOT(0), SLOT(1), SLOT(2), SLOT(3), SLOT(4), SLOT(5), SLOT(6), SLOT(7)};
#undef SLOT

int g_ova = 0, g_ovb = 0;
void fn_ova(RecompContext*) { ++g_ova; }
void fn_ovb(RecompContext*) { ++g_ovb; }

constexpr uint32_t kSlotBase = 0x80082448;
const RecompFuncEntry kAFuncs[] = {{kSlotBase + 0x10, fn_ova}};
const RecompFuncEntry kBFuncs[] = {{kSlotBase + 0x10, fn_ovb}};
const RecompProbe kAProbes[] = {{kSlotBase + 0x10, 0x27BDFFE8}};
const RecompProbe kBProbes[] = {{kSlotBase + 0x10, 0x27BDFFD0}};
const RecompSegment kSegs[] = {
    {"MAIN", 0x80010000, 0x10000, 0, kMainFuncs, 8, nullptr, 0},
    {"OVA", kSlotBase, 0x100, 0, kAFuncs, 1, kAProbes, 1},
    {"OVB", kSlotBase, 0x100, 0, kBFuncs, 1, kBProbes, 1},
};

struct Throwing {
    Throwing() { dw3::set_fatal_handler([](const std::string& m) { throw std::runtime_error(m); }); }
    ~Throwing() { dw3::set_fatal_handler(nullptr); }
};

constexpr uint32_t kIMask = 0x1F801074;
constexpr uint32_t kSrIen = (1u << 10) | 1u;  // IM2 + IEc
constexpr uint32_t kClassRCnt = 0xF2000000, kClassSw = 0xF4000001, kClassHw = 0xF0000011;
constexpr uint32_t kData = 0x80100000;  // scratch area for nodes / buffers

// Every GPR/HI/LO distinct, so any clobbering is visible.
void fill_regs(RecompContext& c) {
    for (uint32_t i = 1; i < 32; ++i) c.r[i] = 0x1000 + i;
    c.hi = 0xAAAA0001;
    c.lo = 0xBBBB0002;
}

void trash_regs(RecompContext* c) {
    for (int i = 1; i < 32; ++i) c->r[i] = 0xDEAD0000u + static_cast<uint32_t>(i);
    c->hi = c->lo = 0xDEADBEEF;
}

class BiosTest : public ::testing::Test {
protected:
    BiosTest() {
        for (auto& f : g_impl) f = nullptr;
        g_ova = g_ovb = 0;
    }
    ~BiosTest() override {
        for (auto& f : g_impl) f = nullptr;
    }

    RecompContext& ctx() { return m.ctx(); }
    void poke(uint32_t a, uint32_t v) { dw3_write32(&ctx(), a, v); }
    uint32_t peek(uint32_t a) { return dw3_read32(&ctx(), a); }
    void impl(int n, std::function<void(RecompContext*)> f) { g_impl[n] = std::move(f); }

    // Invokes a BIOS function the way generated code does and returns v0.
    uint32_t call(uint32_t table, uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        ctx().r[4] = a0;
        ctx().r[5] = a1;
        ctx().r[6] = a2;
        ctx().r[7] = a3;
        dw3_bios_call(&ctx(), table, fn);
        return ctx().r[2];
    }
    uint32_t b0(uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        return call(0xB0, fn, a0, a1, a2, a3);
    }
    uint32_t c0(uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0) { return call(0xC0, fn, a0, a1); }

    uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func = 0) {
        return b0(0x08, cls, spec, mode, func);
    }
    uint32_t open_enabled(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func = 0) {
        uint32_t h = open_event(cls, spec, mode, func);
        b0(0x0C, h);
        return h;
    }

    void enable_irq(uint32_t mask) {
        dw3_mtc0(&ctx(), 12, kSrIen);
        poke(kIMask, mask);
    }
    // Raises the source and lets the machine deliver the interrupt to the BIOS.
    void fire(dw3::IrqSource s) {
        m.irq().raise(s);
        m.service_events();
    }

    // Enqueues a SysEnqIntRP node {next, func2, func1} at `node`.
    void enq(uint32_t prio, uint32_t node, uint32_t func1, uint32_t func2 = 0) {
        poke(node + 4, func2);
        poke(node + 8, func1);
        c0(0x02, prio, node);
    }

    Throwing throwing;
    dw3::Machine m{kSegs};
    Bios bios{m};
};

// ---- PS-X EXE image builder -------------------------------------------------------------------

struct ExeSpec {
    uint32_t pc = 0x80010000, gp = 0x80099999, addr = 0x80010000;
    uint32_t bss = 0, bss_size = 0, sp_base = 0, sp_off = 0;
    std::vector<uint8_t> text{1, 2, 3, 4, 5, 6, 7, 8};
};

std::vector<uint8_t> make_exe(const ExeSpec& s) {
    std::vector<uint8_t> f(0x800 + s.text.size(), 0);
    std::memcpy(f.data(), "PS-X EXE", 8);
    auto put = [&](std::size_t o, uint32_t v) { std::memcpy(f.data() + o, &v, 4); };
    put(0x10, s.pc);
    put(0x14, s.gp);
    put(0x18, s.addr);
    put(0x1C, static_cast<uint32_t>(s.text.size()));
    put(0x28, s.bss);
    put(0x2C, s.bss_size);
    put(0x30, s.sp_base);
    put(0x34, s.sp_off);
    std::memcpy(f.data() + 0x800, s.text.data(), s.text.size());
    return f;
}

// ---- load_exe ---------------------------------------------------------------------------------

TEST_F(BiosTest, LoadExeReturnsEntryPc) {
    ExeSpec s;
    s.pc = 0x80010234;
    EXPECT_EQ(bios.load_exe(make_exe(s)), 0x80010234u);
}

TEST_F(BiosTest, LoadExeCopiesTextToRam) {
    ExeSpec s;
    s.addr = 0x80011000;
    bios.load_exe(make_exe(s));
    for (uint32_t i = 0; i < 8; ++i) EXPECT_EQ(m.bus().ram()[0x11000 + i], i + 1);
    EXPECT_EQ(m.bus().ram()[0x11008], 0);
    EXPECT_EQ(m.bus().ram()[0x10FFF], 0);
}

TEST_F(BiosTest, LoadExeAcceptsKusegAndKseg1LoadAddress) {
    ExeSpec s;
    s.addr = 0xA0012000;
    bios.load_exe(make_exe(s));
    EXPECT_EQ(m.bus().ram()[0x12000], 1);
    s.addr = 0x00013000;
    bios.load_exe(make_exe(s));
    EXPECT_EQ(m.bus().ram()[0x13007], 8);
}

TEST_F(BiosTest, LoadExeZeroesBss) {
    std::memset(m.bus().ram() + 0x20000, 0xAA, 0x100);
    ExeSpec s;
    s.bss = 0x80020010;
    s.bss_size = 0x20;
    bios.load_exe(make_exe(s));
    EXPECT_EQ(m.bus().ram()[0x2000F], 0xAA);
    for (uint32_t i = 0; i < 0x20; ++i) EXPECT_EQ(m.bus().ram()[0x20010 + i], 0) << i;
    EXPECT_EQ(m.bus().ram()[0x20030], 0xAA);
}

TEST_F(BiosTest, LoadExeWithoutBssLeavesRamAlone) {
    std::memset(m.bus().ram() + 0x20000, 0xAA, 0x100);
    ExeSpec s;
    s.bss = 0x80020000;
    s.bss_size = 0;
    bios.load_exe(make_exe(s));
    EXPECT_EQ(m.bus().ram()[0x20000], 0xAA);
}

TEST_F(BiosTest, LoadExeBssMayOverlapText) {
    ExeSpec s;
    s.addr = 0x80010000;
    s.bss = 0x80010004;
    s.bss_size = 4;
    bios.load_exe(make_exe(s));
    EXPECT_EQ(m.bus().ram()[0x10003], 4);
    EXPECT_EQ(m.bus().ram()[0x10004], 0);
    EXPECT_EQ(m.bus().ram()[0x10007], 0);
}

TEST_F(BiosTest, LoadExeSetsGpAndDefaultStack) {
    ExeSpec s;
    s.gp = 0x80077777;
    bios.load_exe(make_exe(s));
    EXPECT_EQ(ctx().r[28], 0x80077777u);
    EXPECT_EQ(ctx().r[29], 0x801FFF00u);
    EXPECT_EQ(ctx().r[30], 0x801FFF00u);
}

TEST_F(BiosTest, LoadExeStackParameterOverridesDefault) {
    bios.load_exe(make_exe({}), 0x801F0000);
    EXPECT_EQ(ctx().r[29], 0x801F0000u);
}

TEST_F(BiosTest, LoadExeStackFromHeaderIsBasePlusOffset) {
    ExeSpec s;
    s.sp_base = 0x801FFF00;
    s.sp_off = 0x10;
    bios.load_exe(make_exe(s), 0x801F0000);
    EXPECT_EQ(ctx().r[29], 0x801FFF10u);
    EXPECT_EQ(ctx().r[30], 0x801FFF10u);
}

TEST_F(BiosTest, LoadExeHeaderBaseWithZeroOffset) {
    ExeSpec s;
    s.sp_base = 0x801EEE00;
    bios.load_exe(make_exe(s));
    EXPECT_EQ(ctx().r[29], 0x801EEE00u);
}

TEST_F(BiosTest, LoadExeSetsSrWithCop2Only) {
    ctx().cop0[12] = 0xFFFFFFFF;
    bios.load_exe(make_exe({}));
    EXPECT_EQ(ctx().cop0[12], 0x40000000u);
}

TEST_F(BiosTest, LoadExeClearsOtherRegisters) {
    fill_regs(ctx());
    bios.load_exe(make_exe({}));
    for (int i = 0; i < 32; ++i) {
        if (i == 28 || i == 29 || i == 30) continue;
        EXPECT_EQ(ctx().r[i], 0u) << i;
    }
}

TEST_F(BiosTest, LoadExeRejectsBadMagic) {
    auto f = make_exe({});
    f[0] = 'X';
    EXPECT_THROW(bios.load_exe(f), std::runtime_error);
}

TEST_F(BiosTest, LoadExeRejectsShortFile) {
    std::vector<uint8_t> f(0x7FF, 0);
    std::memcpy(f.data(), "PS-X EXE", 8);
    EXPECT_THROW(bios.load_exe(f), std::runtime_error);
    EXPECT_THROW(bios.load_exe({}), std::runtime_error);
}

TEST_F(BiosTest, LoadExeRejectsTruncatedText) {
    auto f = make_exe({});
    f.pop_back();
    EXPECT_THROW(bios.load_exe(f), std::runtime_error);
}

TEST_F(BiosTest, LoadExeAcceptsEmptyText) {
    ExeSpec s;
    s.text.clear();
    EXPECT_EQ(bios.load_exe(make_exe(s)), s.pc);
}

TEST_F(BiosTest, LoadExeFatalMessageNamesProblem) {
    auto f = make_exe({});
    f[1] = 'Q';
    try {
        bios.load_exe(f);
        FAIL();
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("PS-X EXE"), std::string::npos);
    }
}

// Regression: a text/bss range running past the end of RAM used to memcpy/memset beyond the host buffer.
TEST_F(BiosTest, LoadExeRejectsTextBeyondRam) {
    ExeSpec s;
    s.addr = 0x801FFFFC;  // 8 bytes of text: 4 beyond the 2 MiB
    EXPECT_THROW(bios.load_exe(make_exe(s)), std::runtime_error);
}

TEST_F(BiosTest, LoadExeRejectsBssBeyondRam) {
    ExeSpec s;
    s.bss = 0x801FFFF0;
    s.bss_size = 0x20;
    EXPECT_THROW(bios.load_exe(make_exe(s)), std::runtime_error);
}

TEST_F(BiosTest, LoadExeAcceptsTextEndingExactlyAtRamEnd) {
    ExeSpec s;
    s.addr = 0x801FFFF8;
    EXPECT_NO_THROW(bios.load_exe(make_exe(s)));
    EXPECT_EQ(m.bus().ram()[0x1FFFFF], 8);
}

TEST_F(BiosTest, LoadExeInvalidatesOverlayIdentification) {
    dw3_write32(&ctx(), kSlotBase + 0x10, 0x27BDFFE8);
    dw3_call(&ctx(), kSlotBase + 0x10);
    ASSERT_EQ(g_ova, 1);
    ExeSpec s;
    s.addr = kSlotBase + 0x10;
    s.text = {0xD0, 0xFF, 0xBD, 0x27};  // 0x27BDFFD0
    bios.load_exe(make_exe(s));
    dw3_call(&ctx(), kSlotBase + 0x10);
    EXPECT_EQ(g_ovb, 1);
}

// ---- run --------------------------------------------------------------------------------------

TEST_F(BiosTest, RunCallsEntryThroughLookup) {
    int n = 0;
    impl(0, [&](RecompContext*) { ++n; });
    bios.run(fn_addr(0));
    EXPECT_EQ(n, 1);
}

TEST_F(BiosTest, RunSetsRaToZero) {
    ctx().r[31] = 0x80012345;
    uint32_t seen = 1;
    impl(0, [&](RecompContext* c) { seen = c->r[31]; });
    bios.run(fn_addr(0));
    EXPECT_EQ(seen, 0u);
}

TEST_F(BiosTest, RunAcceptsKseg1EntryAlias) {
    int n = 0;
    impl(1, [&](RecompContext*) { ++n; });
    bios.run(0xA0000000 | (fn_addr(1) & 0x1FFFFFFF));
    EXPECT_EQ(n, 1);
}

TEST_F(BiosTest, RunUsesRegistersFromLoadExe) {
    ExeSpec s;
    s.gp = 0x80055555;
    bios.load_exe(make_exe(s));
    uint32_t gp = 0, sp = 0;
    impl(0, [&](RecompContext* c) { gp = c->r[28]; sp = c->r[29]; });
    bios.run(fn_addr(0));
    EXPECT_EQ(gp, 0x80055555u);
    EXPECT_EQ(sp, 0x801FFF00u);
}

TEST_F(BiosTest, RunUnknownEntryIsFatal) {
    EXPECT_THROW(bios.run(0x80010004), std::runtime_error);
}

// ---- Events -----------------------------------------------------------------------------------

TEST_F(BiosTest, OpenEventHandleFormat) {
    EXPECT_EQ(open_event(1, 2, 0x2000), 0xF1000000u);
    EXPECT_EQ(open_event(1, 2, 0x2000), 0xF1000001u);
    EXPECT_EQ(open_event(1, 2, 0x2000), 0xF1000002u);
}

TEST_F(BiosTest, OpenEventStoresFieldsAndWaitStatus) {
    uint32_t h = open_event(0x11, 0x22, Bios::kEvMdIntr, 0x80010100);
    const auto& e = bios.event(static_cast<int>(h & 0xFFFF));
    EXPECT_EQ(e.cls, 0x11u);
    EXPECT_EQ(e.spec, 0x22u);
    EXPECT_EQ(e.mode, Bios::kEvMdIntr);
    EXPECT_EQ(e.func, 0x80010100u);
    EXPECT_EQ(e.status, Bios::kEvStWait);
}

TEST_F(BiosTest, OpenEventExhaustsAfterSixteen) {
    for (uint32_t i = 0; i < 16; ++i) EXPECT_EQ(open_event(i, 1, 0x2000), 0xF1000000u | i);
    EXPECT_EQ(open_event(99, 1, 0x2000), 0xFFFFFFFFu);
    EXPECT_EQ(open_event(99, 1, 0x2000), 0xFFFFFFFFu);
}

TEST_F(BiosTest, CloseEventFreesSlotForReuse) {
    for (int i = 0; i < 16; ++i) open_event(i, 1, 0x2000);
    EXPECT_EQ(b0(0x09, 0xF1000005), 1u);
    EXPECT_EQ(open_event(7, 7, 0x2000), 0xF1000005u);
    EXPECT_EQ(bios.event(5).cls, 7u);
}

TEST_F(BiosTest, OpenEventReusesLowestFreeSlot) {
    open_event(1, 1, 0x2000);
    open_event(2, 2, 0x2000);
    open_event(3, 3, 0x2000);
    b0(0x09, 0xF1000002);
    b0(0x09, 0xF1000000);
    EXPECT_EQ(open_event(4, 4, 0x2000), 0xF1000000u);
    EXPECT_EQ(open_event(5, 5, 0x2000), 0xF1000002u);
}

TEST_F(BiosTest, CloseEventClearsStatus) {
    uint32_t h = open_enabled(1, 1, 0x2000);
    EXPECT_EQ(b0(0x09, h), 1u);
    EXPECT_EQ(bios.event(0).status, 0u);
}

TEST_F(BiosTest, CloseEventInvalidHandleReturnsZero) {
    EXPECT_EQ(b0(0x09, 0xF1000000), 0u);  // never opened
    uint32_t h = open_event(1, 1, 0x2000);
    b0(0x09, h);
    EXPECT_EQ(b0(0x09, h), 0u);  // double close
}

TEST_F(BiosTest, InvalidHandlesAreRejectedEverywhere) {
    open_event(1, 1, 0x2000);
    for (uint32_t bad : {0u, 0xFFFFFFFFu, 0xF2000000u, 0xF1010000u, 0xF1000010u, 0xF1000100u, 0x80000000u}) {
        EXPECT_EQ(b0(0x0C, bad), 0u) << std::hex << bad;
        EXPECT_EQ(b0(0x0B, bad), 0u) << std::hex << bad;
        EXPECT_EQ(b0(0x0A, bad), 0u) << std::hex << bad;
        EXPECT_EQ(b0(0x09, bad), 0u) << std::hex << bad;
    }
    EXPECT_EQ(bios.event(0).status, Bios::kEvStWait);
}

TEST_F(BiosTest, EnableEventActivates) {
    uint32_t h = open_event(1, 1, 0x2000);
    EXPECT_EQ(b0(0x0C, h), 1u);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
}

TEST_F(BiosTest, EnableEventOnClosedHandleFails) {
    uint32_t h = open_event(1, 1, 0x2000);
    b0(0x09, h);
    EXPECT_EQ(b0(0x0C, h), 0u);
    EXPECT_EQ(bios.event(0).status, 0u);
}

TEST_F(BiosTest, TestEventOnWaitAndActiveReturnsZero) {
    uint32_t h = open_event(1, 1, 0x2000);
    EXPECT_EQ(b0(0x0B, h), 0u);
    b0(0x0C, h);
    EXPECT_EQ(b0(0x0B, h), 0u);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
}

TEST_F(BiosTest, TestEventConsumesAlready) {
    uint32_t h = open_enabled(1, 1, 0x2000);
    bios.deliver_event(1, 1);
    ASSERT_EQ(bios.event(0).status, Bios::kEvStAlready);
    EXPECT_EQ(b0(0x0B, h), 1u);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
    EXPECT_EQ(b0(0x0B, h), 0u);
}

TEST_F(BiosTest, TestEventResetsV0WhenNotSignalled) {
    uint32_t h = open_enabled(1, 1, 0x2000);
    ctx().r[2] = 77;
    EXPECT_EQ(b0(0x0B, h), 0u);
}

TEST_F(BiosTest, DeliverNoIntrSetsAlready) {
    open_enabled(5, 6, Bios::kEvMdNoIntr);
    bios.deliver_event(5, 6);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStAlready);
}

TEST_F(BiosTest, DeliverOnlyToActiveEvents) {
    open_event(5, 6, Bios::kEvMdNoIntr);  // WAIT (never enabled)
    bios.deliver_event(5, 6);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStWait);
}

TEST_F(BiosTest, DeliverIgnoresNonMatchingClassOrSpec) {
    open_enabled(5, 6, Bios::kEvMdNoIntr);
    bios.deliver_event(5, 7);
    bios.deliver_event(4, 6);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
}

TEST_F(BiosTest, DeliverReachesEveryMatchingEvent) {
    open_enabled(5, 6, Bios::kEvMdNoIntr);
    open_enabled(5, 6, Bios::kEvMdNoIntr);
    open_enabled(5, 7, Bios::kEvMdNoIntr);
    bios.deliver_event(5, 6);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStAlready);
    EXPECT_EQ(bios.event(1).status, Bios::kEvStAlready);
    EXPECT_EQ(bios.event(2).status, Bios::kEvStActive);
}

TEST_F(BiosTest, DeliverLeavesAlreadyEventsAlone) {
    open_enabled(5, 6, Bios::kEvMdNoIntr);
    bios.deliver_event(5, 6);
    bios.deliver_event(5, 6);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStAlready);
}

TEST_F(BiosTest, DeliverAfterTestRearmsEvent) {
    uint32_t h = open_enabled(5, 6, Bios::kEvMdNoIntr);
    bios.deliver_event(5, 6);
    ASSERT_EQ(b0(0x0B, h), 1u);
    bios.deliver_event(5, 6);
    EXPECT_EQ(b0(0x0B, h), 1u);
}

TEST_F(BiosTest, DeliverIntrCallsGameFunction) {
    int n = 0;
    impl(2, [&](RecompContext*) { ++n; });
    open_enabled(5, 6, Bios::kEvMdIntr, fn_addr(2));
    bios.deliver_event(5, 6);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);  // callback events stay armed
}

TEST_F(BiosTest, DeliverIntrCallsEachTimeAndSetsRa) {
    int n = 0;
    uint32_t ra = 1;
    impl(2, [&](RecompContext* c) { ++n; ra = c->r[31]; });
    open_enabled(5, 6, Bios::kEvMdIntr, fn_addr(2));
    bios.deliver_event(5, 6);
    bios.deliver_event(5, 6);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(ra, 0u);
}

TEST_F(BiosTest, DeliverIntrPreservesAllRegisters) {
    impl(2, [](RecompContext* c) { trash_regs(c); });
    open_enabled(5, 6, Bios::kEvMdIntr, fn_addr(2));
    fill_regs(ctx());
    RecompContext before = ctx();
    bios.deliver_event(5, 6);
    EXPECT_EQ(std::memcmp(before.r, ctx().r, sizeof(before.r)), 0);
    EXPECT_EQ(ctx().hi, 0xAAAA0001u);
    EXPECT_EQ(ctx().lo, 0xBBBB0002u);
}

TEST_F(BiosTest, DeliverIntrWithoutFunctionIsHarmless) {
    open_enabled(5, 6, Bios::kEvMdIntr, 0);
    bios.deliver_event(5, 6);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
}

TEST_F(BiosTest, DeliverIntrOnWaitEventDoesNotCall) {
    int n = 0;
    impl(2, [&](RecompContext*) { ++n; });
    open_event(5, 6, Bios::kEvMdIntr, fn_addr(2));
    bios.deliver_event(5, 6);
    EXPECT_EQ(n, 0);
}

TEST_F(BiosTest, DeliverWithUnknownModeDoesNothing) {
    int n = 0;
    impl(2, [&](RecompContext*) { ++n; });
    open_enabled(5, 6, 0x0, fn_addr(2));
    bios.deliver_event(5, 6);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
}

TEST_F(BiosTest, DeliverEventSyscallBehavesLikeDirectCall) {
    open_enabled(5, 6, Bios::kEvMdNoIntr);
    b0(0x07, 5, 6);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStAlready);
}

TEST_F(BiosTest, DeliverEventBiosCallPreservesRegisters) {
    impl(2, [](RecompContext* c) { trash_regs(c); });
    open_enabled(5, 6, Bios::kEvMdIntr, fn_addr(2));
    fill_regs(ctx());
    ctx().r[4] = 5;
    ctx().r[5] = 6;
    ctx().r[2] = 0x1002;
    dw3_bios_call(&ctx(), 0xB0, 0x07);
    for (int i = 1; i < 32; ++i) {
        uint32_t expect = i == 4 ? 5u : i == 5 ? 6u : 0x1000u + static_cast<uint32_t>(i);
        EXPECT_EQ(ctx().r[i], expect) << i;
    }
    EXPECT_EQ(ctx().hi, 0xAAAA0001u);
}

TEST_F(BiosTest, WaitEventReturnsImmediatelyIfAlready) {
    uint32_t h = open_enabled(5, 6, Bios::kEvMdNoIntr);
    bios.deliver_event(5, 6);
    int64_t t = ctx().cycles;
    EXPECT_EQ(b0(0x0A, h), 1u);
    EXPECT_EQ(ctx().cycles, t);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
}

TEST_F(BiosTest, WaitEventAdvancesTimeUntilSchedulerDelivers) {
    uint32_t h = open_enabled(5, 6, Bios::kEvMdNoIntr);
    m.scheduler().schedule(5000, [&](int64_t) { bios.deliver_event(5, 6); });
    EXPECT_EQ(b0(0x0A, h), 1u);
    EXPECT_EQ(ctx().cycles, 5000);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
}

TEST_F(BiosTest, WaitEventWalksThroughSeveralEvents) {
    uint32_t h = open_enabled(5, 6, Bios::kEvMdNoIntr);
    int ticks = 0;
    m.scheduler().schedule(1000, [&](int64_t) { ++ticks; });
    m.scheduler().schedule(2000, [&](int64_t) { ++ticks; });
    m.scheduler().schedule(3000, [&](int64_t) { bios.deliver_event(5, 6); });
    m.scheduler().schedule(4000, [&](int64_t) { ++ticks; });
    b0(0x0A, h);
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(ctx().cycles, 3000);
}

TEST_F(BiosTest, WaitEventPreservesRegistersAcrossCallbacks) {
    uint32_t h = open_enabled(5, 6, Bios::kEvMdNoIntr);
    impl(2, [](RecompContext* c) { trash_regs(c); });
    m.scheduler().schedule(100, [&](int64_t) {
        dw3_call(&ctx(), fn_addr(2));
        bios.deliver_event(5, 6);
    });
    fill_regs(ctx());
    ctx().r[4] = h;
    ctx().r[2] = 0;
    dw3_bios_call(&ctx(), 0xB0, 0x0A);
    EXPECT_EQ(ctx().r[2], 1u);
    for (int i = 3; i < 32; ++i) {
        if (i == 4) continue;
        EXPECT_EQ(ctx().r[i], 0x1000u + static_cast<uint32_t>(i)) << i;
    }
}

TEST_F(BiosTest, WaitEventFatalWhenNothingIsScheduled) {
    uint32_t h = open_enabled(5, 6, Bios::kEvMdNoIntr);
    ctx().next_event = INT64_MAX;
    EXPECT_THROW(b0(0x0A, h), std::runtime_error);
}

TEST_F(BiosTest, WaitEventFatalWhenEventNeverArrives) {
    uint32_t h = open_enabled(5, 6, Bios::kEvMdNoIntr);
    int n = 0;
    m.scheduler().cancel(1);  // the VBLANK event scheduled by Machine
    m.scheduler().schedule(100, [&](int64_t) { ++n; });
    EXPECT_THROW(b0(0x0A, h), std::runtime_error);
    EXPECT_EQ(n, 1);
}

TEST_F(BiosTest, WaitEventForVblankThroughInterruptPath) {
    enable_irq(1);
    uint32_t h = open_enabled(kClassRCnt + 3, 2, Bios::kEvMdNoIntr);
    EXPECT_EQ(b0(0x0A, h), 1u);
    EXPECT_GE(ctx().cycles, m.video().vblank_cycle(0));
    EXPECT_EQ(bios.interrupts_delivered(), 1u);
}

TEST_F(BiosTest, WaitEventOnWaitStatusNeverCompletesEvenIfDelivered) {
    uint32_t h = open_event(5, 6, Bios::kEvMdNoIntr);  // not enabled
    m.scheduler().schedule(100, [&](int64_t) { bios.deliver_event(5, 6); });
    m.scheduler().cancel(1);
    EXPECT_THROW(b0(0x0A, h), std::runtime_error);
}

// ---- Syscalls ---------------------------------------------------------------------------------

TEST_F(BiosTest, EnterCriticalSectionReportsEnabled) {
    ctx().cop0[12] = 0x40000000 | kSrIen;
    ctx().r[4] = 1;
    dw3_syscall(&ctx(), 0x80010000);
    EXPECT_EQ(ctx().r[2], 1u);
}

TEST_F(BiosTest, EnterCriticalSectionClearsIecAndIm2Only) {
    ctx().cop0[12] = 0x40000000 | kSrIen | (1u << 8);
    ctx().r[4] = 1;
    dw3_syscall(&ctx(), 0);
    EXPECT_EQ(ctx().cop0[12], 0x40000000u | (1u << 8));
}

TEST_F(BiosTest, EnterCriticalSectionReportsDisabled) {
    ctx().cop0[12] = 0x40000000;
    ctx().r[2] = 55;
    ctx().r[4] = 1;
    dw3_syscall(&ctx(), 0);
    EXPECT_EQ(ctx().r[2], 0u);
    EXPECT_EQ(ctx().cop0[12], 0x40000000u);
}

TEST_F(BiosTest, EnterCriticalSectionPartiallyEnabledReportsZero) {
    ctx().cop0[12] = 1;  // IEc without IM2
    ctx().r[4] = 1;
    dw3_syscall(&ctx(), 0);
    EXPECT_EQ(ctx().r[2], 0u);
    EXPECT_EQ(ctx().cop0[12], 0u);  // IEc is cleared regardless
}

TEST_F(BiosTest, ExitCriticalSectionEnablesAndRequestsService) {
    ctx().cop0[12] = 0x40000000;
    ctx().cycles = 1234;
    ctx().next_event = 999999;
    ctx().r[4] = 2;
    dw3_syscall(&ctx(), 0);
    EXPECT_EQ(ctx().cop0[12], 0x40000000u | kSrIen);
    EXPECT_EQ(ctx().next_event, 1234);
}

TEST_F(BiosTest, CriticalSectionPairRoundTrips) {
    ctx().cop0[12] = 0x40000000 | kSrIen;
    ctx().r[4] = 1;
    dw3_syscall(&ctx(), 0);
    ctx().r[4] = 2;
    dw3_syscall(&ctx(), 0);
    EXPECT_EQ(ctx().cop0[12], 0x40000000u | kSrIen);
}

TEST_F(BiosTest, ExitCriticalSectionDeliversPendingInterrupt) {
    int n = 0;
    impl(0, [&](RecompContext*) { ++n; });
    enq(0, kData, fn_addr(0));
    poke(kIMask, 1);
    m.irq().raise(dw3::IrqSource::Vblank);
    ctx().cop0[12] = 0x40000000;
    ctx().r[4] = 2;
    dw3_syscall(&ctx(), 0);
    dw3_service_events(&ctx());  // what DW3_CHECK_EVENTS does at the next block boundary
    EXPECT_EQ(n, 1);
}

TEST_F(BiosTest, SyscallZeroIsNoOp) {
    ctx().cop0[12] = 0x1234;
    ctx().r[4] = 0;
    dw3_syscall(&ctx(), 0);
    EXPECT_EQ(ctx().cop0[12], 0x1234u);
}

TEST_F(BiosTest, UnsupportedSyscallIsFatal) {
    ctx().r[4] = 3;
    EXPECT_THROW(dw3_syscall(&ctx(), 0x80012345), std::runtime_error);
    ctx().r[4] = 0xFFFFFFFF;
    EXPECT_THROW(dw3_syscall(&ctx(), 0), std::runtime_error);
}

TEST_F(BiosTest, UnsupportedSyscallMessageHasNumberAndPc) {
    ctx().r[4] = 7;
    try {
        dw3_syscall(&ctx(), 0x80012345);
        FAIL();
    } catch (const std::runtime_error& e) {
        std::string w = e.what();
        EXPECT_NE(w.find("syscall(7)"), std::string::npos) << w;
        EXPECT_NE(w.find("80012345"), std::string::npos) << w;
    }
}

// ---- Interrupt path ---------------------------------------------------------------------------

TEST_F(BiosTest, InterruptNotTakenWhenCpuDisabled) {
    int n = 0;
    impl(0, [&](RecompContext*) { ++n; });
    enq(0, kData, fn_addr(0));
    poke(kIMask, 1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(bios.interrupts_delivered(), 0u);
}

TEST_F(BiosTest, InterruptNotTakenWhenMasked) {
    int n = 0;
    impl(0, [&](RecompContext*) { ++n; });
    enq(0, kData, fn_addr(0));
    enable_irq(0);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(n, 0);
}

TEST_F(BiosTest, InterruptRunsHandlerChain) {
    int n = 0;
    impl(0, [&](RecompContext*) { ++n; });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(bios.interrupts_delivered(), 1u);
}

TEST_F(BiosTest, InterruptSavesAndRestoresAllRegisters) {
    impl(0, [](RecompContext* c) { trash_regs(c); });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    fill_regs(ctx());
    RecompContext before = ctx();
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(std::memcmp(before.r, ctx().r, sizeof(before.r)), 0);
    EXPECT_EQ(ctx().hi, 0xAAAA0001u);
    EXPECT_EQ(ctx().lo, 0xBBBB0002u);
}

TEST_F(BiosTest, InterruptDisablesInterruptsInsideHandlerAndRestoresSr) {
    uint32_t sr_in = 0;
    impl(0, [&](RecompContext* c) { sr_in = c->cop0[12]; });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    uint32_t sr = ctx().cop0[12];
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(sr_in & 1u, 0u);         // IEc cleared
    EXPECT_EQ((sr_in >> 2) & 1u, 1u);  // old IEc pushed to IEp
    EXPECT_EQ(sr_in & (1u << 10), 1u << 10);  // IM bits untouched
    EXPECT_EQ(ctx().cop0[12], sr);
}

TEST_F(BiosTest, InterruptPushesWholeModeStack) {
    uint32_t sr_in = 0;
    impl(0, [&](RecompContext* c) { sr_in = c->cop0[12]; });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    ctx().cop0[12] = kSrIen | (0x3u << 2);  // IEp, KUp set
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(sr_in & 0x3Fu, 0x34u);  // cur = 0, prev = old cur (0b01), old = old prev (0b11)
}

TEST_F(BiosTest, InterruptClearsCauseExcCode) {
    uint32_t cause = 0xFFFFFFFF;
    impl(0, [&](RecompContext* c) { cause = c->cop0[13]; });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    ctx().cop0[13] = 0x7C;
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(cause & 0x7C, 0u);
}

TEST_F(BiosTest, InterruptIsNotReentrant) {
    int n = 0;
    impl(0, [&](RecompContext* c) {
        ++n;
        dw3_mtc0(c, 12, c->cop0[12] | kSrIen);  // handler wrongly re-enables interrupts
        m.irq().raise(dw3::IrqSource::Vblank);
        m.service_events();
        bios.interrupt(*c);
    });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(bios.interrupts_delivered(), 1u);
}

TEST_F(BiosTest, InterruptCanBeTakenAgainAfterReturn) {
    int n = 0;
    impl(0, [&](RecompContext*) { ++n; });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(bios.interrupts_delivered(), 2u);
}

TEST_F(BiosTest, InterruptStateRecoversAfterHandlerThrowsReturnFromException) {
    impl(0, [](RecompContext* c) { dw3_bios_call(c, 0xB0, 0x17); });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(bios.interrupts_delivered(), 2u);
}

TEST_F(BiosTest, ReturnFromExceptionOutsideInterruptThrows) {
    EXPECT_THROW(b0(0x17), dw3::ReturnFromException);
}

// SysEnqIntRP / SysDeqIntRP

TEST_F(BiosTest, EnqPutsNewestNodeFirst) {
    std::vector<int> order;
    impl(0, [&](RecompContext*) { order.push_back(0); });
    impl(1, [&](RecompContext*) { order.push_back(1); });
    impl(2, [&](RecompContext*) { order.push_back(2); });
    enq(0, kData + 0x00, fn_addr(0));
    enq(0, kData + 0x20, fn_addr(1));
    enq(0, kData + 0x40, fn_addr(2));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(order, (std::vector<int>{2, 1, 0}));
}

TEST_F(BiosTest, EnqLinksNextPointer) {
    enq(1, kData, fn_addr(0));
    EXPECT_EQ(peek(kData), 0u);
    enq(1, kData + 0x20, fn_addr(1));
    EXPECT_EQ(peek(kData + 0x20), kData);
}

TEST_F(BiosTest, ChainsRunInPriorityOrder) {
    std::vector<int> order;
    for (int i = 0; i < 4; ++i) impl(i, [&order, i](RecompContext*) { order.push_back(i); });
    enq(3, kData + 0x60, fn_addr(3));
    enq(1, kData + 0x20, fn_addr(1));
    enq(0, kData + 0x00, fn_addr(0));
    enq(2, kData + 0x40, fn_addr(2));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
}

TEST_F(BiosTest, EnqIgnoresInvalidPriority) {
    int n = 0;
    impl(0, [&](RecompContext*) { ++n; });
    poke(kData, 0x12345678);
    enq(4, kData, fn_addr(0));
    EXPECT_EQ(peek(kData), 0x12345678u);  // node untouched
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(n, 0);
}

TEST_F(BiosTest, Func2RunsWithV0OnlyWhenFunc1ReturnsNonzero) {
    uint32_t arg = 0;
    int f2 = 0;
    impl(0, [](RecompContext* c) { c->r[2] = 0x55; });
    impl(1, [&](RecompContext* c) { ++f2; arg = c->r[4]; });
    enq(0, kData, fn_addr(0), fn_addr(1));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(f2, 1);
    EXPECT_EQ(arg, 0x55u);
}

TEST_F(BiosTest, Func2SkippedWhenFunc1ReturnsZero) {
    int f2 = 0;
    impl(0, [](RecompContext* c) { c->r[2] = 0; });
    impl(1, [&](RecompContext*) { ++f2; });
    enq(0, kData, fn_addr(0), fn_addr(1));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(f2, 0);
}

TEST_F(BiosTest, NonzeroV0WithoutFunc2IsHarmless) {
    int f1 = 0;
    impl(0, [&](RecompContext* c) { ++f1; c->r[2] = 1; });
    enq(0, kData, fn_addr(0), 0);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(f1, 1);
}

TEST_F(BiosTest, NodeWithoutFunc1IsSkippedButChainContinues) {
    int n = 0;
    impl(1, [&](RecompContext*) { ++n; });
    enq(0, kData, fn_addr(1));
    enq(0, kData + 0x20, 0, fn_addr(2));  // no func1
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(n, 1);
}

TEST_F(BiosTest, Func1RunsWithRaZero) {
    uint32_t ra = 1;
    impl(0, [&](RecompContext* c) { ra = c->r[31]; });
    enq(0, kData, fn_addr(0));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(ra, 0u);
}

TEST_F(BiosTest, ChainNodesAreReadFromRamAtDeliveryTime) {
    int a = 0, b = 0;
    impl(0, [&](RecompContext*) { ++a; });
    impl(1, [&](RecompContext*) { ++b; });
    enq(0, kData, fn_addr(0));
    poke(kData + 8, fn_addr(1));  // game patches func1 in place
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
}

struct ThreeNodeChain : BiosTest {
    std::vector<int> order;
    void SetUp() override {
        for (int i = 0; i < 3; ++i) impl(i, [this, i](RecompContext*) { order.push_back(i); });
        enq(0, kData + 0x00, fn_addr(0));  // tail
        enq(0, kData + 0x20, fn_addr(1));  // middle
        enq(0, kData + 0x40, fn_addr(2));  // head
        enable_irq(1);
    }
    void run() {
        order.clear();
        fire(dw3::IrqSource::Vblank);
    }
};

TEST_F(ThreeNodeChain, DeqHead) {
    c0(0x03, 0, kData + 0x40);
    run();
    EXPECT_EQ(order, (std::vector<int>{1, 0}));
}

TEST_F(ThreeNodeChain, DeqMiddle) {
    c0(0x03, 0, kData + 0x20);
    run();
    EXPECT_EQ(order, (std::vector<int>{2, 0}));
    EXPECT_EQ(peek(kData + 0x40), kData);
}

TEST_F(ThreeNodeChain, DeqTail) {
    c0(0x03, 0, kData);
    run();
    EXPECT_EQ(order, (std::vector<int>{2, 1}));
    EXPECT_EQ(peek(kData + 0x20), 0u);
}

TEST_F(ThreeNodeChain, DeqAllThenNothingRuns) {
    c0(0x03, 0, kData + 0x40);
    c0(0x03, 0, kData + 0x20);
    c0(0x03, 0, kData);
    run();
    EXPECT_TRUE(order.empty());
}

TEST_F(ThreeNodeChain, DeqUnknownNodeIsNoOp) {
    c0(0x03, 0, kData + 0x80);
    run();
    EXPECT_EQ(order, (std::vector<int>{2, 1, 0}));
}

TEST_F(ThreeNodeChain, DeqWrongPriorityIsNoOp) {
    c0(0x03, 1, kData + 0x40);
    c0(0x03, 9, kData + 0x40);
    run();
    EXPECT_EQ(order, (std::vector<int>{2, 1, 0}));
}

TEST_F(ThreeNodeChain, ReEnqueueAfterDequeue) {
    c0(0x03, 0, kData + 0x20);
    enq(0, kData + 0x20, fn_addr(1));
    run();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 0}));
}

TEST_F(ThreeNodeChain, ChainRunsOnEveryInterruptSource) {
    // The kernel runs all handlers whatever the source; handlers inspect I_STAT themselves.
    poke(kIMask, 1u << 3);
    run();  // vblank not masked in: no interrupt
    EXPECT_TRUE(order.empty());
    fire(dw3::IrqSource::Dma);
    EXPECT_EQ(order, (std::vector<int>{2, 1, 0}));
}

// Root counters

TEST_F(BiosTest, VblankDeliversRootCounterEvent) {
    open_enabled(kClassRCnt + 3, 2, Bios::kEvMdNoIntr);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStAlready);
}

TEST_F(BiosTest, TimerIrqsMapToRootCounterClasses) {
    struct Case { dw3::IrqSource src; uint32_t mask_bit; uint32_t cls; };
    const Case cases[] = {{dw3::IrqSource::Tmr0, 1u << 4, 0xF2000000}, {dw3::IrqSource::Tmr1, 1u << 5, 0xF2000001},
                          {dw3::IrqSource::Tmr2, 1u << 6, 0xF2000002}};
    for (const auto& k : cases) {
        open_enabled(k.cls, 2, Bios::kEvMdNoIntr);
    }
    enable_irq((1u << 4) | (1u << 5) | (1u << 6));
    for (int i = 0; i < 3; ++i) {
        fire(cases[i].src);
        for (int j = 0; j < 3; ++j) {
            EXPECT_EQ(bios.event(j).status, j <= i ? Bios::kEvStAlready : Bios::kEvStActive) << i << "," << j;
        }
    }
}

TEST_F(BiosTest, RootCounterEventRequiresSpec2) {
    open_enabled(kClassRCnt + 3, 1, Bios::kEvMdNoIntr);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
}

TEST_F(BiosTest, RootCounterIntrEventCallsFunction) {
    int n = 0;
    impl(3, [&](RecompContext*) { ++n; });
    open_enabled(kClassRCnt + 3, 2, Bios::kEvMdIntr, fn_addr(3));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(n, 1);
}

TEST_F(BiosTest, RootCounterEventsRunAfterChains) {
    std::vector<std::string> order;
    impl(0, [&](RecompContext*) { order.push_back("chain"); });
    impl(3, [&](RecompContext*) { order.push_back("event"); });
    enq(0, kData, fn_addr(0));
    open_enabled(kClassRCnt + 3, 2, Bios::kEvMdIntr, fn_addr(3));
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(order, (std::vector<std::string>{"chain", "event"}));
}

TEST_F(BiosTest, VblankIsAckedByDefault) {
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.irq().stat() & 1u, 0u);
}

TEST_F(BiosTest, ChangeClearRCntReturnsOldFlag) {
    EXPECT_EQ(c0(0x0A, 3, 0), 1u);  // default: auto-ack on
    EXPECT_EQ(c0(0x0A, 3, 1), 0u);
    EXPECT_EQ(c0(0x0A, 3, 5), 1u);  // any nonzero is on
}

TEST_F(BiosTest, ChangeClearRCntOffLeavesVblankPending) {
    c0(0x0A, 3, 0);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.irq().stat() & 1u, 1u);
}

TEST_F(BiosTest, ChangeClearRCntOffStillDeliversEvent) {
    c0(0x0A, 3, 0);
    open_enabled(kClassRCnt + 3, 2, Bios::kEvMdNoIntr);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(bios.event(0).status, Bios::kEvStAlready);
}

TEST_F(BiosTest, ChangeClearRCntIsPerCounter) {
    c0(0x0A, 0, 0);
    enable_irq((1u << 4) | (1u << 5));
    fire(dw3::IrqSource::Tmr0);
    EXPECT_TRUE(m.irq().stat() & (1u << 4));  // counter 0 not acked
    fire(dw3::IrqSource::Tmr1);
    EXPECT_FALSE(m.irq().stat() & (1u << 5));  // counter 1 still acked
}

TEST_F(BiosTest, ChangeClearRCntAcksTimerBits) {
    enable_irq(1u << 6);
    fire(dw3::IrqSource::Tmr2);
    EXPECT_FALSE(m.irq().stat() & (1u << 6));
}

TEST_F(BiosTest, ChangeClearRCntInvalidCounterIgnored) {
    c0(0x0A, 3, 1);
    ctx().r[2] = 0x99;
    EXPECT_EQ(c0(0x0A, 4, 0), 0x99u);  // v0 not touched, no flag changed
    EXPECT_EQ(c0(0x0A, 3, 1), 1u);
}

TEST_F(BiosTest, AckDoesNotTouchOtherPendingSources) {
    enable_irq(1);
    m.irq().raise(dw3::IrqSource::Dma);
    m.irq().raise(dw3::IrqSource::Vblank);
    m.service_events();
    EXPECT_EQ(m.irq().stat(), 1u << 3);
}

// Pads

TEST_F(BiosTest, InitPadReturnsTwoAndStartPadEnablesVblankMask) {
    EXPECT_EQ(b0(0x12, kData, 0x22, kData + 0x40, 0x22), 2u);
    b0(0x13);
    EXPECT_EQ(m.irq().mask() & 1u, 1u);
}

TEST_F(BiosTest, StartPadKeepsOtherMaskBits) {
    poke(kIMask, 1u << 2);
    b0(0x13);
    EXPECT_EQ(m.irq().mask(), (1u << 2) | 1u);
}

TEST_F(BiosTest, PadPollingFillsBuffers) {
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    bios.pads[0].buttons = 0xFEFB;
    bios.pads[1].buttons = 0x7FFF;
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    const uint8_t* ram = m.bus().ram();
    EXPECT_EQ(ram[0x100000], 0x00);  // status: pad present
    EXPECT_EQ(ram[0x100001], 0x41);  // id: digital
    EXPECT_EQ(ram[0x100002], 0xFB);
    EXPECT_EQ(ram[0x100003], 0xFE);
    EXPECT_EQ(ram[0x100040], 0x00);
    EXPECT_EQ(ram[0x100041], 0x41);
    EXPECT_EQ(ram[0x100042], 0xFF);
    EXPECT_EQ(ram[0x100043], 0x7F);
}

TEST_F(BiosTest, PadDefaultsToAllReleased) {
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(peek(kData) >> 16, 0xFFFFu);
}

TEST_F(BiosTest, DisconnectedPadReportsFF) {
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    bios.pads[1].connected = false;
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.bus().ram()[0x100000], 0x00);
    EXPECT_EQ(m.bus().ram()[0x100040], 0xFF);
}

TEST_F(BiosTest, PadBuffersUntouchedBeforeStartPad) {
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(peek(kData), 0u);
    EXPECT_EQ(m.irq().stat() & 1u, 0u);
}

TEST_F(BiosTest, PadBuffersUntouchedWithoutVblank) {
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    enable_irq(1u << 3);
    fire(dw3::IrqSource::Dma);
    EXPECT_EQ(peek(kData), 0u);
}

TEST_F(BiosTest, PadPollingTracksButtonChanges) {
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    enable_irq(1);
    bios.pads[0].buttons = 0xFFFE;
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.bus().ram()[0x100002], 0xFE);
    bios.pads[0].buttons = 0xFFFF;
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.bus().ram()[0x100002], 0xFF);
}

TEST_F(BiosTest, PadWithTooSmallBufferIsSkipped) {
    b0(0x12, kData, 3, kData + 0x40, 0x22);
    b0(0x13);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(peek(kData), 0u);
    EXPECT_EQ(m.bus().ram()[0x100041], 0x41);
}

TEST_F(BiosTest, PadWithNullBufferIsSkipped) {
    b0(0x12, 0, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    enable_irq(1);
    EXPECT_NO_THROW(fire(dw3::IrqSource::Vblank));
    EXPECT_EQ(m.bus().ram()[0x100041], 0x41);
}

TEST_F(BiosTest, PadPollingDoesNotWriteBeyondFourBytes) {
    poke(kData + 4, 0xCAFEBABE);
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(peek(kData + 4), 0xCAFEBABEu);
}

TEST_F(BiosTest, PadPollingPrecedesChains) {
    uint8_t seen = 0xEE;
    impl(0, [&](RecompContext* c) { seen = dw3_read8(c, kData + 1); });
    enq(0, kData + 0x80, fn_addr(0));
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(seen, 0x41);
}

TEST_F(BiosTest, PadPollingAcksVblankByDefault) {
    b0(0x13);
    c0(0x0A, 3, 0);  // isolate the pad path from the root-counter path
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.irq().stat() & 1u, 0u);
}

TEST_F(BiosTest, ChangeClearPadOffLeavesVblankPending) {
    b0(0x13);
    c0(0x0A, 3, 0);
    b0(0x5B, 0);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.irq().stat() & 1u, 1u);
}

TEST_F(BiosTest, ChangeClearPadCanBeReEnabled) {
    b0(0x13);
    c0(0x0A, 3, 0);
    b0(0x5B, 0);
    b0(0x5B, 1);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.irq().stat() & 1u, 0u);
}

TEST_F(BiosTest, ChangeClearPadOffDoesNotStopPolling) {
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x13);
    b0(0x5B, 0);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(m.bus().ram()[0x100001], 0x41);
}

TEST_F(BiosTest, OutdatedPadInitAndStartWritesButtonWord) {
    b0(0x15, 0, kData + 0x100);
    bios.pads[0].buttons = 0xFFFE;
    bios.pads[1].buttons = 0x7FFF;
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(peek(kData + 0x100), 0x80000001u);  // active-high, pad 2 in the upper half
}

TEST_F(BiosTest, OutdatedPadAllReleasedIsZero) {
    poke(kData + 0x100, 0x1234);
    b0(0x15, 0, kData + 0x100);
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(peek(kData + 0x100), 0u);
}

TEST_F(BiosTest, OutdatedPadCoexistsWithInitPad) {
    b0(0x12, kData, 0x22, kData + 0x40, 0x22);
    b0(0x15, 0, kData + 0x100);
    bios.pads[0].buttons = 0xEFFF;
    enable_irq(1);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(peek(kData + 0x100), 0x1000u);
    EXPECT_EQ(m.bus().ram()[0x100003], 0xEF);
}

// HookEntryInt custom exit

struct HookRegs {
    uint32_t ra = 0, sp = 0, fp = 0, gp = 0, v0 = 0, s[8]{};
    int calls = 0;
};

class HookTest : public BiosTest {
protected:
    static constexpr uint32_t kJmpBuf = kData + 0x200;
    HookTest() {
        poke(kJmpBuf + 0, fn_addr(4));
        poke(kJmpBuf + 4, 0x801EAAA0);
        poke(kJmpBuf + 8, 0x801EBBB0);
        for (uint32_t k = 0; k < 8; ++k) poke(kJmpBuf + 12 + 4 * k, 0x5000 + k);
        poke(kJmpBuf + 44, 0x80088888);
        enable_irq(1);
    }
    void capture(RecompContext* c) {
        seen.ra = c->r[31];
        seen.sp = c->r[29];
        seen.fp = c->r[30];
        seen.gp = c->r[28];
        seen.v0 = c->r[2];
        for (int k = 0; k < 8; ++k) seen.s[k] = c->r[16 + k];
        ++seen.calls;
    }
    HookRegs seen;
};

TEST_F(HookTest, HookEntryIntRecordsBuffer) {
    EXPECT_EQ(bios.hook_entry(), 0u);
    b0(0x19, kJmpBuf);
    EXPECT_EQ(bios.hook_entry(), kJmpBuf);
}

TEST_F(HookTest, ResetEntryIntClearsHook) {
    b0(0x19, kJmpBuf);
    b0(0x18);
    EXPECT_EQ(bios.hook_entry(), 0u);
    impl(4, [&](RecompContext* c) { capture(c); });
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(seen.calls, 0);
}

TEST_F(HookTest, HookedFunctionRunsWithRegistersFromJmpBuf) {
    impl(4, [&](RecompContext* c) { capture(c); });
    b0(0x19, kJmpBuf);
    fire(dw3::IrqSource::Vblank);
    ASSERT_EQ(seen.calls, 1);
    EXPECT_EQ(seen.ra, fn_addr(4));
    EXPECT_EQ(seen.sp, 0x801EAAA0u);
    EXPECT_EQ(seen.fp, 0x801EBBB0u);
    EXPECT_EQ(seen.gp, 0x80088888u);
    EXPECT_EQ(seen.v0, 1u);
    for (uint32_t k = 0; k < 8; ++k) EXPECT_EQ(seen.s[k], 0x5000 + k) << k;
}

TEST_F(HookTest, HookRunsAfterChainsAndRootCounterEvents) {
    std::vector<std::string> order;
    impl(0, [&](RecompContext*) { order.push_back("chain"); });
    impl(3, [&](RecompContext*) { order.push_back("event"); });
    impl(4, [&](RecompContext*) { order.push_back("hook"); });
    enq(0, kData, fn_addr(0));
    open_enabled(kClassRCnt + 3, 2, Bios::kEvMdIntr, fn_addr(3));
    b0(0x19, kJmpBuf);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(order, (std::vector<std::string>{"chain", "event", "hook"}));
}

TEST_F(HookTest, ReturnFromExceptionUnwindsAndRestoresInterruptedState) {
    bool after = false;
    impl(4, [&](RecompContext* c) {
        trash_regs(c);
        dw3_bios_call(c, 0xB0, 0x17);
        after = true;
    });
    b0(0x19, kJmpBuf);
    fill_regs(ctx());
    RecompContext before = ctx();
    fire(dw3::IrqSource::Vblank);
    EXPECT_FALSE(after);
    EXPECT_EQ(std::memcmp(before.r, ctx().r, sizeof(before.r)), 0);
    EXPECT_EQ(ctx().hi, 0xAAAA0001u);
    EXPECT_EQ(ctx().lo, 0xBBBB0002u);
    EXPECT_EQ(ctx().cop0[12], kSrIen);
}

TEST_F(HookTest, HookReturningNormallyAlsoRestoresState) {
    impl(4, [&](RecompContext* c) { trash_regs(c); });
    b0(0x19, kJmpBuf);
    fill_regs(ctx());
    RecompContext before = ctx();
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(std::memcmp(before.r, ctx().r, sizeof(before.r)), 0);
    EXPECT_EQ(ctx().cop0[12], kSrIen);
}

TEST_F(HookTest, HookCanBeReplacedAndCalledEveryInterrupt) {
    b0(0x19, kJmpBuf);
    impl(4, [&](RecompContext* c) { capture(c); });
    fire(dw3::IrqSource::Vblank);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(seen.calls, 2);
    EXPECT_EQ(bios.interrupts_delivered(), 2u);
}

TEST_F(HookTest, ExceptionFromChainHandlerStillSkipsHook) {
    // A handler that unwinds (ReturnFromException) ends the interrupt: the hook does not run afterwards.
    impl(0, [](RecompContext* c) { dw3_bios_call(c, 0xB0, 0x17); });
    impl(4, [&](RecompContext* c) { capture(c); });
    enq(0, kData, fn_addr(0));
    b0(0x19, kJmpBuf);
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(seen.calls, 0);
}

TEST_F(HookTest, NoHookMeansDefaultExit) {
    impl(4, [&](RecompContext* c) { capture(c); });
    fire(dw3::IrqSource::Vblank);
    EXPECT_EQ(seen.calls, 0);
    EXPECT_EQ(bios.interrupts_delivered(), 1u);
}

// ---- A0 / B0 / C0 misc ------------------------------------------------------------------------

TEST_F(BiosTest, GpuCwWritesGp0) {
    // GP0 E1h (draw mode) is mirrored into GPUSTAT bits 0-10, so the write is observable through the GPU.
    uint64_t before = m.gpu().commands();
    call(0xA0, 0x49, 0xE1000005);
    EXPECT_EQ(m.gpu().read_gpustat() & 0x7FFu, 0x005u);
    call(0xA0, 0x49, 0xE100000A);
    EXPECT_EQ(m.gpu().read_gpustat() & 0x7FFu, 0x00Au);
    EXPECT_EQ(m.gpu().commands(), before + 2);
}

TEST_F(BiosTest, GpuCwUsesA0AsCommandWord) {
    // Registers a1-a3 are not part of the command.
    call(0xA0, 0x49, 0xE1000003, 0xE1000007, 0xE1000007, 0xE1000007);
    EXPECT_EQ(m.gpu().read_gpustat() & 0x7FFu, 0x003u);
}

TEST_F(BiosTest, SimpleA0FunctionsAreHarmless) {
    for (uint32_t fn : {0x39u, 0x44u, 0x70u, 0x72u}) {
        ctx().r[2] = 0x1234;
        EXPECT_NO_THROW(call(0xA0, fn)) << fn;
    }
}

TEST_F(BiosTest, CardInitFunctionsAreHarmless) {
    for (uint32_t fn : {0x4Au, 0x4Bu, 0x4Cu, 0x50u}) EXPECT_NO_THROW(b0(fn)) << fn;
}

TEST_F(BiosTest, FileWriteToStdoutEmitsLines) {
    std::vector<std::string> lines;
    bios.on_tty = [&](const std::string& s) { lines.push_back(s); };
    const char text[] = "hello\nworld\n";
    std::memcpy(m.bus().ram() + 0x100000, text, 12);
    EXPECT_EQ(b0(0x35, 1, kData, 12), 12u);
    EXPECT_EQ(lines, (std::vector<std::string>{"hello", "world"}));
}

TEST_F(BiosTest, FileWriteBuffersPartialLines) {
    std::vector<std::string> lines;
    bios.on_tty = [&](const std::string& s) { lines.push_back(s); };
    std::memcpy(m.bus().ram() + 0x100000, "abc", 3);
    std::memcpy(m.bus().ram() + 0x100010, "def\ngh", 6);
    std::memcpy(m.bus().ram() + 0x100020, "i\n", 2);
    b0(0x35, 1, kData, 3);
    EXPECT_TRUE(lines.empty());
    b0(0x35, 1, kData + 0x10, 6);
    EXPECT_EQ(lines, (std::vector<std::string>{"abcdef"}));
    b0(0x35, 1, kData + 0x20, 2);
    EXPECT_EQ(lines, (std::vector<std::string>{"abcdef", "ghi"}));
}

TEST_F(BiosTest, FileWriteEmptyLinesAreReported) {
    std::vector<std::string> lines;
    bios.on_tty = [&](const std::string& s) { lines.push_back(s); };
    std::memcpy(m.bus().ram() + 0x100000, "\n\nx\n", 4);
    b0(0x35, 1, kData, 4);
    EXPECT_EQ(lines, (std::vector<std::string>{"", "", "x"}));
}

TEST_F(BiosTest, FileWriteKeepsCarriageReturn) {
    std::vector<std::string> lines;
    bios.on_tty = [&](const std::string& s) { lines.push_back(s); };
    std::memcpy(m.bus().ram() + 0x100000, "a\r\n", 3);
    b0(0x35, 1, kData, 3);
    EXPECT_EQ(lines, (std::vector<std::string>{"a\r"}));
}

TEST_F(BiosTest, FileWriteZeroLengthReturnsZero) {
    bool called = false;
    bios.on_tty = [&](const std::string&) { called = true; };
    EXPECT_EQ(b0(0x35, 1, kData, 0), 0u);
    EXPECT_FALSE(called);
}

TEST_F(BiosTest, FileWriteWithoutTtyCallbackStillSucceeds) {
    std::memcpy(m.bus().ram() + 0x100000, "x\n", 2);
    EXPECT_EQ(b0(0x35, 1, kData, 2), 2u);
}

TEST_F(BiosTest, FileWriteLongBufferIsCappedAtOnePage) {
    std::vector<std::string> lines;
    bios.on_tty = [&](const std::string& s) { lines.push_back(s); };
    std::memset(m.bus().ram() + 0x100000, 'z', 5000);
    m.bus().ram()[0x100000 + 4095] = '\n';
    EXPECT_EQ(b0(0x35, 1, kData, 5000), 5000u);  // reports full length
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].size(), 4095u);
}

TEST_F(BiosTest, FileWriteToOtherDescriptorsFails) {
    bool called = false;
    bios.on_tty = [&](const std::string&) { called = true; };
    std::memcpy(m.bus().ram() + 0x100000, "x\n", 2);
    for (uint32_t fd : {0u, 2u, 3u, 0xFFFFFFFFu}) EXPECT_EQ(b0(0x35, fd, kData, 2), 0xFFFFFFFFu) << fd;
    EXPECT_FALSE(called);
}

TEST_F(BiosTest, FileFunctionsFailOnNonCardPathsAndDescriptors) {
    for (uint32_t fn : {0x32u, 0x33u, 0x34u}) EXPECT_EQ(b0(fn, 1, 2, 3), 0xFFFFFFFFu) << fn;
}

TEST_F(BiosTest, FileCloseOfUnopenedDescriptorFails) {
    EXPECT_EQ(b0(0x36, 5), 0xFFFFFFFFu);
}

TEST_F(BiosTest, FirstAndNextFileWithoutCardReturnNone) {
    ctx().r[2] = 0x123;
    EXPECT_EQ(b0(0x42, kData, kData), 0u);
    ctx().r[2] = 0x123;
    EXPECT_EQ(b0(0x43, kData), 0u);
}

struct CardCase {
    uint32_t table, fn;
};

class CardTest : public BiosTest, public ::testing::WithParamInterface<CardCase> {
protected:
    // _card_info/_card_load report SwCARD (and HwCARD); sector transfers report HwCARD only.
    uint32_t ev_class() const { return GetParam().table == 0xA0 ? kClassSw : kClassHw; }
};

TEST_P(CardTest, ReturnsOneAndCompletesAsynchronously) {
    uint32_t sw = open_enabled(ev_class(), 0x100, Bios::kEvMdNoIntr);
    uint32_t hw = open_enabled(kClassHw, 0x100, Bios::kEvMdNoIntr);
    EXPECT_EQ(call(GetParam().table, GetParam().fn), 1u);
    m.service_events();
    EXPECT_EQ(b0(0x0B, hw), 0u);  // not yet
    ctx().cycles = 19999;
    m.service_events();
    EXPECT_EQ(bios.event(0).status, Bios::kEvStActive);
    ctx().cycles = 20000;
    m.service_events();
    EXPECT_EQ(b0(0x0B, sw), 1u);
    EXPECT_EQ(b0(0x0B, hw), 1u);
}

TEST_P(CardTest, TimeoutIsMeasuredFromTheCall) {
    ctx().cycles = 100000;
    uint32_t sw = open_enabled(ev_class(), 0x100, Bios::kEvMdNoIntr);
    call(GetParam().table, GetParam().fn);
    ctx().cycles = 119999;
    m.service_events();
    EXPECT_EQ(b0(0x0B, sw), 0u);
    ctx().cycles = 120000;
    m.service_events();
    EXPECT_EQ(b0(0x0B, sw), 1u);
}

TEST_P(CardTest, OnlyArmedEventsAreSignalled) {
    open_event(kClassSw, 0x100, Bios::kEvMdNoIntr);  // never enabled
    uint32_t other = open_enabled(kClassSw, 0x200, Bios::kEvMdNoIntr);
    call(GetParam().table, GetParam().fn);
    ctx().cycles = 20000;
    m.service_events();
    EXPECT_EQ(bios.event(0).status, Bios::kEvStWait);
    EXPECT_EQ(b0(0x0B, other), 0u);
}

TEST_P(CardTest, WaitEventCompletesViaScheduledTimeout) {
    uint32_t hw = open_enabled(kClassHw, 0x100, Bios::kEvMdNoIntr);
    call(GetParam().table, GetParam().fn);
    EXPECT_EQ(b0(0x0A, hw), 1u);
    EXPECT_EQ(ctx().cycles, 20000);
}

TEST_P(CardTest, IntrCallbackEventsGetCalledWithRegistersPreserved) {
    impl(5, [](RecompContext* c) { trash_regs(c); });
    open_enabled(ev_class(), 0x100, Bios::kEvMdIntr, fn_addr(5));
    call(GetParam().table, GetParam().fn);
    ctx().cycles = 20000;
    fill_regs(ctx());
    m.service_events();
    for (int i = 1; i < 32; ++i) EXPECT_EQ(ctx().r[i], 0x1000u + static_cast<uint32_t>(i)) << i;
}

TEST_P(CardTest, SeveralOutstandingRequestsEachFire) {
    int n = 0;
    impl(5, [&](RecompContext*) { ++n; });
    open_enabled(ev_class(), 0x100, Bios::kEvMdIntr, fn_addr(5));
    call(GetParam().table, GetParam().fn);
    ctx().cycles = 10;
    call(GetParam().table, GetParam().fn);
    ctx().cycles = 20010;
    m.service_events();
    EXPECT_EQ(n, 2);
}

INSTANTIATE_TEST_SUITE_P(Bios, CardTest,
                         ::testing::Values(CardCase{0xA0, 0xAB}, CardCase{0xA0, 0xAC}, CardCase{0xB0, 0x4E},
                                           CardCase{0xB0, 0x4F}));

TEST_F(BiosTest, UnknownFunctionsAreFatal) {
    EXPECT_THROW(call(0xA0, 0x00), std::runtime_error);
    EXPECT_THROW(call(0xA0, 0x3A), std::runtime_error);
    EXPECT_THROW(call(0xB0, 0x00), std::runtime_error);
    EXPECT_THROW(call(0xB0, 0x5D), std::runtime_error);
    EXPECT_THROW(call(0xC0, 0x00), std::runtime_error);
    EXPECT_THROW(call(0xC0, 0x1C), std::runtime_error);
    EXPECT_THROW(call(0x00, 0x39), std::runtime_error);
    EXPECT_THROW(call(0xB1, 0x08), std::runtime_error);
}

TEST_F(BiosTest, UnknownFunctionMessageNamesTableAndNumber) {
    try {
        call(0xA0, 0x3F);
        FAIL();
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("A0:3F"), std::string::npos) << e.what();
    }
}

TEST_F(BiosTest, FunctionNumbersAreTableSpecific) {
    // B0:49 and C0:49 are not A0:49 (GPU_cw).
    EXPECT_THROW(call(0xB0, 0x49), std::runtime_error);
    EXPECT_THROW(call(0xC0, 0x49), std::runtime_error);
    EXPECT_THROW(call(0xA0, 0x08), std::runtime_error);  // B0:08 is OpenEvent
}

TEST_F(BiosTest, CallCounterCountsEveryDispatch) {
    EXPECT_EQ(bios.calls(), 0u);
    b0(0x08, 1, 2, 3);
    call(0xA0, 0x44);
    EXPECT_THROW(call(0xA0, 0x00), std::runtime_error);
    EXPECT_EQ(bios.calls(), 3u);
}

TEST_F(BiosTest, LogReceivesFormattedCalls) {
    std::vector<std::string> log;
    bios.log = [&](const std::string& s) { log.push_back(s); };
    b0(0x08, 0xF2000003, 2, 0x2000, 0);
    call(0xA0, 0x44);
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], "BIOS B0:08(F2000003, 00000002, 00002000, 00000000)");
    EXPECT_EQ(log[1].rfind("BIOS A0:44(", 0), 0u);
}

TEST_F(BiosTest, LogCalledBeforeFatalForUnknownFunction) {
    std::vector<std::string> log;
    bios.log = [&](const std::string& s) { log.push_back(s); };
    EXPECT_THROW(call(0xC0, 0x77, 0xAABBCCDD), std::runtime_error);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], "BIOS C0:77(AABBCCDD, 00000000, 00000000, 00000000)");
}

TEST_F(BiosTest, WorksWithoutLogOrTtyCallbacks) {
    EXPECT_NO_THROW(b0(0x08, 1, 1, 0x2000));
}

TEST_F(BiosTest, ConstructorInstallsMachineHooks) {
    EXPECT_TRUE(static_cast<bool>(m.on_bios_call));
    EXPECT_TRUE(static_cast<bool>(m.on_syscall));
    EXPECT_TRUE(static_cast<bool>(m.on_interrupt));
}

TEST_F(BiosTest, PadsDefaultToConnectedAndReleased) {
    for (const auto& p : bios.pads) {
        EXPECT_TRUE(p.connected);
        EXPECT_EQ(p.buttons, 0xFFFFu);
    }
}

}  // namespace

TEST_F(BiosTest, GetC0AndB0TableReturnKernelAddresses) {
    call(0xB0, 0x56);
    EXPECT_EQ(m.ctx().r[2], 0x00000674u);
    call(0xB0, 0x57);
    EXPECT_EQ(m.ctx().r[2], 0x00000874u);
}

// ---- Memory cards -----------------------------------------------------------------------------

#include <array>

#include "dw3/memcard.h"
#include "hw_test_util.h"

namespace {

namespace mc = dw3::memcard;

constexpr uint32_t kSpecIoe = 0x0004, kSpecTimeout = 0x0100, kSpecNew = 0x2000, kSpecError = 0x8000;
constexpr uint32_t kFread = 1, kFwrite = 2, kFcreat = 0x200, kFasync = 0x8000;
constexpr uint32_t kPath = 0x80101000, kPath2 = 0x80101100, kBuf = 0x80102000, kEntry = 0x80103000;
constexpr uint32_t kFail = 0xFFFFFFFFu;

uint32_t creat(uint32_t blocks, uint32_t flags = kFwrite) { return (blocks << 16) | kFcreat | flags; }

std::vector<uint8_t> pat(std::size_t n, uint8_t seed = 1) {
    std::vector<uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 13 + (i >> 7));
    return v;
}

class CardBiosTest : public BiosTest {
protected:
    CardBiosTest() {
        for (auto& c : cards) c.assign(mc::kCardSize, 0);
        bios.on_card_write = [this](int slot) { writes.push_back(slot); };
        for (int i = 0; i < 4; ++i) {
            sw[i] = open_enabled(kClassSw, specs[i], Bios::kEvMdNoIntr);
            hw[i] = open_enabled(kClassHw, specs[i], Bios::kEvMdNoIntr);
        }
    }
    void insert(int slot, bool format = true) {
        auto& c = cards[static_cast<std::size_t>(slot)];
        if (format) mc::format_memory_card(std::span<uint8_t, mc::kCardSize>(c.data(), c.size()));
        ASSERT_TRUE(m.sio().attach_card(slot, c));
    }
    void str(uint32_t addr, const std::string& s) {
        for (std::size_t i = 0; i <= s.size(); ++i) dw3_write8(&ctx(), addr + static_cast<uint32_t>(i), i < s.size() ? s[i] : 0);
    }
    void put(uint32_t addr, const std::vector<uint8_t>& d) {
        for (std::size_t i = 0; i < d.size(); ++i) dw3_write8(&ctx(), addr + static_cast<uint32_t>(i), d[i]);
    }
    std::vector<uint8_t> get(uint32_t addr, std::size_t n) {
        std::vector<uint8_t> v(n);
        for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(dw3_read8(&ctx(), addr + static_cast<uint32_t>(i)));
        return v;
    }
    // Lets the card latency elapse and reports which (class, spec) events fired, e.g. "SW:IOE HW:IOE".
    std::string settle() {
        ctx().cycles += 20000;
        m.service_events();
        std::string r;
        static const char* names[4] = {"IOE", "ERR", "TMO", "NEW"};
        for (int i = 0; i < 4; ++i)
            if (b0(0x0B, sw[i])) r += std::string(r.empty() ? "" : " ") + "SW:" + names[i];
        for (int i = 0; i < 4; ++i)
            if (b0(0x0B, hw[i])) r += std::string(r.empty() ? "" : " ") + "HW:" + names[i];
        return r;
    }
    uint32_t open(const std::string& path, uint32_t mode) {
        str(kPath, path);
        return b0(0x32, kPath, mode);
    }
    uint32_t err() { return b0(0x54); }
    mc::CardFs fs(int slot = 0) { return mc::CardFs(cards[static_cast<std::size_t>(slot)]); }
    // Creates "bu00:NAME" with `blocks` blocks and returns its fd.
    uint32_t make(const std::string& name, uint32_t blocks, uint32_t flags = kFwrite | kFread) {
        return open("bu00:" + name, creat(blocks, flags));
    }

    std::array<std::vector<uint8_t>, 2> cards;
    std::vector<int> writes;
    static constexpr uint32_t specs[4] = {kSpecIoe, kSpecError, kSpecTimeout, kSpecNew};
    uint32_t sw[4]{}, hw[4]{};
};

// ---- _card_info / _card_load ----

TEST_F(CardBiosTest, CardInfoNoCardTimesOut) {
    EXPECT_EQ(call(0xA0, 0xAB, 0x00), 1u);
    EXPECT_EQ(settle(), "SW:TMO HW:TMO");
}

TEST_F(CardBiosTest, CardInfoNewCardReportsNew) {
    insert(0);
    EXPECT_EQ(call(0xA0, 0xAB, 0x00), 1u);
    EXPECT_EQ(settle(), "SW:NEW HW:NEW");
}

TEST_F(CardBiosTest, CardInfoUsedCardReportsIoe) {
    insert(0);
    m.sio().clear_card_new(0);
    call(0xA0, 0xAB, 0x00);
    EXPECT_EQ(settle(), "SW:IOE HW:IOE");
}

TEST_F(CardBiosTest, CardInfoIsNotSignalledBeforeLatency) {
    insert(0);
    call(0xA0, 0xAB, 0x00);
    ctx().cycles += 19999;
    m.service_events();
    EXPECT_EQ(b0(0x0B, sw[3]), 0u);
    ctx().cycles += 1;
    m.service_events();
    EXPECT_EQ(b0(0x0B, sw[3]), 1u);
}

TEST_F(CardBiosTest, CardInfoSlotTwoPort) {
    insert(1);
    m.sio().clear_card_new(1);
    call(0xA0, 0xAB, 0x10);
    EXPECT_EQ(settle(), "SW:IOE HW:IOE");
    call(0xA0, 0xAB, 0x00);  // slot 1 empty
    EXPECT_EQ(settle(), "SW:TMO HW:TMO");
}

TEST_F(CardBiosTest, CardInfoUnknownPortTimesOut) {
    insert(0);
    call(0xA0, 0xAB, 0x20);
    EXPECT_EQ(settle(), "SW:TMO HW:TMO");
    call(0xA0, 0xAB, 0x01);  // multitap sub-port
    EXPECT_EQ(settle(), "SW:TMO HW:TMO");
}

TEST_F(CardBiosTest, SwCardEventIsDeliveredBeforeHwCard) {
    insert(0);
    std::string order;
    impl(1, [&](RecompContext*) { order += "S"; });
    impl(2, [&](RecompContext*) { order += "H"; });
    open_enabled(kClassSw, kSpecNew, Bios::kEvMdIntr, fn_addr(1));
    open_enabled(kClassHw, kSpecNew, Bios::kEvMdIntr, fn_addr(2));
    call(0xA0, 0xAB, 0x00);
    settle();
    EXPECT_EQ(order, "SH");
}

TEST_F(CardBiosTest, CardLoadNoCardTimesOut) {
    EXPECT_EQ(call(0xA0, 0xAC, 0x00), 1u);
    EXPECT_EQ(settle(), "SW:TMO HW:TMO");
}

TEST_F(CardBiosTest, CardLoadFormattedCardIsIoe) {
    insert(0);
    EXPECT_EQ(call(0xA0, 0xAC, 0x00), 1u);
    EXPECT_EQ(settle(), "SW:IOE HW:IOE");
}

TEST_F(CardBiosTest, CardLoadUnformattedCardIsError) {
    insert(0, false);
    call(0xA0, 0xAC, 0x00);
    EXPECT_EQ(settle(), "SW:ERR HW:ERR");
}

TEST_F(CardBiosTest, CardInfoDoesNotModifyTheCard) {
    insert(0);
    auto before = cards[0];
    call(0xA0, 0xAB, 0x00);
    call(0xA0, 0xAC, 0x00);
    EXPECT_EQ(cards[0], before);
    EXPECT_TRUE(writes.empty());
}

// ---- read/write sector ----

TEST_F(CardBiosTest, ReadSectorCopiesFrameToRam) {
    insert(0);
    auto d = pat(128, 9);
    std::copy(d.begin(), d.end(), cards[0].begin() + 77 * 128);
    EXPECT_EQ(b0(0x4F, 0x00, 77, kBuf), 1u);
    EXPECT_EQ(get(kBuf, 128), d);  // immediate, before the event
    EXPECT_EQ(settle(), "HW:IOE");
}

TEST_F(CardBiosTest, ReadSectorDoesNotTouchNeighbouringRam) {
    insert(0);
    put(kBuf - 4, std::vector<uint8_t>(136, 0xEE));
    b0(0x4F, 0x00, 5, kBuf);
    EXPECT_EQ(get(kBuf - 4, 4), std::vector<uint8_t>(4, 0xEE));
    EXPECT_EQ(get(kBuf + 128, 4), std::vector<uint8_t>(4, 0xEE));
}

TEST_F(CardBiosTest, ReadHeaderSectorShowsMc) {
    insert(0);
    b0(0x4F, 0x00, 0, kBuf);
    EXPECT_EQ(get(kBuf, 2), (std::vector<uint8_t>{'M', 'C'}));
}

TEST_F(CardBiosTest, ReadLastSectorIsValid) {
    insert(0);
    cards[0][1023 * 128 + 5] = 0x42;
    b0(0x4F, 0x00, 1023, kBuf);
    EXPECT_EQ(get(kBuf + 5, 1)[0], 0x42);
    EXPECT_EQ(settle(), "HW:IOE");
}

TEST_F(CardBiosTest, ReadSectorOutOfRangeIsError) {
    insert(0);
    put(kBuf, std::vector<uint8_t>(128, 0x11));
    EXPECT_EQ(b0(0x4F, 0x00, 1024, kBuf), 1u);
    EXPECT_EQ(get(kBuf, 128), std::vector<uint8_t>(128, 0x11));
    EXPECT_EQ(settle(), "HW:ERR");
}

TEST_F(CardBiosTest, ReadSectorNoCardTimesOut) {
    EXPECT_EQ(b0(0x4F, 0x00, 0, kBuf), 1u);
    EXPECT_EQ(settle(), "HW:TMO");
}

TEST_F(CardBiosTest, ReadSectorSlotTwo) {
    insert(1);
    cards[1][3 * 128] = 0x99;
    b0(0x4F, 0x10, 3, kBuf);
    EXPECT_EQ(get(kBuf, 1)[0], 0x99);
}

TEST_F(CardBiosTest, WriteSectorStoresRamData) {
    insert(0);
    auto d = pat(128, 3);
    put(kBuf, d);
    EXPECT_EQ(b0(0x4E, 0x00, 200, kBuf), 1u);
    EXPECT_TRUE(std::equal(d.begin(), d.end(), cards[0].begin() + 200 * 128));  // immediate
    EXPECT_EQ(settle(), "HW:IOE");
}

TEST_F(CardBiosTest, WriteSectorNeighboursUntouched) {
    insert(0);
    auto before = cards[0];
    put(kBuf, pat(128));
    b0(0x4E, 0x00, 200, kBuf);
    for (std::size_t i = 0; i < before.size(); ++i)
        if (i / 128 != 200) ASSERT_EQ(cards[0][i], before[i]) << i;
}

TEST_F(CardBiosTest, WriteSectorNotifiesAndClearsNewFlag) {
    insert(0);
    EXPECT_TRUE(m.sio().card_flag(0) & 0x08);
    b0(0x4E, 0x00, 0x3F, kBuf);
    EXPECT_EQ(writes, std::vector<int>{0});
    EXPECT_FALSE(m.sio().card_flag(0) & 0x08);
    settle();
    call(0xA0, 0xAB, 0x00);
    EXPECT_EQ(settle(), "SW:IOE HW:IOE");
}

TEST_F(CardBiosTest, WriteSectorSlotTwoNotifiesSlotTwo) {
    insert(1);
    b0(0x4E, 0x10, 1, kBuf);
    EXPECT_EQ(writes, std::vector<int>{1});
}

TEST_F(CardBiosTest, WriteSectorOutOfRangeIsErrorAndNoNotification) {
    insert(0);
    auto before = cards[0];
    b0(0x4E, 0x00, 1024, kBuf);
    EXPECT_EQ(settle(), "HW:ERR");
    EXPECT_EQ(cards[0], before);
    EXPECT_TRUE(writes.empty());
}

TEST_F(CardBiosTest, WriteSectorNoCardTimesOutWithoutNotification) {
    b0(0x4E, 0x00, 0, kBuf);
    EXPECT_EQ(settle(), "HW:TMO");
    EXPECT_TRUE(writes.empty());
}

TEST_F(CardBiosTest, WriteThenReadRoundTrip) {
    insert(0);
    auto d = pat(128, 21);
    put(kBuf, d);
    b0(0x4E, 0x00, 500, kBuf);
    b0(0x4F, 0x00, 500, kBuf + 0x400);
    EXPECT_EQ(get(kBuf + 0x400, 128), d);
}

TEST_F(CardBiosTest, EventsOnlyReachEnabledHandles) {
    insert(0);
    uint32_t extra = open_event(kClassSw, kSpecNew, Bios::kEvMdNoIntr);  // never enabled
    call(0xA0, 0xAB, 0x00);
    settle();
    EXPECT_EQ(b0(0x0B, extra), 0u);
}

TEST_F(CardBiosTest, NoOpCardInitFunctionsDoNotFail) {
    for (uint32_t fn : {0x4Au, 0x4Bu, 0x4Cu, 0x50u}) EXPECT_NO_THROW(b0(fn)) << fn;
    EXPECT_NO_THROW(call(0xA0, 0x70));
}

// ---- FileOpen / errno ----

TEST_F(CardBiosTest, OpenOnMissingCardFails) {
    EXPECT_EQ(open("bu00:FILE", kFread), kFail);
    EXPECT_EQ(err(), 0x13u);
}

TEST_F(CardBiosTest, OpenOnUnformattedCardFails) {
    insert(0, false);
    EXPECT_EQ(open("bu00:FILE", kFread), kFail);
    EXPECT_EQ(open("bu00:FILE", creat(1)), kFail);
}

TEST_F(CardBiosTest, OpenMissingFileSetsErrno2) {
    insert(0);
    EXPECT_EQ(open("bu00:NOPE", kFread), kFail);
    EXPECT_EQ(err(), 2u);
}

TEST_F(CardBiosTest, OpenNonCardDevicesFail) {
    insert(0);
    for (const char* p : {"cdrom:FILE", "bu01:FILE", "bu20:X", "bu00", "", "tty:"})
        EXPECT_EQ(open(p, kFread), kFail) << p;
}

TEST_F(CardBiosTest, CreateReturnsFirstFreeDescriptorAboveConsole) {
    insert(0);
    EXPECT_EQ(make("A", 1), 2u);
    EXPECT_EQ(make("B", 1), 3u);
}

TEST_F(CardBiosTest, CreateAllocatesBlocksFromBits16To31) {
    insert(0);
    ASSERT_EQ(make("BIG", 4), 2u);
    auto f = fs().find("BIG");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->blocks.size(), 4u);
    EXPECT_EQ(f->size, 4u * 8192);
}

TEST_F(CardBiosTest, CreateNotifiesAndFileAppearsInImage) {
    insert(0);
    make("SAVE", 1);
    EXPECT_FALSE(writes.empty());
    EXPECT_EQ(writes.front(), 0);
    EXPECT_EQ(cards[0][1 * 128], 0x51);
}

TEST_F(CardBiosTest, CreateExistingFileFails) {
    insert(0);
    make("A", 1);
    EXPECT_EQ(make("A", 1), kFail);
    EXPECT_EQ(err(), 0x11u);
}

TEST_F(CardBiosTest, CreateWithoutSpaceFails) {
    insert(0);
    make("A", 10);
    EXPECT_EQ(make("B", 6), kFail);
    EXPECT_EQ(err(), 0x1Cu);
}

TEST_F(CardBiosTest, CreateWithZeroBlocksFails) {
    insert(0);
    EXPECT_EQ(open("bu00:Z", kFcreat | kFwrite), kFail);
}

TEST_F(CardBiosTest, OpenExistingFile) {
    insert(0);
    uint32_t fd = make("A", 1);
    b0(0x36, fd);
    EXPECT_EQ(open("bu00:A", kFread), 2u);
}

TEST_F(CardBiosTest, OpenSlotTwo) {
    insert(1);
    EXPECT_EQ(open("bu10:S2", creat(1)), 2u);
    EXPECT_TRUE(fs(1).find("S2"));
    EXPECT_EQ(writes.front(), 1);
}

TEST_F(CardBiosTest, DescriptorTableExhausts) {
    insert(0);
    make("A", 1);
    for (int i = 0; i < 13; ++i) EXPECT_NE(open("bu00:A", kFread), kFail) << i;
    EXPECT_EQ(open("bu00:A", kFread), kFail);
}

TEST_F(CardBiosTest, CloseFreesDescriptorForReuse) {
    insert(0);
    uint32_t a = make("A", 1), b = make("B", 1);
    EXPECT_EQ(b0(0x36, a), a);
    EXPECT_EQ(open("bu00:B", kFread), a);
    (void)b;
}

TEST_F(CardBiosTest, CloseTwiceFails) {
    insert(0);
    uint32_t a = make("A", 1);
    b0(0x36, a);
    EXPECT_EQ(b0(0x36, a), kFail);
    EXPECT_EQ(err(), 9u);
}

TEST_F(CardBiosTest, AsyncOpenSignalsIoe) {
    insert(0);
    open("bu00:A", creat(1, kFasync | kFwrite));
    EXPECT_EQ(settle(), "HW:IOE");
}

TEST_F(CardBiosTest, SyncOpenSignalsNothing) {
    insert(0);
    make("A", 1);
    EXPECT_EQ(settle(), "");
}

TEST_F(CardBiosTest, FailedAsyncOpenSignalsNothing) {
    insert(0);
    open("bu00:NOPE", kFasync | kFread);
    EXPECT_EQ(settle(), "");
}

// ---- Read / write / seek ----

TEST_F(CardBiosTest, WriteThenReadBack) {
    insert(0);
    uint32_t fd = make("A", 1);
    auto d = pat(512, 4);
    put(kBuf, d);
    EXPECT_EQ(b0(0x35, fd, kBuf, 512), 512u);
    EXPECT_EQ(b0(0x33, fd, 0, 0), 0u);
    EXPECT_EQ(b0(0x34, fd, kBuf + 0x1000, 512), 512u);
    EXPECT_EQ(get(kBuf + 0x1000, 512), d);
}

TEST_F(CardBiosTest, WrittenDataLandsInFileBlock) {
    insert(0);
    make("X", 1);
    uint32_t fd = make("A", 1);
    auto d = pat(128, 8);
    put(kBuf, d);
    b0(0x35, fd, kBuf, 128);
    EXPECT_TRUE(std::equal(d.begin(), d.end(), cards[0].begin() + 2 * 8192));
}

TEST_F(CardBiosTest, PositionAdvancesAcrossCalls) {
    insert(0);
    uint32_t fd = make("A", 1);
    auto d = pat(256, 2);
    put(kBuf, d);
    b0(0x35, fd, kBuf, 128);
    b0(0x35, fd, kBuf + 128, 128);
    b0(0x33, fd, 0, 0);
    b0(0x34, fd, kBuf + 0x800, 256);
    EXPECT_EQ(get(kBuf + 0x800, 256), d);
}

TEST_F(CardBiosTest, ReadRequiresMultipleOf128) {
    insert(0);
    uint32_t fd = make("A", 1);
    EXPECT_EQ(b0(0x34, fd, kBuf, 100), kFail);
    EXPECT_EQ(b0(0x35, fd, kBuf, 129), kFail);
}

TEST_F(CardBiosTest, ReadOnlyDescriptorCannotWrite) {
    insert(0);
    make("A", 1);
    uint32_t fd = open("bu00:A", kFread);
    EXPECT_EQ(b0(0x35, fd, kBuf, 128), kFail);
    EXPECT_TRUE(std::all_of(cards[0].begin() + 8192, cards[0].begin() + 8192 + 128, [](uint8_t b) { return b == 0; }));
}

TEST_F(CardBiosTest, WriteOnlyDescriptorCannotRead) {
    insert(0);
    uint32_t fd = make("A", 1, kFwrite);
    EXPECT_EQ(b0(0x34, fd, kBuf, 128), kFail);
}

TEST_F(CardBiosTest, ReadAtEndOfFileReturnsZero) {
    insert(0);
    uint32_t fd = make("A", 1);
    b0(0x33, fd, 8192, 0);
    EXPECT_EQ(b0(0x34, fd, kBuf, 128), 0u);
}

TEST_F(CardBiosTest, ReadClampsAtEndOfFile) {
    insert(0);
    uint32_t fd = make("A", 1);
    b0(0x33, fd, 8192 - 128, 0);
    EXPECT_EQ(b0(0x34, fd, kBuf, 512), 128u);
}

TEST_F(CardBiosTest, ReadWriteOnInvalidDescriptors) {
    insert(0);
    for (uint32_t fd : {0u, 2u, 15u, 16u, 99u}) {
        EXPECT_EQ(b0(0x34, fd, kBuf, 128), kFail) << fd;
        EXPECT_EQ(b0(0x35, fd, kBuf, 128), kFail) << fd;
    }
}

TEST_F(CardBiosTest, WriteNotifiesAfterEachWrite) {
    insert(0);
    uint32_t fd = make("A", 1);
    writes.clear();
    b0(0x35, fd, kBuf, 128);
    b0(0x35, fd, kBuf, 128);
    EXPECT_EQ(writes, (std::vector<int>{0, 0}));
}

TEST_F(CardBiosTest, ReadDoesNotNotify) {
    insert(0);
    uint32_t fd = make("A", 1);
    writes.clear();
    b0(0x34, fd, kBuf, 128);
    EXPECT_TRUE(writes.empty());
}

TEST_F(CardBiosTest, MultiBlockFileWriteAcrossBlocks) {
    insert(0);
    uint32_t fd = make("A", 2);
    auto d = pat(16384, 6);
    for (std::size_t off = 0; off < d.size(); off += 4096) {
        put(kBuf, std::vector<uint8_t>(d.begin() + static_cast<std::ptrdiff_t>(off), d.begin() + static_cast<std::ptrdiff_t>(off + 4096)));
        ASSERT_EQ(b0(0x35, fd, kBuf, 4096), 4096u);
    }
    EXPECT_EQ(b0(0x33, fd, 0, 0), 0u);
    std::vector<uint8_t> all;
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(b0(0x34, fd, kBuf, 4096), 4096u);
        auto part = get(kBuf, 4096);
        all.insert(all.end(), part.begin(), part.end());
    }
    EXPECT_EQ(all, d);
}

TEST_F(CardBiosTest, SeekModes) {
    insert(0);
    uint32_t fd = make("A", 1);
    EXPECT_EQ(b0(0x33, fd, 256, 0), 256u);
    EXPECT_EQ(b0(0x33, fd, 128, 1), 384u);
    EXPECT_EQ(b0(0x33, fd, static_cast<uint32_t>(-128), 2), 8192u - 128);
    EXPECT_EQ(b0(0x33, fd, 0, 2), 8192u);
    EXPECT_EQ(b0(0x33, fd, static_cast<uint32_t>(-128), 1), 8192u - 128);
}

TEST_F(CardBiosTest, SeekOutOfRangeFailsAndKeepsPosition) {
    insert(0);
    uint32_t fd = make("A", 1);
    b0(0x33, fd, 256, 0);
    EXPECT_EQ(b0(0x33, fd, 8193, 0), kFail);
    EXPECT_EQ(b0(0x33, fd, static_cast<uint32_t>(-1), 0), kFail);
    EXPECT_EQ(b0(0x33, fd, 0, 7), kFail);
    EXPECT_EQ(b0(0x33, fd, 0, 1), 256u);
}

TEST_F(CardBiosTest, SeekInvalidDescriptor) {
    insert(0);
    EXPECT_EQ(b0(0x33, 5, 0, 0), kFail);
    EXPECT_EQ(b0(0x33, 1, 0, 0), kFail);
}

TEST_F(CardBiosTest, AsyncReadAndWriteSignalIoe) {
    insert(0);
    uint32_t fd = make("A", 1, kFwrite | kFread | kFasync);
    settle();
    EXPECT_EQ(b0(0x35, fd, kBuf, 128), 0u);  // queued: returns 0, completion comes as an event
    EXPECT_EQ(settle(), "SW:IOE HW:IOE");
    b0(0x33, fd, 0, 0);
    EXPECT_EQ(b0(0x34, fd, kBuf, 128), 0u);
    EXPECT_EQ(settle(), "SW:IOE HW:IOE");
}

TEST_F(CardBiosTest, AsyncHandleTransfersEvenWithReadOnlyAccess) {
    insert(0);
    make("A", 1);
    uint32_t fd = open("bu00:A", kFread | kFasync);
    settle();
    put(kBuf, pat(128, 9));
    EXPECT_EQ(b0(0x35, fd, kBuf, 128), 0u);
    EXPECT_EQ(settle(), "SW:IOE HW:IOE");
    EXPECT_EQ(std::vector<uint8_t>(cards[0].begin() + 8192, cards[0].begin() + 8192 + 128), pat(128, 9));
}

TEST_F(CardBiosTest, SyncReadAndWriteSignalNothing) {
    insert(0);
    uint32_t fd = make("A", 1);
    b0(0x35, fd, kBuf, 128);
    b0(0x34, fd, kBuf, 128);
    EXPECT_EQ(settle(), "");
}

TEST_F(CardBiosTest, ConsoleWritesStillGoToTty) {
    insert(0);
    std::string line;
    bios.on_tty = [&](const std::string& s) { line = s; };
    put(kBuf, {'h', 'i', '\n'});
    EXPECT_EQ(b0(0x35, 1, kBuf, 3), 3u);
    EXPECT_EQ(line, "hi");
}

// ---- firstfile / nextfile ----

TEST_F(CardBiosTest, FirstFileFillsDirentry) {
    insert(0);
    make("SAVE1", 2);
    str(kPath, "bu00:*");
    for (uint32_t i = 0; i < 0x28; i += 4) dw3_write32(&ctx(), kEntry + i, 0xDDDDDDDD);
    EXPECT_EQ(b0(0x42, kPath, kEntry), kEntry);
    EXPECT_EQ(get(kEntry, 6), (std::vector<uint8_t>{'S', 'A', 'V', 'E', '1', 0}));
    EXPECT_EQ(get(kEntry + 6, 14), std::vector<uint8_t>(14, 0));
    EXPECT_EQ(peek(kEntry + 0x14), 0x50u);
    EXPECT_EQ(peek(kEntry + 0x18), 16384u);
    EXPECT_EQ(peek(kEntry + 0x1C), 0u);
    EXPECT_EQ(peek(kEntry + 0x20), 64u);
    EXPECT_EQ(peek(kEntry + 0x24), 0u);
}

TEST_F(CardBiosTest, FirstFileOnEmptyCardReturnsZero) {
    insert(0);
    str(kPath, "bu00:*");
    EXPECT_EQ(b0(0x42, kPath, kEntry), 0u);
}

TEST_F(CardBiosTest, FirstFileWithoutNameMatchesEverything) {
    insert(0);
    make("A", 1);
    str(kPath, "bu00:");
    EXPECT_EQ(b0(0x42, kPath, kEntry), kEntry);
}

TEST_F(CardBiosTest, NextFileIteratesInDirectoryOrder) {
    insert(0);
    make("ONE", 1);
    make("TWO", 2);
    make("THREE", 1);
    str(kPath, "bu00:*");
    std::vector<std::string> names;
    uint32_t r = b0(0x42, kPath, kEntry);
    while (r) {
        names.push_back(std::string(reinterpret_cast<const char*>(get(kEntry, 21).data())));
        r = b0(0x43, kEntry);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"ONE", "TWO", "THREE"}));
}

TEST_F(CardBiosTest, NextFileAfterLastKeepsReturningZero) {
    insert(0);
    make("A", 1);
    str(kPath, "bu00:*");
    b0(0x42, kPath, kEntry);
    EXPECT_EQ(b0(0x43, kEntry), 0u);
    EXPECT_EQ(b0(0x43, kEntry), 0u);
}

TEST_F(CardBiosTest, NextFileWithoutFirstFileReturnsZero) {
    insert(0);
    make("A", 1);
    EXPECT_EQ(b0(0x43, kEntry), 0u);
}

TEST_F(CardBiosTest, ChainedFileListedOnce) {
    insert(0);
    make("CH", 3);
    make("Z", 1);
    str(kPath, "bu00:*");
    int n = 0;
    for (uint32_t r = b0(0x42, kPath, kEntry); r; r = b0(0x43, kEntry)) ++n;
    EXPECT_EQ(n, 2);
}

struct WildCase {
    const char* pattern;
    std::vector<std::string> expect;
};

class WildcardTest : public CardBiosTest, public ::testing::WithParamInterface<WildCase> {};

TEST_P(WildcardTest, MatchesExpectedNames) {
    insert(0);
    for (const char* n : {"BASLUS-01436DW3", "BASLUS-01436DW4", "BESLES-00001X", "NOTES"}) make(n, 1);
    str(kPath, std::string("bu00:") + GetParam().pattern);
    std::vector<std::string> got;
    for (uint32_t r = b0(0x42, kPath, kEntry); r; r = b0(0x43, kEntry))
        got.push_back(std::string(reinterpret_cast<const char*>(get(kEntry, 21).data())));
    EXPECT_EQ(got, GetParam().expect);
}

INSTANTIATE_TEST_SUITE_P(
    Patterns, WildcardTest,
    ::testing::Values(WildCase{"*", {"BASLUS-01436DW3", "BASLUS-01436DW4", "BESLES-00001X", "NOTES"}},
                      WildCase{"BASLUS*", {"BASLUS-01436DW3", "BASLUS-01436DW4"}},
                      WildCase{"BASLUS-01436DW?", {"BASLUS-01436DW3", "BASLUS-01436DW4"}},
                      WildCase{"BASLUS-01436DW3", {"BASLUS-01436DW3"}},
                      WildCase{"B?SLES*", {"BESLES-00001X"}},
                      WildCase{"*X", {"BESLES-00001X"}},
                      WildCase{"N?TES", {"NOTES"}},
                      WildCase{"NOTE", {}},
                      WildCase{"NOTES?", {}},
                      WildCase{"??????", {}},
                      WildCase{"?????", {"NOTES"}},
                      WildCase{"*-*", {"BASLUS-01436DW3", "BASLUS-01436DW4", "BESLES-00001X"}},
                      WildCase{"nothing", {}}));

TEST_F(CardBiosTest, FirstFileOnSlotTwo) {
    insert(1);
    open("bu10:TWO", creat(1));
    str(kPath, "bu10:*");
    EXPECT_EQ(b0(0x42, kPath, kEntry), kEntry);
    str(kPath, "bu00:*");
    EXPECT_EQ(b0(0x42, kPath, kEntry), 0u);
}

TEST_F(CardBiosTest, DeletedFilesAreNotListed) {
    insert(0);
    make("A", 1);
    make("B", 1);
    str(kPath, "bu00:A");
    EXPECT_EQ(b0(0x45, kPath), 1u);
    str(kPath, "bu00:*");
    EXPECT_EQ(b0(0x42, kPath, kEntry), kEntry);
    EXPECT_EQ(get(kEntry, 2)[0], 'B');
    EXPECT_EQ(b0(0x43, kEntry), 0u);
}

// ---- erase / rename / format ----

TEST_F(CardBiosTest, EraseRemovesFileAndNotifies) {
    insert(0);
    make("A", 2);
    writes.clear();
    str(kPath, "bu00:A");
    EXPECT_EQ(b0(0x45, kPath), 1u);
    EXPECT_FALSE(fs().find("A"));
    EXPECT_EQ(fs().free_blocks(), 15);
    EXPECT_EQ(writes, std::vector<int>{0});
}

TEST_F(CardBiosTest, EraseMissingFileFails) {
    insert(0);
    str(kPath, "bu00:A");
    writes.clear();
    EXPECT_EQ(b0(0x45, kPath), 0u);
    EXPECT_EQ(err(), 2u);
    EXPECT_TRUE(writes.empty());
}

TEST_F(CardBiosTest, EraseWithoutCardFails) {
    str(kPath, "bu00:A");
    EXPECT_EQ(b0(0x45, kPath), 0u);
}

TEST_F(CardBiosTest, ErasedSpaceCanBeReallocated) {
    insert(0);
    make("A", 15);
    EXPECT_EQ(make("B", 1), kFail);
    str(kPath, "bu00:A");
    b0(0x45, kPath);
    EXPECT_NE(make("B", 15), kFail);
}

TEST_F(CardBiosTest, RenameChangesName) {
    insert(0);
    make("OLD", 1);
    writes.clear();
    str(kPath, "bu00:OLD");
    str(kPath2, "bu00:NEW");
    EXPECT_EQ(b0(0x44, kPath, kPath2), 1u);
    EXPECT_FALSE(fs().find("OLD"));
    EXPECT_TRUE(fs().find("NEW"));
    EXPECT_EQ(writes, std::vector<int>{0});
}

TEST_F(CardBiosTest, RenameFailures) {
    insert(0);
    make("A", 1);
    make("B", 1);
    str(kPath, "bu00:A");
    str(kPath2, "bu00:B");
    EXPECT_EQ(b0(0x44, kPath, kPath2), 0u);
    EXPECT_EQ(err(), 0x11u);
    str(kPath, "bu00:MISSING");
    EXPECT_EQ(b0(0x44, kPath, kPath2), 0u);
    str(kPath, "bu00:A");
    str(kPath2, "bu10:C");
    EXPECT_EQ(b0(0x44, kPath, kPath2), 0u);
    EXPECT_TRUE(fs().find("A"));
}

TEST_F(CardBiosTest, RenameKeepsOpenDescriptorValid) {
    insert(0);
    uint32_t fd = make("OLD", 1);
    str(kPath, "bu00:OLD");
    str(kPath2, "bu00:NEW");
    b0(0x44, kPath, kPath2);
    EXPECT_EQ(b0(0x35, fd, kBuf, 128), 128u);
}

TEST_F(CardBiosTest, RenamePreservesData) {
    insert(0);
    uint32_t fd = make("OLD", 1);
    auto d = pat(128, 5);
    put(kBuf, d);
    b0(0x35, fd, kBuf, 128);
    b0(0x36, fd);
    str(kPath, "bu00:OLD");
    str(kPath2, "bu00:NEW");
    b0(0x44, kPath, kPath2);
    uint32_t nfd = open("bu00:NEW", kFread);
    b0(0x34, nfd, kBuf + 0x400, 128);
    EXPECT_EQ(get(kBuf + 0x400, 128), d);
}

TEST_F(CardBiosTest, FormatErasesEverything) {
    insert(0);
    make("A", 3);
    make("B", 1);
    writes.clear();
    str(kPath, "bu00:");
    EXPECT_EQ(b0(0x41, kPath), 1u);
    EXPECT_TRUE(fs().list().empty());
    EXPECT_EQ(fs().free_blocks(), 15);
    EXPECT_EQ(writes, std::vector<int>{0});
}

TEST_F(CardBiosTest, FormatInitializesBlankCard) {
    insert(0, false);
    str(kPath, "bu00:");
    EXPECT_EQ(b0(0x41, kPath), 1u);
    EXPECT_TRUE(mc::is_formatted(cards[0]));
    EXPECT_EQ(make("A", 1), 2u);
}

TEST_F(CardBiosTest, FormatInvalidatesOpenDescriptors) {
    insert(0);
    uint32_t fd = make("A", 1);
    str(kPath, "bu00:");
    b0(0x41, kPath);
    EXPECT_EQ(b0(0x35, fd, kBuf, 128), kFail);
}

TEST_F(CardBiosTest, FormatOnlyAffectsNamedSlot) {
    insert(0);
    insert(1);
    open("bu10:KEEP", creat(1));
    str(kPath, "bu00:");
    b0(0x41, kPath);
    EXPECT_TRUE(fs(1).find("KEEP"));
}

TEST_F(CardBiosTest, FormatWithoutCardFails) {
    str(kPath, "bu00:");
    EXPECT_EQ(b0(0x41, kPath), 0u);
    str(kPath, "cdrom:");
    EXPECT_EQ(b0(0x41, kPath), 0u);
}

TEST_F(CardBiosTest, GetErrnoDefaultsToZeroAndClearsOnSuccess) {
    insert(0);
    EXPECT_EQ(err(), 0u);
    open("bu00:NOPE", kFread);
    EXPECT_NE(err(), 0u);
    make("A", 1);
    EXPECT_EQ(err(), 0u);
}

// ---- Cross-check against the SIO0 memory card protocol ----

struct SioRig : dw3test::Clock {
    dw3::IrqController irq{bus};
    dw3::Sio sio{bus, sched, irq};
    SioRig() {
        w16(0x1F80104E, 0x88);
        w16(0x1F801048, 0x000D);
    }
    uint8_t xfer(uint8_t tx) {
        w8(0x1F801040, tx);
        run_to(ctx().cycles + 8 * 0x88 + dw3::Sio::kAckDelay + dw3::Sio::kAckWidth + 1);
        uint8_t rx = static_cast<uint8_t>(bus.read(0x1F801040, 1));
        w16(0x1F80104A, r16(0x1F80104A) | 0x10);
        w32(0x1F801070, ~0x80u);
        return rx;
    }
    std::vector<uint8_t> read_sector(uint32_t s, int port = 0) {
        w16(0x1F80104A, 0x1003 | (port ? 0x2000u : 0u));
        std::vector<uint8_t> tx = {0x81, 0x52, 0, 0, uint8_t(s >> 8), uint8_t(s)};
        tx.resize(10 + 128 + 2, 0);
        std::vector<uint8_t> rx;
        for (auto b : tx) rx.push_back(xfer(b));
        w16(0x1F80104A, 0x1000 | (port ? 0x2000u : 0u));
        return std::vector<uint8_t>(rx.begin() + 10, rx.begin() + 138);
    }
};

TEST_F(CardBiosTest, FileWrittenViaBiosIsReadableOverSio) {
    insert(0);
    uint32_t fd = make("XCHECK", 2);
    auto d = pat(1024, 17);
    put(kBuf, d);
    ASSERT_EQ(b0(0x35, fd, kBuf, 1024), 1024u);
    SioRig rig;
    ASSERT_TRUE(rig.sio.attach_card(0, cards[0]));
    auto f = *fs().find("XCHECK");
    uint32_t base = f.blocks.front() * 64u;
    for (uint32_t i = 0; i < 8; ++i) {
        auto sec = rig.read_sector(base + i);
        EXPECT_EQ(sec, std::vector<uint8_t>(d.begin() + i * 128, d.begin() + (i + 1) * 128)) << i;
    }
}

TEST_F(CardBiosTest, DirectoryFrameWrittenViaBiosIsReadableOverSio) {
    insert(0);
    make("DIRCHECK", 1);
    SioRig rig;
    ASSERT_TRUE(rig.sio.attach_card(0, cards[0]));
    auto e = rig.read_sector(1);
    EXPECT_EQ(e[0], 0x51);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(&e[10])), "DIRCHECK");
    EXPECT_TRUE(mc::frame_checksum_ok(e));
}

TEST_F(CardBiosTest, SectorWrittenViaBiosIsReadableOverSio) {
    insert(0);
    auto d = pat(128, 40);
    put(kBuf, d);
    b0(0x4E, 0x00, 700, kBuf);
    SioRig rig;
    ASSERT_TRUE(rig.sio.attach_card(0, cards[0]));
    EXPECT_EQ(rig.read_sector(700), d);
}

TEST_F(CardBiosTest, SectorWrittenOverSioIsReadableViaBios) {
    insert(0);
    SioRig rig;
    ASSERT_TRUE(rig.sio.attach_card(0, cards[0]));
    EXPECT_EQ(rig.sio.card_data(0).data(), cards[0].data());
    cards[0][900 * 128 + 3] = 0x5A;  // same storage: visible to both views
    b0(0x4F, 0x00, 900, kBuf);
    EXPECT_EQ(get(kBuf + 3, 1)[0], 0x5A);
    EXPECT_EQ(rig.read_sector(900)[3], 0x5A);
}

TEST_F(CardBiosTest, BiosFileLayerSeesCardPreparedByOtherCode) {
    insert(0);
    mc::CardFs prep(cards[0]);
    prep.create("PREPARED", 1);
    auto d = pat(256, 33);
    prep.write(*prep.find("PREPARED"), 0, d);
    uint32_t fd = open("bu00:PREPARED", kFread);
    ASSERT_EQ(fd, 2u);
    EXPECT_EQ(b0(0x34, fd, kBuf, 256), 256u);
    EXPECT_EQ(get(kBuf, 256), d);
}

// ---- Kernel device table ----

constexpr uint32_t kBuDcb = Bios::kDcbBase + 2 * Bios::kDcbSize;

TEST_F(CardBiosTest, DeviceTableIsBuiltInKernelRam) {
    EXPECT_EQ(peek(0x150), Bios::kDcbBase);
    EXPECT_EQ(peek(0x154), 3 * 0x50u);
    EXPECT_EQ(peek(0x140), Bios::kFcbBase);
    EXPECT_EQ(peek(0x144), Bios::kFcbCount * Bios::kFcbSize);
    EXPECT_EQ(get(peek(kBuDcb), 3), (std::vector<uint8_t>{'b', 'u', 0}));
    EXPECT_EQ(get(peek(Bios::kDcbBase + 0x50), 6), (std::vector<uint8_t>{'c', 'd', 'r', 'o', 'm', 0}));
    EXPECT_EQ(peek(kBuDcb + 0x08), 0x80u);  // sector size
    EXPECT_EQ(peek(kBuDcb + 0x34), Bios::native_addr(Bios::kNativeBuFirst));
    EXPECT_EQ(peek(kBuDcb + 0x38), Bios::native_addr(Bios::kNativeBuNext));
    EXPECT_NO_THROW(m.lookup(peek(kBuDcb + 0x34)));
    EXPECT_NO_THROW(m.lookup(peek(kBuDcb + 0x10)));  // unimplemented handlers still resolve
}

// Stands in for the game's libcard wrapper (0x8003D404): records its arguments, restores the original
// DCB pointer and forwards to it.
struct FirstFileSpy {
    uint32_t a0 = 0, a1 = 0, a2 = 0;
    std::string name;
    int calls = 0;
};

TEST_F(CardBiosTest, FirstFileDispatchesThroughPatchedDcbPointer) {
    insert(0);
    make("SAVE1", 1);
    FirstFileSpy spy;
    uint32_t orig = peek(kBuDcb + 0x34);
    impl(1, [&](RecompContext* c) {
        ++spy.calls;
        spy.a0 = c->r[4];
        spy.a1 = c->r[5];
        spy.a2 = c->r[6];
        for (uint32_t i = 0; i < 16 && dw3_read8(c, spy.a1 + i); ++i) spy.name += static_cast<char>(dw3_read8(c, spy.a1 + i));
        dw3_write32(c, kBuDcb + 0x34, orig);
        dw3_call(c, orig);
    });
    poke(kBuDcb + 0x34, fn_addr(1));
    str(kPath, "bu00:SAVE*");
    EXPECT_EQ(b0(0x42, kPath, kEntry), kEntry);
    EXPECT_EQ(spy.calls, 1);
    EXPECT_EQ(spy.a0, Bios::kFcbBase);
    EXPECT_EQ(spy.a1, kPath + 5);
    EXPECT_EQ(spy.name, "SAVE*");
    EXPECT_EQ(spy.a2, kEntry);
    EXPECT_EQ(peek(kBuDcb + 0x34), orig);
    EXPECT_EQ(get(kEntry, 6), (std::vector<uint8_t>{'S', 'A', 'V', 'E', '1', 0}));
    EXPECT_EQ(b0(0x42, kPath, kEntry), kEntry);  // pointer restored: spy not hit again
    EXPECT_EQ(spy.calls, 1);
}

TEST_F(CardBiosTest, FirstFileSelectsPortFromPath) {
    insert(1);
    str(kPath, "bu10:*");
    b0(0x42, kPath, kEntry);
    EXPECT_EQ(peek(Bios::kFcbBase + 4), 0x10u);
    EXPECT_EQ(settle(), "HW:IOE");
}

TEST_F(CardBiosTest, FirstFileCompletesWithHwCardEventOnly) {
    insert(0);
    make("A", 1);
    str(kPath, "bu00:*");
    b0(0x42, kPath, kEntry);
    EXPECT_EQ(b0(0x0B, hw[0]), 0u);  // asynchronous: nothing yet
    EXPECT_EQ(settle(), "HW:IOE");
}

TEST_F(CardBiosTest, FirstFileNoCardTimesOut) {
    str(kPath, "bu00:*");
    EXPECT_EQ(b0(0x42, kPath, kEntry), 0u);
    EXPECT_EQ(settle(), "HW:TMO");
}

TEST_F(CardBiosTest, FirstFileUnformattedCardIsError) {
    insert(0, false);
    str(kPath, "bu00:*");
    EXPECT_EQ(b0(0x42, kPath, kEntry), 0u);
    EXPECT_EQ(settle(), "HW:ERR");
}

TEST_F(CardBiosTest, NextFileGoesThroughDcbAndSignalsHwCard) {
    insert(0);
    make("ONE", 1);
    make("TWO", 1);
    str(kPath, "bu00:*");
    ASSERT_EQ(b0(0x42, kPath, kEntry), kEntry);
    settle();
    int calls = 0;
    uint32_t a0 = 0, a1 = 0;
    uint32_t orig = peek(kBuDcb + 0x38);
    impl(2, [&](RecompContext* c) {
        ++calls;
        a0 = c->r[4];
        a1 = c->r[5];
        dw3_call(c, orig);
    });
    poke(kBuDcb + 0x38, fn_addr(2));
    EXPECT_EQ(b0(0x43, kEntry), kEntry);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(a0, Bios::kFcbBase);
    EXPECT_EQ(a1, kEntry);
    EXPECT_EQ(get(kEntry, 4), (std::vector<uint8_t>{'T', 'W', 'O', 0}));
    EXPECT_EQ(settle(), "HW:IOE");
    EXPECT_EQ(b0(0x43, kEntry), 0u);  // exhausted
}

TEST_F(CardBiosTest, FirstFileOnUnknownDeviceFails) {
    str(kPath, "xx00:*");
    EXPECT_EQ(b0(0x42, kPath, kEntry), 0u);
    EXPECT_EQ(settle(), "");
}

TEST_F(CardBiosTest, CardInfoReportsSwAndHw) {
    insert(0);
    m.sio().clear_card_new(0);
    call(0xA0, 0xAB, 0);
    EXPECT_EQ(settle(), "SW:IOE HW:IOE");
}

TEST_F(CardBiosTest, SectorReadReportsHwOnly) {
    insert(0);
    call(0xB0, 0x4F, 0, 1, kBuf);
    EXPECT_EQ(settle(), "HW:IOE");
}

}  // namespace
