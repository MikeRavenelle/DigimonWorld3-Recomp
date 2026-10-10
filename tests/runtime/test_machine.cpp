// Machine wiring: scheduler-driven VBLANK, interrupt delivery gating, DMA -> overlay invalidation, lookup.

#include <gtest/gtest.h>

#include <stdexcept>

#include "dw3/machine.h"

namespace {

int g_calls_a = 0, g_calls_b = 0, g_calls_main = 0;
void fn_main(RecompContext*) { ++g_calls_main; }
void fn_a(RecompContext*) { ++g_calls_a; }
void fn_b(RecompContext*) { ++g_calls_b; }

constexpr uint32_t kSlot = 0x80082448;
const RecompFuncEntry kMainFuncs[] = {{0x80010000, fn_main}};
const RecompFuncEntry kAFuncs[] = {{kSlot + 0x10, fn_a}};
const RecompFuncEntry kBFuncs[] = {{kSlot + 0x10, fn_b}};
const RecompProbe kAProbes[] = {{kSlot + 0x10, 0x27BDFFE8}};  // addiu sp,sp,-24
const RecompProbe kBProbes[] = {{kSlot + 0x10, 0x27BDFFD0}};  // addiu sp,sp,-48
const RecompSegment kSegs[] = {
    {"MAIN", 0x80010000, 0x1000, 0, kMainFuncs, 1, nullptr, 0},
    {"OVA", kSlot, 0x100, 0, kAFuncs, 1, kAProbes, 1},
    {"OVB", kSlot, 0x100, 0, kBFuncs, 1, kBProbes, 1},
};

struct Throwing {
    Throwing() { dw3::set_fatal_handler([](const std::string& m) { throw std::runtime_error(m); }); }
    ~Throwing() { dw3::set_fatal_handler(nullptr); }
};

void poke32(dw3::Machine& m, uint32_t addr, uint32_t v) { dw3_write32(&m.ctx(), addr, v); }

void enable_cpu_irq(dw3::Machine& m) { dw3_mtc0(&m.ctx(), 12, (1u << 10) | 1u); }  // IM2 + IEc

}  // namespace

TEST(Machine, VblankIsScheduledAtConstruction) {
    dw3::Machine m(kSegs);
    EXPECT_EQ(m.ctx().next_event, m.video().vblank_cycle(0));
}

TEST(Machine, ServiceEventsRaisesVblankIrq) {
    dw3::Machine m(kSegs);
    m.ctx().cycles = m.video().vblank_cycle(0);
    dw3_service_events(&m.ctx());
    EXPECT_TRUE(m.irq().stat() & 1u);
    EXPECT_EQ(m.video().frame_count(), 1u);
    EXPECT_GT(m.ctx().next_event, m.ctx().cycles);
}

TEST(Machine, LineRaiseSetsCauseIp2AndRequestsService) {
    dw3::Machine m(kSegs);
    poke32(m, 0x1F801074, 1);  // I_MASK: vblank
    m.ctx().cycles = 100;
    m.irq().raise(dw3::IrqSource::Vblank);
    EXPECT_TRUE(m.ctx().cop0[13] & (1u << 10));
    EXPECT_EQ(m.ctx().next_event, 100);
}

TEST(Machine, InterruptNotDeliveredWhileDisabled) {
    dw3::Machine m(kSegs);
    int delivered = 0;
    m.on_interrupt = [&](RecompContext&) { ++delivered; };
    poke32(m, 0x1F801074, 1);
    m.irq().raise(dw3::IrqSource::Vblank);
    dw3_service_events(&m.ctx());
    EXPECT_EQ(delivered, 0);
}

TEST(Machine, InterruptDeliveredWhenEnabledAndMasked) {
    dw3::Machine m(kSegs);
    int delivered = 0;
    m.on_interrupt = [&](RecompContext&) { ++delivered; };
    enable_cpu_irq(m);
    poke32(m, 0x1F801074, 1);
    m.irq().raise(dw3::IrqSource::Vblank);
    dw3_service_events(&m.ctx());
    EXPECT_EQ(delivered, 1);
}

TEST(Machine, InterruptMaskedByIMask) {
    dw3::Machine m(kSegs);
    int delivered = 0;
    m.on_interrupt = [&](RecompContext&) { ++delivered; };
    enable_cpu_irq(m);
    m.irq().raise(dw3::IrqSource::Vblank);  // I_MASK = 0
    dw3_service_events(&m.ctx());
    EXPECT_EQ(delivered, 0);
}

TEST(Machine, EnablingSrRequestsServiceForPendingLine) {
    dw3::Machine m(kSegs);
    poke32(m, 0x1F801074, 1);
    m.irq().raise(dw3::IrqSource::Vblank);
    m.ctx().cycles = 5000;
    m.ctx().next_event = 999999;
    enable_cpu_irq(m);
    EXPECT_EQ(m.ctx().next_event, 5000);
}

