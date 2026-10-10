#include <gtest/gtest.h>

#include "dw3/bus.h"

using dw3::Bus;

TEST(Phys, SegmentStripping) {
    EXPECT_EQ(dw3_phys(0x80010000u), 0x00010000u);  // KSEG0
    EXPECT_EQ(dw3_phys(0xA0010000u), 0x00010000u);  // KSEG1
    EXPECT_EQ(dw3_phys(0x00010000u), 0x00010000u);  // KUSEG
    EXPECT_EQ(dw3_phys(0x1F801810u), 0x1F801810u);
    EXPECT_EQ(dw3_phys(0xBFC00000u), 0x1FC00000u);  // BIOS via KSEG1
    EXPECT_EQ(dw3_phys(0xFFFE0130u), 0xFFFE0130u);  // KSEG2 cache control untouched
}

TEST(Bus, RamReadWriteAllSizes) {
    Bus bus;
    auto* c = &bus.ctx();
    dw3_write32(c, 0x80001000u, 0xDEADBEEFu);
    EXPECT_EQ(dw3_read32(c, 0x80001000u), 0xDEADBEEFu);
    EXPECT_EQ(dw3_read16(c, 0x80001000u), 0xBEEFu);
    EXPECT_EQ(dw3_read16(c, 0x80001002u), 0xDEADu);
    EXPECT_EQ(dw3_read8(c, 0x80001003u), 0xDEu);
    dw3_write8(c, 0x80001001u, 0x11);
    EXPECT_EQ(dw3_read32(c, 0x80001000u), 0xDEAD11EFu);
    dw3_write16(c, 0x80001002u, 0x2233);
    EXPECT_EQ(dw3_read32(c, 0x80001000u), 0x223311EFu);
}

TEST(Bus, RamMirrorsEvery2MiB) {
    Bus bus;
    auto* c = &bus.ctx();
    dw3_write32(c, 0x80000100u, 0x12345678u);
    EXPECT_EQ(dw3_read32(c, 0x80200100u), 0x12345678u);
    EXPECT_EQ(dw3_read32(c, 0x80600100u), 0x12345678u);
    EXPECT_EQ(dw3_read32(c, 0xA0000100u), 0x12345678u);
    EXPECT_EQ(dw3_read32(c, 0x00000100u), 0x12345678u);
}

TEST(Bus, Scratchpad) {
    Bus bus;
    auto* c = &bus.ctx();
    dw3_write32(c, 0x1F800010u, 0xCAFEBABEu);
    EXPECT_EQ(dw3_read32(c, 0x1F800010u), 0xCAFEBABEu);
    EXPECT_EQ(dw3_read32(c, 0x9F800010u), 0xCAFEBABEu);  // KSEG0 alias
    EXPECT_EQ(dw3_read32(c, 0x00000010u), 0u);           // does not alias RAM
}

TEST(Bus, IoDispatch) {
    Bus bus;
    uint32_t last_off = 0, last_val = 0;
    int last_size = 0;
    bus.map_io({0x1F801810u, 8,
                [](uint32_t off, int) { return 0x1000u + off; },
                [&](uint32_t off, uint32_t v, int sz) { last_off = off; last_val = v; last_size = sz; }});
    auto* c = &bus.ctx();
    EXPECT_EQ(dw3_read32(c, 0x1F801814u), 0x1004u);
    dw3_write32(c, 0x1F801810u, 0xE1000000u);
    EXPECT_EQ(last_off, 0u);
    EXPECT_EQ(last_val, 0xE1000000u);
    EXPECT_EQ(last_size, 4);
    dw3_write16(c, 0xBF801816u, 0xABCD);  // KSEG1 alias reaches the same handler
    EXPECT_EQ(last_off, 6u);
    EXPECT_EQ(last_size, 2);
}

TEST(Bus, UnmappedAccessCounted) {
    Bus bus;
    auto* c = &bus.ctx();
    EXPECT_EQ(dw3_read32(c, 0x1F802000u), 0u);
    dw3_write32(c, 0x1F802000u, 1);
    EXPECT_EQ(bus.unmapped_reads(), 1u);
    EXPECT_EQ(bus.unmapped_writes(), 1u);
}
