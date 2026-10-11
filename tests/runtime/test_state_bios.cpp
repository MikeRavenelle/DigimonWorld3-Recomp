// Native BIOS save state: kernel tables, interrupt chains, pad setup, open card files and pending card completions.

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "dw3/bios.h"
#include "dw3/machine.h"
#include "dw3/memcard.h"
#include "dw3/state.h"

namespace {

using dw3::Bios;
using dw3::StateError;
using dw3::StateIo;
namespace mc = dw3::memcard;

constexpr uint32_t kFnBase = 0x80010000;
std::function<void(RecompContext*)> g_impl[2];
void slot0(RecompContext* c) { if (g_impl[0]) g_impl[0](c); }
void slot1(RecompContext* c) { if (g_impl[1]) g_impl[1](c); }
const RecompFuncEntry kMainFuncs[] = {{kFnBase, slot0}, {kFnBase + 0x100, slot1}};
const RecompSegment kSegs[] = {{"MAIN", kFnBase, 0x10000, 0, kMainFuncs, 2, nullptr, 0}};

constexpr uint32_t kIMask = 0x1F801074;
constexpr uint32_t kSrIen = (1u << 10) | 1u;
constexpr uint32_t kClassSw = 0xF4000001, kClassHw = 0xF0000011;
constexpr uint32_t kData = 0x80100000, kPath = 0x80101000, kEntry = 0x80103000;
constexpr uint32_t kFread = 1, kFwrite = 2, kFcreat = 0x200;

// One machine plus its BIOS and two card images.
struct Rig {
    Rig() {
        for (auto& c : cards) c.assign(mc::kCardSize, 0);
        mc::format_memory_card(std::span<uint8_t, mc::kCardSize>(cards[0].data(), cards[0].size()));
        m.sio().attach_card(0, cards[0]);
    }
    RecompContext& ctx() { return m.ctx(); }
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
    uint32_t open_enabled(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func = 0) {
        uint32_t h = b0(0x08, cls, spec, mode, func);
        b0(0x0C, h);
        return h;
    }
    void str(uint32_t addr, const std::string& s) {
        for (std::size_t i = 0; i <= s.size(); ++i)
            dw3_write8(&ctx(), addr + static_cast<uint32_t>(i), static_cast<uint8_t>(i < s.size() ? s[i] : 0));
    }
    uint32_t fopen(const std::string& path, uint32_t mode) {
        str(kPath, path);
        return b0(0x32, kPath, mode);
    }
    void fire_vblank() {
        ctx().cop0[12] = kSrIen;
        m.bus().write(kIMask, 1, 4);
        m.irq().raise(dw3::IrqSource::Vblank);
        m.service_events();
    }

    std::vector<uint8_t> save() {
        StateIo w;
        m.scheduler().io(w);
        bios.io(w);
        return w.take();
    }
    void load(const std::vector<uint8_t>& d) {
        StateIo r{std::span<const uint8_t>(d)};
        m.scheduler().io(r);
        bios.io(r);
        EXPECT_TRUE(r.at_end());
    }
    // Brings RAM, time and the card image up to date with `o` (these belong to other state blocks).
    void adopt(Rig& o) {
        std::memcpy(m.bus().ram(), o.m.bus().ram(), DW3_RAM_SIZE);
        ctx().cycles = o.ctx().cycles;
        cards[0] = o.cards[0];
        cards[1] = o.cards[1];
        m.sio().attach_card(0, cards[0]);
    }

