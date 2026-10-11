// Native BIOS: exe loading, events, interrupt path (chains, root counters, pads, custom exit), syscalls, A0/B0/C0.

#include <gtest/gtest.h>

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

TEST_F(BiosTest, FileOpenSeekReadFail) {
    for (uint32_t fn : {0x32u, 0x33u, 0x34u}) EXPECT_EQ(b0(fn, 1, 2, 3), 0xFFFFFFFFu) << fn;
}

TEST_F(BiosTest, FileCloseEchoesDescriptor) {
    EXPECT_EQ(b0(0x36, 5), 5u);
}

TEST_F(BiosTest, FirstAndNextFileReturnNone) {
    ctx().r[2] = 0x123;
    EXPECT_EQ(b0(0x42, kData, kData), 0u);
    ctx().r[2] = 0x123;
    EXPECT_EQ(b0(0x43, kData), 0u);
}

struct CardCase {
    uint32_t table, fn;
};

class CardTest : public BiosTest, public ::testing::WithParamInterface<CardCase> {};

TEST_P(CardTest, ReturnsOneAndCompletesAsynchronously) {
    uint32_t sw = open_enabled(kClassSw, 0x100, Bios::kEvMdNoIntr);
    uint32_t hw = open_enabled(kClassHw, 0x100, Bios::kEvMdNoIntr);
    EXPECT_EQ(call(GetParam().table, GetParam().fn), 1u);
    m.service_events();
    EXPECT_EQ(b0(0x0B, sw), 0u);  // not yet
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
    uint32_t sw = open_enabled(kClassSw, 0x100, Bios::kEvMdNoIntr);
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
    open_enabled(kClassSw, 0x100, Bios::kEvMdIntr, fn_addr(5));
    call(GetParam().table, GetParam().fn);
    ctx().cycles = 20000;
    fill_regs(ctx());
    m.service_events();
    for (int i = 1; i < 32; ++i) EXPECT_EQ(ctx().r[i], 0x1000u + static_cast<uint32_t>(i)) << i;
}

TEST_P(CardTest, SeveralOutstandingRequestsEachFire) {
    int n = 0;
    impl(5, [&](RecompContext*) { ++n; });
    open_enabled(kClassSw, 0x100, Bios::kEvMdIntr, fn_addr(5));
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