TEST(Machine, RfeRequestsService) {
    dw3::Machine m(kSegs);
    m.ctx().cycles = 77;
    m.ctx().next_event = 999999;
    m.ctx().cop0[12] = 0x4;  // IEp set: rfe moves it to IEc
    dw3_rfe(&m.ctx());
    EXPECT_EQ(m.ctx().cop0[12] & 1u, 1u);
    EXPECT_EQ(m.ctx().next_event, 77);
}

TEST(Machine, ServiceOverrideReplacesDefault) {
    dw3::Machine m(kSegs);
    int calls = 0;
    m.on_service_events = [&](RecompContext&) { ++calls; };
    m.ctx().cycles = m.video().vblank_cycle(0);
    dw3_service_events(&m.ctx());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(m.video().frame_count(), 0u);
}

TEST(Machine, LookupMainFunction) {
    dw3::Machine m(kSegs);
    g_calls_main = 0;
    dw3_call(&m.ctx(), 0x80010000);
    dw3_call(&m.ctx(), 0xA0010000);  // KSEG1 alias
    EXPECT_EQ(g_calls_main, 2);
}

TEST(Machine, LookupIdentifiesOverlayByProbe) {
    dw3::Machine m(kSegs);
    g_calls_a = g_calls_b = 0;
    poke32(m, kSlot + 0x10, 0x27BDFFE8);
    dw3_call(&m.ctx(), kSlot + 0x10);
    EXPECT_EQ(g_calls_a, 1);
    poke32(m, kSlot + 0x10, 0x27BDFFD0);  // B loaded over A (CPU write: no invalidate)
    m.invalidate(kSlot, 0x100);
    dw3_call(&m.ctx(), kSlot + 0x10);
    EXPECT_EQ(g_calls_b, 1);
    EXPECT_EQ(m.identifications(), 2u);
}

TEST(Machine, DmaRamWriteInvalidatesOverlay) {
    dw3::Machine m(kSegs);
    g_calls_a = g_calls_b = 0;
    poke32(m, kSlot + 0x10, 0x27BDFFE8);
    dw3_call(&m.ctx(), kSlot + 0x10);
    ASSERT_EQ(g_calls_a, 1);
    // Swap the code, then run an OTC DMA over the slot: on_ram_written must invalidate the overlay.
    poke32(m, kSlot + 0x10, 0x27BDFFD0);
    poke32(m, 0x1F8010F0, 1u << 27);                    // DPCR: enable channel 6
    poke32(m, 0x1F8010E0, (kSlot + 0x8) & 0x00FFFFFF);  // MADR (table top below the probe)
    poke32(m, 0x1F8010E4, 1);                           // BCR: 1 entry
    poke32(m, 0x1F8010E8, 0x11000002);                  // CHCR: start + trigger, decrement
    EXPECT_EQ(dw3_read32(&m.ctx(), kSlot + 0x10), 0x27BDFFD0u);  // OTC did not touch the probe word
    dw3_call(&m.ctx(), kSlot + 0x10);
    EXPECT_EQ(g_calls_b, 1);
}

TEST(Machine, LookupMissIsFatal) {
    Throwing t;
    dw3::Machine m(kSegs);
    EXPECT_THROW(dw3_lookup_func(&m.ctx(), 0x80010004), std::runtime_error);
    EXPECT_THROW(dw3_lookup_func(&m.ctx(), 0x80150000), std::runtime_error);
}

TEST(Machine, UnknownOverlayContentIsFatal) {
    Throwing t;
    dw3::Machine m(kSegs);
    poke32(m, kSlot + 0x10, 0x12345678);  // neither A nor B
    EXPECT_THROW(dw3_lookup_func(&m.ctx(), kSlot + 0x10), std::runtime_error);
}

TEST(Machine, BiosCallWithoutHandlerIsFatal) {
    Throwing t;
    dw3::Machine m(kSegs);
    EXPECT_THROW(dw3_bios_call(&m.ctx(), 0xA0, 0x39), std::runtime_error);
}

TEST(Machine, BiosCallDispatchesToHandler) {
    dw3::Machine m(kSegs);
    uint32_t got_t = 0, got_f = 0;
    m.on_bios_call = [&](RecompContext&, uint32_t t, uint32_t f) { got_t = t; got_f = f; };
    dw3_bios_call(&m.ctx(), 0xB0, 0x17);
    EXPECT_EQ(got_t, 0xB0u);
    EXPECT_EQ(got_f, 0x17u);
}

TEST(Machine, GteBridge) {
    dw3::Machine m(kSegs);
    dw3_gte_write_data(&m.ctx(), 0, 0x00020001);
    EXPECT_EQ(dw3_gte_read_data(&m.ctx(), 0), 0x00020001u);
    int64_t before = m.ctx().cycles;
    dw3_gte_cmd(&m.ctx(), 0x0A00428);  // SQR
    EXPECT_GT(m.ctx().cycles, before);
}