    std::array<std::vector<uint8_t>, 2> cards;
    dw3::Machine m{kSegs};
    Bios bios{m};
};

class StateBiosTest : public ::testing::Test {
protected:
    StateBiosTest() {
        for (auto& f : g_impl) f = nullptr;
        dw3::set_fatal_handler([](const std::string& s) { throw std::runtime_error(s); });
    }
    ~StateBiosTest() override {
        for (auto& f : g_impl) f = nullptr;
        dw3::set_fatal_handler(nullptr);
    }
    // Copies `a` into a fresh rig through a save state.
    std::unique_ptr<Rig> clone(Rig& a) {
        auto b = std::make_unique<Rig>();
        auto data = a.save();
        b->adopt(a);
        b->load(data);
        return b;
    }
    // Lets the card latency elapse on `r` and reports which of the given handles signalled.
    static std::string settle(Rig& r, const std::vector<uint32_t>& handles) {
        r.ctx().cycles += 20000;
        r.m.service_events();
        std::string s;
        for (uint32_t h : handles) s += r.b0(0x0B, h) ? '1' : '0';
        return s;
    }
};

TEST_F(StateBiosTest, EventsSurviveTheRoundTrip) {
    Rig a;
    uint32_t e0 = a.b0(0x08, 0xF2000000, 2, Bios::kEvMdIntr, 0x80010100);
    uint32_t e1 = a.open_enabled(kClassHw, 4, Bios::kEvMdNoIntr);
    uint32_t e2 = a.b0(0x08, kClassSw, 8, Bios::kEvMdNoIntr, 0);
    a.b0(0x09, e2);
    a.b0(0x07, kClassHw, 4);  // delivered: e1 becomes "already"
    auto b = clone(a);
    for (int i = 0; i < Bios::kMaxEvents; ++i) {
        EXPECT_EQ(b->bios.event(i).cls, a.bios.event(i).cls) << i;
        EXPECT_EQ(b->bios.event(i).spec, a.bios.event(i).spec) << i;
        EXPECT_EQ(b->bios.event(i).mode, a.bios.event(i).mode) << i;
        EXPECT_EQ(b->bios.event(i).func, a.bios.event(i).func) << i;
        EXPECT_EQ(b->bios.event(i).status, a.bios.event(i).status) << i;
    }
    EXPECT_EQ(b->bios.event(0).status, Bios::kEvStWait);
    EXPECT_EQ(b->bios.event(1).status, Bios::kEvStAlready);
    EXPECT_EQ(b->bios.event(2).status, 0u);
    uint64_t calls = b->bios.calls();
    EXPECT_EQ(b->b0(0x0B, e1), 1u);  // TestEvent consumes the delivered signal
    EXPECT_EQ(b->b0(0x0C, e0), 1u);
    EXPECT_EQ(b->b0(0x09, e2), 0u);  // closed before the save
    EXPECT_EQ(b->bios.calls(), calls + 3);
}

TEST_F(StateBiosTest, InterruptChainsAndHookSurvive) {
    Rig a;
    constexpr uint32_t kNodeA = kData, kNodeB = kData + 0x40;
    for (uint32_t node : {kNodeA, kNodeB}) {
        dw3_write32(&a.ctx(), node + 4, 0);
        dw3_write32(&a.ctx(), node + 8, kFnBase);
        a.c0(0x02, 1, node);
    }
    a.b0(0x19, kData + 0x100);  // HookEntryInt
    auto b = clone(a);
    EXPECT_EQ(b->bios.hook_entry(), kData + 0x100);

    int calls = 0;
    g_impl[0] = [&](RecompContext*) { ++calls; };
    b->b0(0x18);  // drop the hook so the interrupt path ends on its own
    EXPECT_EQ(b->bios.hook_entry(), 0u);
    b->fire_vblank();
    EXPECT_EQ(calls, 2);  // both nodes on the restored priority-1 chain ran
    EXPECT_EQ(b->bios.interrupts_delivered(), 1u);
}

TEST_F(StateBiosTest, PadSetupSurvives) {
    Rig a;
    EXPECT_EQ(a.b0(0x12, kData, 34, kData + 0x40, 34), 2u);
    a.b0(0x13);
    auto b = clone(a);
    b->bios.pads[0].buttons = 0xFFFE;
    b->fire_vblank();
    EXPECT_EQ(dw3_read8(&b->ctx(), kData + 0), 0x00u);
    EXPECT_EQ(dw3_read8(&b->ctx(), kData + 1), 0x41u);
    EXPECT_EQ(dw3_read8(&b->ctx(), kData + 2), 0xFEu);
    EXPECT_EQ(dw3_read8(&b->ctx(), kData + 3), 0xFFu);
}

TEST_F(StateBiosTest, OutdatedPadDestinationSurvives) {
    Rig a;
    a.b0(0x15, 0, kData + 0x80);
    auto b = clone(a);
    b->bios.pads[0].buttons = 0xFFFE;
    b->bios.pads[1].buttons = 0xFFFF;
    b->fire_vblank();
    EXPECT_EQ(dw3_read32(&b->ctx(), kData + 0x80), 1u);
}

TEST_F(StateBiosTest, PendingCardCompletionFiresIdentically) {
    Rig a;
    std::vector<uint32_t> hs;
    for (uint32_t cls : {kClassSw, kClassHw}) hs.push_back(a.open_enabled(cls, 0x0004, Bios::kEvMdNoIntr));
    a.ctx().cycles = 1000;
    EXPECT_EQ(a.call(0xA0, 0xAC, 0), 1u);  // _card_load: SwCARD then HwCARD, 20000 cycles out
    EXPECT_EQ(a.b0(0x4F, 0, 3, kData), 1u);
    a.ctx().cycles = 5000;
    a.m.service_events();
    auto b = clone(a);
    EXPECT_EQ(b->m.scheduler().pending_count(), 2u);

    for (Rig* r : {&a, b.get()}) {
        r->ctx().cycles = 20999;
        r->m.service_events();
        EXPECT_EQ(r->b0(0x0B, hs[0]), 0u);
        EXPECT_EQ(r->b0(0x0B, hs[1]), 0u);
        r->ctx().cycles = 21000;
        r->m.service_events();
        EXPECT_EQ(r->b0(0x0B, hs[0]), 1u);
        EXPECT_EQ(r->b0(0x0B, hs[1]), 1u);
    }
}

TEST_F(StateBiosTest, CompletionRunsIntrCallbackAfterRestore) {
    Rig a;
    a.open_enabled(kClassHw, 0x0004, Bios::kEvMdIntr, kFnBase + 0x100);
    a.b0(0x4F, 0, 1, kData);
    auto b = clone(a);
    int hits = 0;
    g_impl[1] = [&](RecompContext*) { ++hits; };
    settle(*b, {});
    EXPECT_EQ(hits, 1);
}

TEST_F(StateBiosTest, SeveralPendingCompletionsKeepTheirOrder) {
    Rig a;
    std::vector<uint32_t> hs;
    for (uint32_t spec : {0x0004u, 0x8000u}) hs.push_back(a.open_enabled(kClassHw, spec, Bios::kEvMdNoIntr));
    a.b0(0x4F, 0, 1, kData);
    a.ctx().cycles += 10;
    a.b0(0x4F, 0, 0x7FFF, kData);  // out of range: error completion
    auto b = clone(a);
    EXPECT_EQ(settle(*b, hs), "11");
    EXPECT_EQ(settle(a, hs), "11");
}

TEST_F(StateBiosTest, OpenFilesAndSearchSurvive) {
    Rig a;
    uint32_t fd = a.fopen("bu00:SAVE1", (2u << 16) | kFcreat | kFwrite | kFread);
    ASSERT_GE(fd, 2u);
    std::vector<uint8_t> pattern(256);
    for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<uint8_t>(i * 7 + 1);
    for (std::size_t i = 0; i < pattern.size(); ++i) dw3_write8(&a.ctx(), kData + static_cast<uint32_t>(i), pattern[i]);
    EXPECT_EQ(a.b0(0x35, fd, kData, 256), 256u);
    a.fopen("bu00:SAVE2", (1u << 16) | kFcreat | kFwrite);
    a.b0(0x33, fd, 100, 0);
    a.str(kPath, "bu00:SAVE*");
    ASSERT_EQ(a.b0(0x42, kPath, kEntry), kEntry);  // first match; the next one is still to come

    auto b = clone(a);
    EXPECT_EQ(b->b0(0x33, fd, 0, 1), 100u);  // position survived: relative seek returns it
    b->b0(0x33, fd, 0, 0);
    EXPECT_EQ(b->b0(0x34, fd, kData + 0x400, 256), 256u);
    for (std::size_t i = 0; i < pattern.size(); ++i)
        ASSERT_EQ(dw3_read8(&b->ctx(), kData + 0x400 + static_cast<uint32_t>(i)), pattern[i]) << i;
    EXPECT_EQ(b->b0(0x43, kEntry), kEntry);  // search continues with the second file
    EXPECT_EQ(dw3_read8(&b->ctx(), kEntry + 4), '2');
    EXPECT_EQ(b->b0(0x43, kEntry), 0u);
    EXPECT_EQ(b->b0(0x36, fd), fd);
    EXPECT_EQ(b->b0(0x36, fd), 0xFFFFFFFFu);  // closed once
    EXPECT_EQ(b->b0(0x54), 9u);               // errno set by the bad close
}

TEST_F(StateBiosTest, TtyPartialLineSurvives) {
    Rig a;
    a.str(kData, "hel");
    a.b0(0x35, 1, kData, 3);
    auto b = clone(a);
    std::vector<std::string> lines;
    b->bios.on_tty = [&](const std::string& s) { lines.push_back(s); };
    b->str(kData, "lo\n");
    b->b0(0x35, 1, kData, 3);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "hello");
}

TEST_F(StateBiosTest, SaveLoadSaveIsIdentical) {
    Rig a;
    a.open_enabled(kClassHw, 4, Bios::kEvMdNoIntr);
    a.c0(0x02, 2, kData);
    a.b0(0x19, kData + 0x100);
    a.b0(0x12, kData, 34, kData + 0x40, 34);
    a.b0(0x13);
    a.fopen("bu00:ROUND", (1u << 16) | kFcreat | kFwrite);
    a.str(kData + 0x200, "partial");
    a.b0(0x35, 1, kData + 0x200, 7);
    a.b0(0x4F, 0, 2, kData);
    auto first = a.save();
    Rig b;
    b.adopt(a);
    b.load(first);
    EXPECT_EQ(b.save(), first);
}

TEST_F(StateBiosTest, TruncatedDataThrows) {
    Rig a;
    a.open_enabled(kClassHw, 4, Bios::kEvMdNoIntr);
    a.b0(0x4F, 0, 2, kData);
    auto data = a.save();
    for (std::size_t n : {std::size_t{0}, std::size_t{3}, data.size() / 2, data.size() - 1}) {
        std::vector<uint8_t> cut(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(n));
        Rig b;
        EXPECT_THROW(b.load(cut), StateError) << n;
    }
}

TEST_F(StateBiosTest, HostDepthCountsActiveKernelFrames) {
    Rig a;
    EXPECT_EQ(a.bios.host_depth(), 0);
    a.open_enabled(kClassHw, 4, Bios::kEvMdIntr, kFnBase);
    int seen = -1;
    bool threw = false;
    g_impl[0] = [&](RecompContext*) {
        seen = a.bios.host_depth();
        StateIo w;
        try {
            a.bios.io(w);
        } catch (const StateError&) {
            threw = true;
        }
    };
    a.b0(0x07, kClassHw, 4);
    EXPECT_EQ(seen, 1);
    EXPECT_TRUE(threw);
    EXPECT_EQ(a.bios.host_depth(), 0);
    StateIo w;
    EXPECT_NO_THROW(a.bios.io(w));
}

TEST_F(StateBiosTest, SavingFromAnInterruptChainIsRefused) {
    Rig a;
    dw3_write32(&a.ctx(), kData + 4, 0);
    dw3_write32(&a.ctx(), kData + 8, kFnBase);
    a.c0(0x02, 0, kData);
    int depth = -1;
    bool threw = false;
    g_impl[0] = [&](RecompContext*) {
        depth = a.bios.host_depth();
        StateIo w;
        try {
            a.bios.io(w);
        } catch (const StateError&) {
            threw = true;
        }
    };
    a.fire_vblank();
    EXPECT_GE(depth, 1);
    EXPECT_TRUE(threw);
    EXPECT_EQ(a.bios.host_depth(), 0);
}

}  // namespace
