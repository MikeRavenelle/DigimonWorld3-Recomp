// GTE (COP2) tests: register quirks, every command, saturation / flag behaviour, divider.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "dw3/gte.h"

namespace {

// Data registers.
enum D : unsigned {
    VXY0 = 0, VZ0, VXY1, VZ1, VXY2, VZ2, RGBC, OTZ, IR0, IR1, IR2, IR3, SXY0, SXY1, SXY2, SXYP,
    SZ0, SZ1, SZ2, SZ3, RGB0, RGB1, RGB2, RES1, MAC0, MAC1, MAC2, MAC3, IRGB, ORGB, LZCS, LZCR
};
// Control registers (index within the control file).
enum C : unsigned {
    RT0 = 0, RT1, RT2, RT3, RT4, TRX, TRY, TRZ, L0, L1, L2, L3, L4, RBK, GBK, BBK,
    LC0, LC1, LC2, LC3, LC4, RFC, GFC, BFC, OFX, OFY, H, DQA, DQB, ZSF3, ZSF4, FLAG
};

// Command opcodes.
enum Op : uint32_t {
    RTPS = 0x01, NCLIP = 0x06, OP = 0x0C, DPCS = 0x10, INTPL = 0x11, MVMVA = 0x12, NCDS = 0x13, CDP = 0x14,
    NCDT = 0x16, NCCS = 0x1B, CC = 0x1C, NCS = 0x1E, NCT = 0x20, SQR = 0x28, DCPL = 0x29, DPCT = 0x2A,
    AVSZ3 = 0x2D, AVSZ4 = 0x2E, RTPT = 0x30, GPF = 0x3D, GPL = 0x3E, NCCT = 0x3F
};

constexpr uint32_t pk(int x, int y) { return (static_cast<uint32_t>(x) & 0xFFFFu) | (static_cast<uint32_t>(y) << 16); }
constexpr uint32_t cmd(uint32_t op, int sf = 0, int lm = 0) {
    return 0x4A000000u | op | (static_cast<uint32_t>(sf) << 19) | (static_cast<uint32_t>(lm) << 10);
}
constexpr uint32_t mv(int mx, int vx, int cv, int sf = 0, int lm = 0) {
    return cmd(MVMVA, sf, lm) | (static_cast<uint32_t>(mx) << 17) | (static_cast<uint32_t>(vx) << 15) |
           (static_cast<uint32_t>(cv) << 13);
}

constexpr uint32_t F_SUM = 0x80000000u;
constexpr uint32_t F_MAC1P = 1u << 30, F_MAC2P = 1u << 29, F_MAC3P = 1u << 28;
constexpr uint32_t F_MAC1N = 1u << 27, F_MAC2N = 1u << 26, F_MAC3N = 1u << 25;
constexpr uint32_t F_IR1 = 1u << 24, F_IR2 = 1u << 23, F_IR3 = 1u << 22;
constexpr uint32_t F_CR = 1u << 21, F_CG = 1u << 20, F_CB = 1u << 19;
constexpr uint32_t F_SZ = 1u << 18, F_DIV = 1u << 17, F_MAC0P = 1u << 16, F_MAC0N = 1u << 15;
constexpr uint32_t F_SX = 1u << 14, F_SY = 1u << 13, F_IR0 = 1u << 12;

class GteTest : public ::testing::Test {
protected:
    dw3::Gte g;

    uint32_t d(unsigned r) const { return g.read_data(r); }
    int32_t sd(unsigned r) const { return static_cast<int32_t>(g.read_data(r)); }
    uint32_t c(unsigned r) const { return g.read_ctrl(r); }
    uint32_t flag() const { return g.read_ctrl(FLAG); }
    void wd(unsigned r, uint32_t v) { g.write_data(r, v); }
    void wc(unsigned r, uint32_t v) { g.write_ctrl(r, v); }
    void run(uint32_t command) { g.execute(command); }

    void vec(int n, int x, int y, int z) {
        wd(static_cast<unsigned>(n) * 2, pk(x, y));
        wd(static_cast<unsigned>(n) * 2 + 1, static_cast<uint32_t>(z));
    }
    void ir(int a, int b, int cc, int i0 = 0) {
        wd(IR0, static_cast<uint32_t>(i0));
        wd(IR1, static_cast<uint32_t>(a));
        wd(IR2, static_cast<uint32_t>(b));
        wd(IR3, static_cast<uint32_t>(cc));
    }
    void mat(unsigned base, int s) {  // diagonal matrix, value s
        wc(base, pk(s, 0));
        wc(base + 1, pk(0, 0));
        wc(base + 2, pk(s, 0));
        wc(base + 3, pk(0, 0));
        wc(base + 4, static_cast<uint32_t>(s));
    }
    void rot_id() { mat(RT0, 0x1000); }
    void light_id() {
        mat(L0, 0x1000);
        mat(LC0, 0x1000);
    }
    void tr(int x, int y, int z) {
        wc(TRX, static_cast<uint32_t>(x));
        wc(TRY, static_cast<uint32_t>(y));
        wc(TRZ, static_cast<uint32_t>(z));
    }
    void fc(int x, int y, int z) {
        wc(RFC, static_cast<uint32_t>(x));
        wc(GFC, static_cast<uint32_t>(y));
        wc(BFC, static_cast<uint32_t>(z));
    }
    void bk(int x, int y, int z) {
        wc(RBK, static_cast<uint32_t>(x));
        wc(GBK, static_cast<uint32_t>(y));
        wc(BBK, static_cast<uint32_t>(z));
    }
    void color(uint32_t r, uint32_t gg, uint32_t b, uint32_t code = 0) { wd(RGBC, r | gg << 8 | b << 16 | code << 24); }
    // Projection setup with H=0x1000 so sz3=0x1000 gives a divider result of exactly 0x10000.
    void proj_setup() {
        rot_id();
        wc(H, 0x1000);
    }
};

// ---------------------------------------------------------------- data register quirks

TEST_F(GteTest, VxyRoundTrip) {
    wd(VXY0, 0x80017FFFu);
    EXPECT_EQ(d(VXY0), 0x80017FFFu);
    wd(VXY1, 0x12345678u);
    EXPECT_EQ(d(VXY1), 0x12345678u);
    wd(VXY2, 0xFFFFFFFFu);
    EXPECT_EQ(d(VXY2), 0xFFFFFFFFu);
}

TEST_F(GteTest, VzSignExtends) {
    wd(VZ0, 0x8000u);
    EXPECT_EQ(d(VZ0), 0xFFFF8000u);
    wd(VZ1, 0x7FFFu);
    EXPECT_EQ(d(VZ1), 0x7FFFu);
    wd(VZ2, 0xFFFFu);
    EXPECT_EQ(d(VZ2), 0xFFFFFFFFu);
}

TEST_F(GteTest, VzIgnoresUpperBits) {
    wd(VZ0, 0x12345678u);
    EXPECT_EQ(d(VZ0), 0x5678u);
    wd(VZ1, 0xABCD8001u);
    EXPECT_EQ(d(VZ1), 0xFFFF8001u);
}

TEST_F(GteTest, RgbcRoundTrip) {
    wd(RGBC, 0xDEADBEEFu);
    EXPECT_EQ(d(RGBC), 0xDEADBEEFu);
}

TEST_F(GteTest, OtzIsUnsigned16) {
    wd(OTZ, 0xFFFFFFFFu);
    EXPECT_EQ(d(OTZ), 0xFFFFu);
    wd(OTZ, 0x12348000u);
    EXPECT_EQ(d(OTZ), 0x8000u);
}

TEST_F(GteTest, IrSignExtendsOnWrite) {
    wd(IR0, 0xFFFFu);
    EXPECT_EQ(d(IR0), 0xFFFFFFFFu);
    wd(IR1, 0x8000u);
    EXPECT_EQ(d(IR1), 0xFFFF8000u);
    wd(IR2, 0x12347FFFu);
    EXPECT_EQ(d(IR2), 0x7FFFu);
    wd(IR3, 0x00010001u);
    EXPECT_EQ(d(IR3), 1u);
}

TEST_F(GteTest, SxypReadsSxy2) {
    wd(SXY2, 0x11223344u);
    EXPECT_EQ(d(SXYP), 0x11223344u);
}

TEST_F(GteTest, SxypWritePushesFifo) {
    wd(SXYP, 0x00010002u);
    wd(SXYP, 0x00030004u);
    wd(SXYP, 0x00050006u);
    EXPECT_EQ(d(SXY0), 0x00010002u);
    EXPECT_EQ(d(SXY1), 0x00030004u);
    EXPECT_EQ(d(SXY2), 0x00050006u);
    wd(SXYP, 0x00070008u);
    EXPECT_EQ(d(SXY0), 0x00030004u);
    EXPECT_EQ(d(SXY1), 0x00050006u);
    EXPECT_EQ(d(SXY2), 0x00070008u);
}

TEST_F(GteTest, SxyDirectWriteDoesNotPush) {
    wd(SXY0, 1);
    wd(SXY1, 2);
    wd(SXY2, 3);
    wd(SXY1, 9);
    EXPECT_EQ(d(SXY0), 1u);
    EXPECT_EQ(d(SXY1), 9u);
    EXPECT_EQ(d(SXY2), 3u);
}

TEST_F(GteTest, SzRegistersAreUnsigned16) {
    for (unsigned r = SZ0; r <= SZ3; ++r) {
        wd(r, 0xFFFF8123u);
        EXPECT_EQ(d(r), 0x8123u);
    }
}

TEST_F(GteTest, RgbFifoRegistersRoundTrip) {
    wd(RGB0, 0x01020304u);
    wd(RGB1, 0x05060708u);
    wd(RGB2, 0x090A0B0Cu);
    EXPECT_EQ(d(RGB0), 0x01020304u);
    EXPECT_EQ(d(RGB1), 0x05060708u);
    EXPECT_EQ(d(RGB2), 0x090A0B0Cu);
}

TEST_F(GteTest, Res1RoundTrip) {
    wd(RES1, 0xCAFEBABEu);
    EXPECT_EQ(d(RES1), 0xCAFEBABEu);
}

TEST_F(GteTest, MacRegistersRoundTrip) {
    wd(MAC0, 0x80000000u);
    wd(MAC1, 0xFFFFFFFFu);
    wd(MAC2, 0x12345678u);
    wd(MAC3, 0x00000001u);
    EXPECT_EQ(d(MAC0), 0x80000000u);
    EXPECT_EQ(d(MAC1), 0xFFFFFFFFu);
    EXPECT_EQ(d(MAC2), 0x12345678u);
    EXPECT_EQ(d(MAC3), 1u);
}

TEST_F(GteTest, IrgbWriteSetsIr) {
    wd(IRGB, 0x7FFFu);
    EXPECT_EQ(d(IR1), 0xF80u);
    EXPECT_EQ(d(IR2), 0xF80u);
    EXPECT_EQ(d(IR3), 0xF80u);
}

TEST_F(GteTest, IrgbWriteChannelSeparation) {
    wd(IRGB, 0x1Fu);
    EXPECT_EQ(d(IR1), 0xF80u);
    EXPECT_EQ(d(IR2), 0u);
    EXPECT_EQ(d(IR3), 0u);
    wd(IRGB, 5u << 5);
    EXPECT_EQ(d(IR1), 0u);
    EXPECT_EQ(d(IR2), 5u * 0x80);
    wd(IRGB, 3u << 10);
    EXPECT_EQ(d(IR3), 3u * 0x80);
}

TEST_F(GteTest, IrgbWriteIgnoresUpperBits) {
    wd(IRGB, 0xFFFF8000u);
    EXPECT_EQ(d(IR1), 0u);
    EXPECT_EQ(d(IR2), 0u);
    EXPECT_EQ(d(IR3), 0u);
}

TEST_F(GteTest, OrgbConvertsIr) {
    ir(0x400, 0xF80, -1);
    EXPECT_EQ(d(ORGB), 0x3E8u);
    EXPECT_EQ(d(IRGB), 0x3E8u);
}

TEST_F(GteTest, OrgbSaturatesHigh) {
    ir(0x7FFF, 0x1000, 0x0F80);
    EXPECT_EQ(d(ORGB), 0x7FFFu);
}

TEST_F(GteTest, OrgbSaturatesNegativeToZero) {
    ir(-0x8000, -1, -0x100);
    EXPECT_EQ(d(ORGB), 0u);
}

TEST_F(GteTest, OrgbWriteIgnored) {
    ir(0x80, 0, 0);
    wd(ORGB, 0x7FFF);
    EXPECT_EQ(d(ORGB), 1u);
    EXPECT_EQ(d(IR1), 0x80u);
}

TEST_F(GteTest, LzcsZeroGivesThirtyTwo) {
    wd(LZCS, 0);
    EXPECT_EQ(d(LZCR), 32u);
}

TEST_F(GteTest, LzcsMinusOneGivesThirtyTwo) {
    wd(LZCS, 0xFFFFFFFFu);
    EXPECT_EQ(d(LZCR), 32u);
}

TEST_F(GteTest, LzcsPositiveCountsLeadingZeros) {
    wd(LZCS, 1);
    EXPECT_EQ(d(LZCR), 31u);
    wd(LZCS, 0x7FFFFFFFu);
    EXPECT_EQ(d(LZCR), 1u);
    wd(LZCS, 0x00010000u);
    EXPECT_EQ(d(LZCR), 15u);
    wd(LZCS, 0x00FFFFFFu);
    EXPECT_EQ(d(LZCR), 8u);
}

TEST_F(GteTest, LzcsNegativeCountsLeadingOnes) {
    wd(LZCS, 0x80000000u);
    EXPECT_EQ(d(LZCR), 1u);
    wd(LZCS, 0xFFFF0000u);
    EXPECT_EQ(d(LZCR), 16u);
    wd(LZCS, 0xFFFFFFFEu);
    EXPECT_EQ(d(LZCR), 31u);
    wd(LZCS, 0xC0000000u);
    EXPECT_EQ(d(LZCR), 2u);
}

TEST_F(GteTest, LzcrIsReadOnly) {
    wd(LZCS, 0);
    wd(LZCR, 5);
    EXPECT_EQ(d(LZCR), 32u);
}

TEST_F(GteTest, LzcsReadsBack) {
    wd(LZCS, 0xDEADBEEFu);
    EXPECT_EQ(d(LZCS), 0xDEADBEEFu);
}

TEST_F(GteTest, DataRegisterIndexMasked) {
    wd(32 + RGBC, 0x11223344u);
    EXPECT_EQ(d(RGBC), 0x11223344u);
    EXPECT_EQ(g.read_data(32 + RGBC), 0x11223344u);
}

// ---------------------------------------------------------------- control register quirks

TEST_F(GteTest, PackedMatrixRegistersAreRaw) {
    wc(RT0, 0x80017FFFu);
    wc(RT1, 0x12345678u);
    wc(RT2, 0xFFFF0001u);
    wc(RT3, 0x00008000u);
    EXPECT_EQ(c(RT0), 0x80017FFFu);
    EXPECT_EQ(c(RT1), 0x12345678u);
    EXPECT_EQ(c(RT2), 0xFFFF0001u);
    EXPECT_EQ(c(RT3), 0x00008000u);
}

TEST_F(GteTest, Rt33SignExtends) {
    wc(RT4, 0x8000u);
    EXPECT_EQ(c(RT4), 0xFFFF8000u);
    wc(RT4, 0x1234u);
    EXPECT_EQ(c(RT4), 0x1234u);
}

TEST_F(GteTest, L33SignExtends) {
    wc(L4, 0xFFFFu);
    EXPECT_EQ(c(L4), 0xFFFFFFFFu);
}

TEST_F(GteTest, Lb3SignExtends) {
    wc(LC4, 0x9000u);
    EXPECT_EQ(c(LC4), 0xFFFF9000u);
}

TEST_F(GteTest, TranslationIs32Bit) {
    wc(TRX, 0x80000000u);
    wc(TRY, 0x7FFFFFFFu);
    wc(TRZ, 0x12345678u);
    EXPECT_EQ(c(TRX), 0x80000000u);
    EXPECT_EQ(c(TRY), 0x7FFFFFFFu);
    EXPECT_EQ(c(TRZ), 0x12345678u);
}

TEST_F(GteTest, BackgroundAndFarColourAre32Bit) {
    for (unsigned r : {RBK, GBK, BBK, RFC, GFC, BFC}) {
        wc(r, 0xF0000001u);
        EXPECT_EQ(c(r), 0xF0000001u);
    }
}

TEST_F(GteTest, OffsetsAre32Bit) {
    wc(OFX, 0xFFFF0000u);
    wc(OFY, 0x00018000u);
    EXPECT_EQ(c(OFX), 0xFFFF0000u);
    EXPECT_EQ(c(OFY), 0x00018000u);
}

TEST_F(GteTest, HReadsSignExtended) {
    wc(H, 0xFFFFu);
    EXPECT_EQ(c(H), 0xFFFFFFFFu);
    wc(H, 0x1000u);
    EXPECT_EQ(c(H), 0x1000u);
    wc(H, 0x12348000u);
    EXPECT_EQ(c(H), 0xFFFF8000u);
}

TEST_F(GteTest, DqaSignExtendsDqbDoesNot) {
    wc(DQA, 0x8000u);
    EXPECT_EQ(c(DQA), 0xFFFF8000u);
    wc(DQB, 0x80000000u);
    EXPECT_EQ(c(DQB), 0x80000000u);
}

TEST_F(GteTest, ZsfSignExtend) {
    wc(ZSF3, 0xF000u);
    wc(ZSF4, 0x0800u);
    EXPECT_EQ(c(ZSF3), 0xFFFFF000u);
    EXPECT_EQ(c(ZSF4), 0x0800u);
}

TEST_F(GteTest, FlagWriteMasksLowBits) {
    wc(FLAG, 0xFFFFFFFFu);
    EXPECT_EQ(c(FLAG), 0xFFFFF000u);
}

TEST_F(GteTest, FlagBit31IsSummaryOfErrorBits) {
    wc(FLAG, 0x00001000u);  // IR0 saturation is not an error bit
    EXPECT_EQ(c(FLAG), 0x00001000u);
    wc(FLAG, 0x00002000u);  // SY2 saturation is
    EXPECT_EQ(c(FLAG), 0x80002000u);
    wc(FLAG, 0x00400000u);  // IR3 saturation is not
    EXPECT_EQ(c(FLAG), 0x00400000u);
    wc(FLAG, 0x00800000u);  // IR2 is
    EXPECT_EQ(c(FLAG), 0x80800000u);
    wc(FLAG, 0x00080000u);  // colour B saturation is not
    EXPECT_EQ(c(FLAG), 0x00080000u);
}

TEST_F(GteTest, FlagLowBitsAlwaysZero) {
    wc(FLAG, 0x00000FFFu);
    EXPECT_EQ(c(FLAG), 0u);
    wc(FLAG, 0x80000000u);
    EXPECT_EQ(c(FLAG), 0u);
}

TEST_F(GteTest, ResetClearsEverything) {
    wd(VXY0, 5);
    wc(TRX, 7);
    wc(FLAG, 0x2000);
    g.reset();
    EXPECT_EQ(d(VXY0), 0u);
    EXPECT_EQ(c(TRX), 0u);
    EXPECT_EQ(c(FLAG), 0u);
}

TEST_F(GteTest, ControlRegisterIndexMasked) {
    wc(32 + TRX, 9);
    EXPECT_EQ(c(TRX), 9u);
}

// ---------------------------------------------------------------- cycle counts

TEST(GteCycles, TimingTable) {
    struct E { uint32_t op; int cycles; };
    const E table[] = {{RTPS, 15}, {NCLIP, 8}, {OP, 6}, {DPCS, 8}, {INTPL, 8}, {MVMVA, 8}, {NCDS, 19}, {CDP, 13},
                       {NCDT, 44}, {NCCS, 17}, {CC, 11}, {NCS, 14}, {NCT, 30}, {SQR, 5}, {DCPL, 8}, {DPCT, 17},
                       {AVSZ3, 5}, {AVSZ4, 6}, {RTPT, 23}, {GPF, 5}, {GPL, 5}, {NCCT, 39}};
    for (const E& e : table) EXPECT_EQ(dw3::Gte::command_cycles(cmd(e.op)), e.cycles) << std::hex << e.op;
}

TEST(GteCycles, IgnoresSfLmAndUpperBits) {
    EXPECT_EQ(dw3::Gte::command_cycles(cmd(RTPS, 1, 1)), 15);
    EXPECT_EQ(dw3::Gte::command_cycles((0x1FFFFFFFu & ~0x3Fu) | RTPT), 23);
    EXPECT_EQ(dw3::Gte::command_cycles(mv(3, 3, 3, 1, 1)), 8);
}

TEST(GteCycles, UnknownCommandIsPositive) { EXPECT_GE(dw3::Gte::command_cycles(0x00), 1); }

// ---------------------------------------------------------------- RTPS / RTPT / divider

TEST_F(GteTest, RtpsIdentityProjection) {
    proj_setup();
    vec(0, 0x100, 0x200, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(sd(MAC1), 0x100);
    EXPECT_EQ(sd(MAC2), 0x200);
    EXPECT_EQ(sd(MAC3), 0x1000);
    EXPECT_EQ(sd(IR1), 0x100);
    EXPECT_EQ(sd(IR2), 0x200);
    EXPECT_EQ(sd(IR3), 0x1000);
    EXPECT_EQ(d(SZ3), 0x1000u);
    EXPECT_EQ(d(SXY2), pk(0x100, 0x200));
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, RtpsHalvesAtDoubleDepth) {
    proj_setup();
    vec(0, 0x100, 0x200, 0x2000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SXY2), pk(0x80, 0x100));
}

TEST_F(GteTest, RtpsAppliesScreenOffset) {
    proj_setup();
    wc(OFX, 10u << 16);
    wc(OFY, static_cast<uint32_t>(-20) << 16);
    vec(0, 0, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SXY2), pk(10, -20));
}

TEST_F(GteTest, RtpsTranslation) {
    proj_setup();
    tr(5, -6, 0x1000);
    vec(0, 1, 2, 0);
    run(cmd(RTPS, 1));
    EXPECT_EQ(sd(MAC1), 6);
    EXPECT_EQ(sd(MAC2), -4);
    EXPECT_EQ(sd(MAC3), 0x1000);
}

TEST_F(GteTest, RtpsSf0KeepsFixedPoint) {
    proj_setup();
    vec(0, 1, 2, 3);
    run(cmd(RTPS, 0));
    EXPECT_EQ(sd(MAC1), 0x1000);
    EXPECT_EQ(sd(MAC2), 0x2000);
    EXPECT_EQ(sd(MAC3), 0x3000);
    EXPECT_EQ(d(SZ3), 3u);  // SZ3 always uses MAC3 >> 12
}

TEST_F(GteTest, RtpsRotationMatrix) {
    wc(RT0, pk(0, -0x1000));
    wc(RT1, pk(0, 0x1000));
    wc(RT2, pk(0, 0));
    wc(RT3, pk(0, 0));
    wc(RT4, 0x1000);
    wc(H, 0x1000);
    vec(0, 0x100, 0x200, 0x300);
    run(cmd(RTPS, 1));
    EXPECT_EQ(sd(MAC1), -0x200);
    EXPECT_EQ(sd(MAC2), 0x100);
    EXPECT_EQ(sd(MAC3), 0x300);
}

TEST_F(GteTest, RtpsPushesSzFifo) {
    proj_setup();
    for (int z : {0x10, 0x20, 0x30, 0x40}) {
        vec(0, 0, 0, z);
        run(cmd(RTPS, 1));
    }
    EXPECT_EQ(d(SZ0), 0x10u);
    EXPECT_EQ(d(SZ1), 0x20u);
    EXPECT_EQ(d(SZ2), 0x30u);
    EXPECT_EQ(d(SZ3), 0x40u);
}

TEST_F(GteTest, RtpsPushesSxyFifo) {
    proj_setup();
    for (int x : {1, 2, 3}) {
        vec(0, x, 0, 0x1000);
        run(cmd(RTPS, 1));
    }
    EXPECT_EQ(d(SXY0), pk(1, 0));
    EXPECT_EQ(d(SXY1), pk(2, 0));
    EXPECT_EQ(d(SXY2), pk(3, 0));
}

TEST_F(GteTest, RtpsDepthCueing) {
    proj_setup();
    wc(DQA, 0x100);
    wc(DQB, 0);
    vec(0, 0, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(sd(MAC0), 0x1000000);
    EXPECT_EQ(sd(IR0), 0x1000);
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, RtpsDepthCueingMac0IsDivideResult) {
    proj_setup();
    wc(DQA, 1);
    vec(0, 0, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(sd(MAC0), 0x10000);
}

TEST_F(GteTest, RtpsIr0SaturatesHigh) {
    proj_setup();
    wc(DQA, 0x200);
    vec(0, 0, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(sd(IR0), 0x1000);
    EXPECT_EQ(flag(), F_IR0);  // not an error bit: no summary
}

TEST_F(GteTest, RtpsIr0SaturatesLow) {
    proj_setup();
    wc(DQA, static_cast<uint32_t>(-0x100));
    vec(0, 0, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(sd(IR0), 0);
    EXPECT_EQ(flag(), F_IR0);
}

TEST_F(GteTest, RtpsMac0PositiveOverflow) {
    proj_setup();
    wc(DQA, 1);
    wc(DQB, 0x7FFFFFFFu);
    vec(0, 0, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(flag(), F_SUM | F_MAC0P | F_IR0);
}

TEST_F(GteTest, RtpsMac0NegativeOverflow) {
    proj_setup();
    wc(DQA, static_cast<uint32_t>(-1));
    wc(DQB, 0x80000000u);
    vec(0, 0, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(flag(), F_SUM | F_MAC0N | F_IR0);
}

TEST_F(GteTest, RtpsSxClampHigh) {
    proj_setup();
    vec(0, 0x400, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SXY2) & 0xFFFF, 0x3FFu);
    EXPECT_EQ(flag(), F_SUM | F_SX);
}

TEST_F(GteTest, RtpsSxNoClampAtLowLimit) {
    proj_setup();
    vec(0, -0x400, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SXY2), pk(-0x400, 0));
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, RtpsSxClampLow) {
    proj_setup();
    vec(0, -0x401, 0, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SXY2), pk(-0x400, 0));
    EXPECT_EQ(flag(), F_SUM | F_SX);
}

TEST_F(GteTest, RtpsSyClampHigh) {
    proj_setup();
    vec(0, 0, 0x7FFF, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SXY2), pk(0, 0x3FF));
    EXPECT_EQ(flag(), F_SUM | F_SY);
}

TEST_F(GteTest, RtpsSyClampLow) {
    proj_setup();
    vec(0, 0, -0x7FFF, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SXY2), pk(0, -0x400));
    EXPECT_EQ(flag(), F_SUM | F_SY);
}

TEST_F(GteTest, RtpsSzClampHigh) {
    proj_setup();
    wc(H, 0);
    tr(0, 0, 0x12345);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SZ3), 0xFFFFu);
    EXPECT_EQ(flag(), F_SUM | F_SZ | F_IR3);
}

TEST_F(GteTest, RtpsSzClampLow) {
    proj_setup();
    tr(0, 0, -5);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(SZ3), 0u);
    EXPECT_EQ(sd(IR3), -5);
    EXPECT_EQ(flag(), F_SUM | F_SZ | F_DIV);  // sz3 = 0 also overflows the divider
}

TEST_F(GteTest, RtpsDivideOverflow) {
    proj_setup();
    wc(H, 0x2000);
    vec(0, 0, 0, 0x1000);
    wc(DQA, 1);
    run(cmd(RTPS, 1));
    EXPECT_EQ(sd(MAC0), 0x1FFFF);
    EXPECT_EQ(flag(), F_SUM | F_DIV);
}

TEST_F(GteTest, RtpsNoDivideOverflowJustBelow) {
    proj_setup();
    wc(H, 0x1FFF);
    vec(0, 0, 0, 0x1000);
    wc(DQA, 1);
    run(cmd(RTPS, 1));
    EXPECT_LE(sd(MAC0), 0x1FFFF);
    EXPECT_GT(sd(MAC0), 0x1FF00);
    EXPECT_EQ(flag() & F_DIV, 0u);
}

TEST_F(GteTest, RtpsIr1Saturation) {
    proj_setup();
    vec(0, 8, 0, 0x1000);
    run(cmd(RTPS, 0));
    EXPECT_EQ(sd(IR1), 0x7FFF);
    EXPECT_EQ(flag() & F_IR1, F_IR1);
}

TEST_F(GteTest, RtpsIr2NegativeSaturation) {
    proj_setup();
    vec(0, 0, -9, 0x1000);
    run(cmd(RTPS, 0));
    EXPECT_EQ(sd(IR2), -0x8000);
    EXPECT_EQ(flag() & F_IR2, F_IR2);
}

TEST_F(GteTest, RtpsLmClampsNegativeIr) {
    proj_setup();
    vec(0, -5, 0, 0x1000);
    run(cmd(RTPS, 1, 1));
    EXPECT_EQ(sd(IR1), 0);
    EXPECT_EQ(sd(MAC1), -5);
    EXPECT_EQ(flag() & F_IR1, F_IR1);
}

TEST_F(GteTest, RtpsIr3FlagUsesUnshiftedValue) {
    proj_setup();
    vec(0, 0, 0, 0x7FFF);
    run(cmd(RTPS, 0));  // sf=0: MAC3 = 0x7FFF000, but MAC3 >> 12 fits
    EXPECT_EQ(sd(MAC3), 0x7FFF000);
    EXPECT_EQ(sd(IR3), 0x7FFF);
    EXPECT_EQ(flag() & F_IR3, 0u);
}

TEST_F(GteTest, RtpsIr3NegativeWithLmDoesNotFlagInRange) {
    proj_setup();
    tr(0, 0, -5);
    run(cmd(RTPS, 1, 1));
    EXPECT_EQ(sd(IR3), 0);
    EXPECT_EQ(flag() & F_IR3, 0u);
}

TEST_F(GteTest, RtpsMacOverflowPositive) {
    proj_setup();
    wc(RT0, pk(0x7FFF, 0));
    tr(0x7FFFFFFF, 0, 0);
    vec(0, 0x7FFF, 0, 0);
    run(cmd(RTPS, 0));
    EXPECT_EQ(flag() & F_MAC1P, F_MAC1P);
}

TEST_F(GteTest, RtptProjectsThreeVertices) {
    proj_setup();
    vec(0, 0x10, 0x20, 0x1000);
    vec(1, 0x30, 0x40, 0x1000);
    vec(2, 0x50, 0x60, 0x1000);
    run(cmd(RTPT, 1));
    EXPECT_EQ(d(SXY0), pk(0x10, 0x20));
    EXPECT_EQ(d(SXY1), pk(0x30, 0x40));
    EXPECT_EQ(d(SXY2), pk(0x50, 0x60));
    EXPECT_EQ(d(SZ1), 0x1000u);
    EXPECT_EQ(d(SZ2), 0x1000u);
    EXPECT_EQ(d(SZ3), 0x1000u);
    EXPECT_EQ(sd(IR1), 0x50);
    EXPECT_EQ(sd(IR2), 0x60);
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, RtptDepthCueUsesLastVertex) {
    proj_setup();
    wc(DQA, 1);
    vec(0, 0, 0, 0x1000);
    vec(1, 0, 0, 0x1000);
    vec(2, 0, 0, 0x2000);
    run(cmd(RTPT, 1));
    EXPECT_EQ(sd(MAC0), 0x8000);
}

TEST_F(GteTest, RtptDistinctDepths) {
    proj_setup();
    vec(0, 0x100, 0, 0x1000);
    vec(1, 0x100, 0, 0x2000);
    vec(2, 0x100, 0, 0x4000);
    run(cmd(RTPT, 1));
    EXPECT_EQ(d(SXY0), pk(0x100, 0));
    EXPECT_EQ(d(SXY1), pk(0x80, 0));
    EXPECT_EQ(d(SXY2), pk(0x40, 0));
    EXPECT_EQ(d(SZ1), 0x1000u);
    EXPECT_EQ(d(SZ2), 0x2000u);
    EXPECT_EQ(d(SZ3), 0x4000u);
}

TEST_F(GteTest, RtptFlagsAccumulateAcrossVertices) {
    proj_setup();
    vec(0, 0x7FFF, 0, 0x1000);
    vec(1, 0, 0, 0x1000);
    vec(2, 0, 0, 0x1000);
    run(cmd(RTPT, 1));
    EXPECT_EQ(flag() & F_SX, F_SX);
}

TEST_F(GteTest, DividerSpotCheckPowersOfTwo) {
    proj_setup();
    wc(DQA, 1);
    struct E { uint32_t h, sz, q; };
    const E cases[] = {{0x1000, 0x1000, 0x10000}, {0x1000, 0x2000, 0x8000}, {0x1000, 0x4000, 0x4000},
                       {0x2000, 0x4000, 0x8000}, {0x800, 0x1000, 0x8000}, {0x1000, 0x800, 0x1FFFF}};
    for (const E& e : cases) {
        wc(H, e.h);
        vec(0, 0, 0, static_cast<int>(e.sz));
        run(cmd(RTPS, 1));
        EXPECT_EQ(static_cast<uint32_t>(sd(MAC0)), e.q) << std::hex << e.h << "/" << e.sz;
    }
}

TEST_F(GteTest, DividerTracksTrueQuotient) {
    proj_setup();
    wc(DQA, 1);
    for (uint32_t sz = 0x100; sz <= 0xFFFF; sz += 0x137) {
        for (uint32_t h : {0x100u, 0x555u, 0x1000u, 0x3000u, 0x7FFFu}) {
            if (h >= sz * 2) continue;
            wc(H, h);
            vec(0, 0, 0, 0);
            tr(0, 0, static_cast<int>(sz));
            run(cmd(RTPS, 1));
            double ideal = static_cast<double>(h) * 65536.0 / sz;
            if (ideal > 0x1FFFF) ideal = 0x1FFFF;
            EXPECT_LE(std::abs(sd(MAC0) - ideal), 2.0 + ideal * 0.0003) << std::hex << h << "/" << sz;
        }
    }
}

TEST_F(GteTest, DividerMaxQuotientSaturates) {
    proj_setup();
    wc(DQA, 1);
    wc(H, 0xFFFF);
    vec(0, 0, 0, 0x8000);  // H just under 2*SZ3
    run(cmd(RTPS, 1));
    EXPECT_LE(sd(MAC0), 0x1FFFF);
    EXPECT_GE(sd(MAC0), 0x1FFF0);
}

// ---------------------------------------------------------------- NCLIP

TEST_F(GteTest, NclipCounterClockwise) {
    wd(SXY0, pk(0, 0));
    wd(SXY1, pk(10, 0));
    wd(SXY2, pk(0, 10));
    run(cmd(NCLIP));
    EXPECT_EQ(sd(MAC0), 100);
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, NclipClockwise) {
    wd(SXY0, pk(0, 0));
    wd(SXY1, pk(0, 10));
    wd(SXY2, pk(10, 0));
    run(cmd(NCLIP));
    EXPECT_EQ(sd(MAC0), -100);
}

TEST_F(GteTest, NclipCollinearIsZero) {
    wd(SXY0, pk(0, 0));
    wd(SXY1, pk(5, 5));
    wd(SXY2, pk(10, 10));
    run(cmd(NCLIP));
    EXPECT_EQ(sd(MAC0), 0);
}

TEST_F(GteTest, NclipNonOriginTriangle) {
    wd(SXY0, pk(10, 10));
    wd(SXY1, pk(30, 10));
    wd(SXY2, pk(10, 50));
    run(cmd(NCLIP));
    EXPECT_EQ(sd(MAC0), 20 * 40);
}

TEST_F(GteTest, NclipNegativeCoordinates) {
    wd(SXY0, pk(-10, -10));
    wd(SXY1, pk(10, -10));
    wd(SXY2, pk(-10, 10));
    run(cmd(NCLIP));
    EXPECT_EQ(sd(MAC0), 20 * 20);
}

TEST_F(GteTest, NclipPositiveOverflow) {
    wd(SXY0, pk(0, 0x7FFF));
    wd(SXY1, pk(-0x8000, 0));
    wd(SXY2, pk(0x7FFF, -0x8000));
    run(cmd(NCLIP));
    EXPECT_EQ(flag(), F_SUM | F_MAC0P);
    EXPECT_EQ(d(MAC0), 0xBFFE8001u);
}

TEST_F(GteTest, NclipNegativeOverflow) {
    wd(SXY0, pk(0, 0x7FFF));
    wd(SXY1, pk(0x7FFF, -0x8000));
    wd(SXY2, pk(-0x8000, 0));
    run(cmd(NCLIP));
    EXPECT_EQ(flag(), F_SUM | F_MAC0N);
}

TEST_F(GteTest, FlagClearedBetweenCommands) {
    wd(SXY0, pk(0, 0x7FFF));
    wd(SXY1, pk(-0x8000, 0));
    wd(SXY2, pk(0x7FFF, -0x8000));
    run(cmd(NCLIP));
    ASSERT_NE(flag(), 0u);
    wd(SXY0, 0);
    wd(SXY1, 0);
    wd(SXY2, 0);
    run(cmd(NCLIP));
    EXPECT_EQ(flag(), 0u);
}

// ---------------------------------------------------------------- OP

TEST_F(GteTest, OpCrossProductSf1) {
    mat(RT0, 0x1000);
    ir(1, 2, 3);
    run(cmd(OP, 1));
    EXPECT_EQ(sd(MAC1), 1);
    EXPECT_EQ(sd(MAC2), -2);
    EXPECT_EQ(sd(MAC3), 1);
    EXPECT_EQ(sd(IR2), -2);
}

TEST_F(GteTest, OpCrossProductSf0) {
    wc(RT0, pk(1, 0));
    wc(RT2, pk(2, 0));
    wc(RT4, 3);
    ir(4, 5, 6);
    run(cmd(OP));
    EXPECT_EQ(sd(MAC1), -3);
    EXPECT_EQ(sd(MAC2), 6);
    EXPECT_EQ(sd(MAC3), -3);
}

TEST_F(GteTest, OpLmClampsNegative) {
    mat(RT0, 0x1000);
    ir(1, 2, 3);
    run(cmd(OP, 1, 1));
    EXPECT_EQ(sd(IR2), 0);
    EXPECT_EQ(sd(MAC2), -2);
    EXPECT_EQ(flag(), F_SUM | F_IR2);
}

TEST_F(GteTest, OpIgnoresOffDiagonalRotation) {
    wc(RT0, pk(1, 0x555));
    wc(RT1, pk(0x555, 0x555));
    wc(RT2, pk(1, 0x555));
    wc(RT3, pk(0x555, 0x555));
    wc(RT4, 1);
    ir(1, 0, 0);
    run(cmd(OP));
    EXPECT_EQ(sd(MAC1), 0);
    EXPECT_EQ(sd(MAC2), 1);
    EXPECT_EQ(sd(MAC3), -1);
}

TEST_F(GteTest, OpSaturatesIr) {
    wc(RT0, pk(0x7FFF, 0));
    wc(RT2, pk(0x7FFF, 0));
    wc(RT4, 0x7FFF);
    ir(0x7FFF, 0, 0x7FFF);
    run(cmd(OP));
    // MAC1 = ir3*d2 - ir2*d3 = 0x3FFF0001 -> IR1 saturates
    EXPECT_EQ(sd(MAC1), 0x3FFF0001);
    EXPECT_EQ(sd(IR1), 0x7FFF);
    EXPECT_EQ(flag() & F_IR1, F_IR1);
}

// ---------------------------------------------------------------- SQR

TEST_F(GteTest, SqrSf0) {
    ir(0, 3, -4);
    wd(IR3, 0x100);
    wd(IR1, 3);
    wd(IR2, static_cast<uint32_t>(-4));
    run(cmd(SQR));
    EXPECT_EQ(sd(MAC1), 9);
    EXPECT_EQ(sd(MAC2), 16);
    EXPECT_EQ(sd(MAC3), 0x10000);
    EXPECT_EQ(sd(IR3), 0x7FFF);
    EXPECT_EQ(flag(), F_IR3);  // IR3 saturation is not an error bit
}

TEST_F(GteTest, SqrSf1) {
    ir(0x100, 0x200, 0x300);
    run(cmd(SQR, 1));
    EXPECT_EQ(sd(MAC1), 0x10);
    EXPECT_EQ(sd(MAC2), 0x40);
    EXPECT_EQ(sd(MAC3), 0x90);
    EXPECT_EQ(sd(IR1), 0x10);
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, SqrIr1Saturation) {
    ir(0x7FFF, 0, 0);
    run(cmd(SQR));
    EXPECT_EQ(sd(MAC1), 0x3FFF0001);
    EXPECT_EQ(sd(IR1), 0x7FFF);
    EXPECT_EQ(flag(), F_SUM | F_IR1);
}

TEST_F(GteTest, SqrNegativeInputGivesPositive) {
    ir(-0x100, -1, -0x8000);
    run(cmd(SQR, 1));
    EXPECT_EQ(sd(MAC1), 0x10);
    EXPECT_EQ(sd(MAC2), 0);
    EXPECT_EQ(sd(MAC3), 0x40000);
    EXPECT_EQ(sd(IR3), 0x7FFF);
}

TEST_F(GteTest, SqrLmIrrelevant) {
    ir(-3, 4, 5);
    run(cmd(SQR, 0, 1));
    EXPECT_EQ(sd(IR1), 9);
}

// ---------------------------------------------------------------- AVSZ

TEST_F(GteTest, Avsz3Average) {
    wd(SZ1, 0x100);
    wd(SZ2, 0x200);
    wd(SZ3, 0x300);
    wc(ZSF3, 0x555);
    run(cmd(AVSZ3));
    EXPECT_EQ(sd(MAC0), 0x555 * 0x600);
    EXPECT_EQ(d(OTZ), 0x1FFu);
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, Avsz3IgnoresSz0) {
    wd(SZ0, 0xFFFF);
    wd(SZ1, 1);
    wd(SZ2, 2);
    wd(SZ3, 3);
    wc(ZSF3, 0x1000);
    run(cmd(AVSZ3));
    EXPECT_EQ(d(OTZ), 6u);
}

TEST_F(GteTest, Avsz4Average) {
    wd(SZ0, 0x100);
    wd(SZ1, 0x200);
    wd(SZ2, 0x300);
    wd(SZ3, 0x400);
    wc(ZSF4, 0x400);
    run(cmd(AVSZ4));
    EXPECT_EQ(sd(MAC0), 0x280000);
    EXPECT_EQ(d(OTZ), 0x280u);
}

TEST_F(GteTest, Avsz3ClampHigh) {
    wd(SZ1, 0xFFFF);
    wd(SZ2, 0xFFFF);
    wd(SZ3, 0xFFFF);
    wc(ZSF3, 0x1000);
    run(cmd(AVSZ3));
    EXPECT_EQ(d(OTZ), 0xFFFFu);
    EXPECT_EQ(flag(), F_SUM | F_SZ);
}

TEST_F(GteTest, Avsz3ClampLow) {
    wd(SZ1, 1);
    wd(SZ2, 1);
    wd(SZ3, 1);
    wc(ZSF3, static_cast<uint32_t>(-0x1000));
    run(cmd(AVSZ3));
    EXPECT_EQ(d(OTZ), 0u);
    EXPECT_EQ(sd(MAC0), -0x3000);
    EXPECT_EQ(flag(), F_SUM | F_SZ);
}

TEST_F(GteTest, Avsz4ClampHigh) {
    wd(SZ0, 0xFFFF);
    wd(SZ1, 0xFFFF);
    wd(SZ2, 0xFFFF);
    wd(SZ3, 0xFFFF);
    wc(ZSF4, 0x1000);
    run(cmd(AVSZ4));
    EXPECT_EQ(d(OTZ), 0xFFFFu);
    EXPECT_EQ(flag() & F_SZ, F_SZ);
}

TEST_F(GteTest, Avsz3Mac0Overflow) {
    wd(SZ1, 0xFFFF);
    wd(SZ2, 0xFFFF);
    wd(SZ3, 0xFFFF);
    wc(ZSF3, 0x7FFF);
    run(cmd(AVSZ3));
    EXPECT_EQ(flag() & F_MAC0P, F_MAC0P);
    EXPECT_EQ(flag() & F_SZ, F_SZ);
}

TEST_F(GteTest, Avsz3ZeroZsf) {
    wd(SZ1, 5);
    wd(SZ2, 6);
    wd(SZ3, 7);
    run(cmd(AVSZ3));
    EXPECT_EQ(d(OTZ), 0u);
    EXPECT_EQ(flag(), 0u);
}

// ---------------------------------------------------------------- MVMVA

TEST_F(GteTest, MvmvaRotationVectorV0) {
    rot_id();
    vec(0, 1, -2, 3);
    run(mv(0, 0, 3, 1));
    EXPECT_EQ(sd(MAC1), 1);
    EXPECT_EQ(sd(MAC2), -2);
    EXPECT_EQ(sd(MAC3), 3);
    EXPECT_EQ(sd(IR2), -2);
}

TEST_F(GteTest, MvmvaSelectsV1AndV2) {
    rot_id();
    vec(0, 1, 1, 1);
    vec(1, 2, 3, 4);
    vec(2, 5, 6, 7);
    run(mv(0, 1, 3, 1));
    EXPECT_EQ(sd(MAC1), 2);
    EXPECT_EQ(sd(MAC3), 4);
    run(mv(0, 2, 3, 1));
    EXPECT_EQ(sd(MAC1), 5);
    EXPECT_EQ(sd(MAC3), 7);
}

TEST_F(GteTest, MvmvaVectorFromIr) {
    rot_id();
    ir(7, 8, 9);
    run(mv(0, 3, 3, 1));
    EXPECT_EQ(sd(MAC1), 7);
    EXPECT_EQ(sd(MAC2), 8);
    EXPECT_EQ(sd(MAC3), 9);
}

TEST_F(GteTest, MvmvaSelectsLightMatrix) {
    mat(L0, 0x2000);
    mat(LC0, 0x3000);
    rot_id();
    vec(0, 1, 1, 1);
    run(mv(1, 0, 3, 1));
    EXPECT_EQ(sd(MAC1), 2);
    run(mv(2, 0, 3, 1));
    EXPECT_EQ(sd(MAC1), 3);
    run(mv(0, 0, 3, 1));
    EXPECT_EQ(sd(MAC1), 1);
}

TEST_F(GteTest, MvmvaTranslationVector) {
    rot_id();
    tr(10, 20, 30);
    vec(0, 1, 2, 3);
    run(mv(0, 0, 0, 1));
    EXPECT_EQ(sd(MAC1), 11);
    EXPECT_EQ(sd(MAC2), 22);
    EXPECT_EQ(sd(MAC3), 33);
}

TEST_F(GteTest, MvmvaBackgroundVector) {
    rot_id();
    bk(100, 200, 300);
    vec(0, 1, 2, 3);
    run(mv(0, 0, 1, 1));
    EXPECT_EQ(sd(MAC1), 101);
    EXPECT_EQ(sd(MAC2), 202);
    EXPECT_EQ(sd(MAC3), 303);
}

TEST_F(GteTest, MvmvaNoTranslation) {
    rot_id();
    tr(10, 20, 30);
    bk(1, 1, 1);
    fc(5, 5, 5);
    vec(0, 1, 2, 3);
    run(mv(0, 0, 3, 1));
    EXPECT_EQ(sd(MAC1), 1);
    EXPECT_EQ(sd(MAC2), 2);
    EXPECT_EQ(sd(MAC3), 3);
}

TEST_F(GteTest, MvmvaFarColourBugDropsFirstTerm) {
    rot_id();
    fc(0x10, 0x20, 0x30);
    vec(0, 2, 3, 4);
    run(mv(0, 0, 2, 1));
    EXPECT_EQ(sd(MAC1), 0);  // (FC + M0*V0) is discarded
    EXPECT_EQ(sd(MAC2), 3);
    EXPECT_EQ(sd(MAC3), 4);
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, MvmvaFarColourBugStillFlagsFirstTerm) {
    rot_id();
    fc(0x10000, 0, 0);
    vec(0, 0, 0, 0);
    run(mv(0, 0, 2, 1));
    EXPECT_EQ(sd(MAC1), 0);
    EXPECT_EQ(flag(), F_SUM | F_IR1);
}

TEST_F(GteTest, MvmvaFarColourBugFirstTermNegativeIgnoresLm) {
    rot_id();
    fc(-5, 0, 0);
    vec(0, 0, 0, 0);
    run(mv(0, 0, 2, 1, 1));
    // The hidden first term is always saturated with lm=0, so -5 is in range.
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, MvmvaFarColourBugSecondColumns) {
    wc(RT0, pk(0x1000, 0x1000));
    wc(RT1, pk(0x1000, 0));
    wc(RT2, pk(0, 0));
    wc(RT3, pk(0, 0));
    wc(RT4, 0);
    fc(100, 0, 0);
    vec(0, 7, 8, 9);
    run(mv(0, 0, 2, 1));
    EXPECT_EQ(sd(MAC1), 17);  // 8 + 9, no FC and no V0 term
}

TEST_F(GteTest, MvmvaGarbageMatrix) {
    color(0x10, 0, 0);
    wc(RT1, pk(5, 0));       // RT13 = 5
    wc(RT2, pk(7, 0));       // RT22 = 7
    vec(0, 1, 1, 1);
    ir(0, 3, 0, 0);
    wd(IR0, 3);
    run(mv(3, 0, 3, 0));
    EXPECT_EQ(sd(MAC1), -0x100 + 0x100 + 3);
    EXPECT_EQ(sd(MAC2), 15);
    EXPECT_EQ(sd(MAC3), 21);
}

TEST_F(GteTest, MvmvaSf0KeepsFixedPoint) {
    rot_id();
    vec(0, 2, 0, 0);
    run(mv(0, 0, 3, 0));
    EXPECT_EQ(sd(MAC1), 0x2000);
    EXPECT_EQ(sd(IR1), 0x2000);
}

TEST_F(GteTest, MvmvaIrSaturatesHigh) {
    rot_id();
    vec(0, 8, 0, 0);
    run(mv(0, 0, 3, 0));
    EXPECT_EQ(sd(IR1), 0x7FFF);
    EXPECT_EQ(flag(), F_SUM | F_IR1);
}

TEST_F(GteTest, MvmvaIrSaturatesLowWithoutLm) {
    rot_id();
    vec(0, 0, -9, 0);
    run(mv(0, 0, 3, 0));
    EXPECT_EQ(sd(IR2), -0x8000);
    EXPECT_EQ(flag(), F_SUM | F_IR2);
}

TEST_F(GteTest, MvmvaLmClampsToZero) {
    rot_id();
    vec(0, -1, 0, 0);
    run(mv(0, 0, 3, 1, 1));
    EXPECT_EQ(sd(IR1), 0);
    EXPECT_EQ(sd(MAC1), -1);
    EXPECT_EQ(flag(), F_SUM | F_IR1);
}

TEST_F(GteTest, MvmvaIr3SaturationNotErrorBit) {
    rot_id();
    vec(0, 0, 0, 9);
    run(mv(0, 0, 3, 0));
    EXPECT_EQ(sd(IR3), 0x7FFF);
    EXPECT_EQ(flag(), F_IR3);
}

TEST_F(GteTest, MvmvaMac1PositiveOverflow) {
    wc(RT0, pk(0x7FFF, 0));
    tr(0x7FFFFFFF, 0, 0);
    vec(0, 0x7FFF, 0, 0);
    run(mv(0, 0, 0, 0));
    EXPECT_EQ(flag(), F_SUM | F_MAC1P | F_IR1);
}

TEST_F(GteTest, MvmvaMac1NegativeOverflow) {
    wc(RT0, pk(-0x8000, 0));
    tr(static_cast<int>(0x80000000u), 0, 0);
    vec(0, 0x7FFF, 0, 0);
    run(mv(0, 0, 0, 0));
    EXPECT_EQ(flag(), F_SUM | F_MAC1N | F_IR1);
}

TEST_F(GteTest, MvmvaMac2PositiveOverflow) {
    wc(RT2, pk(0x7FFF, 0));
    tr(0, 0x7FFFFFFF, 0);
    vec(0, 0, 0x7FFF, 0);
    run(mv(0, 0, 0, 0));
    EXPECT_EQ(flag() & F_MAC2P, F_MAC2P);
}

TEST_F(GteTest, MvmvaMac3PositiveOverflow) {
    wc(RT4, 0x7FFF);
    tr(0, 0, 0x7FFFFFFF);
    vec(0, 0, 0, 0x7FFF);
    run(mv(0, 0, 0, 0));
    EXPECT_EQ(flag() & F_MAC3P, F_MAC3P);
}

TEST_F(GteTest, MvmvaMac2And3NegativeOverflow) {
    wc(RT2, pk(-0x8000, 0));
    wc(RT4, static_cast<uint32_t>(-0x8000));
    tr(0, static_cast<int>(0x80000000u), static_cast<int>(0x80000000u));
    vec(0, 0, 0x7FFF, 0x7FFF);
    run(mv(0, 0, 0, 0));
    EXPECT_EQ(flag() & F_MAC2N, F_MAC2N);
    EXPECT_EQ(flag() & F_MAC3N, F_MAC3N);
}

TEST_F(GteTest, MvmvaSf1ExtendsRangeBeyondMac32) {
    wc(RT0, pk(0x7FFF, 0));
    vec(0, 0x7FFF, 0, 0);
    run(mv(0, 0, 3, 1));
    EXPECT_EQ(sd(MAC1), 0x3FFF0);
    EXPECT_EQ(sd(IR1), 0x7FFF);
    EXPECT_EQ(flag(), F_SUM | F_IR1);
}

TEST_F(GteTest, MvmvaUsesFullRotationMatrix) {
    wc(RT0, pk(1, 2));
    wc(RT1, pk(3, 4));
    wc(RT2, pk(5, 6));
    wc(RT3, pk(7, 8));
    wc(RT4, 9);
    vec(0, 1, 10, 100);
    run(mv(0, 0, 3, 0));
    EXPECT_EQ(sd(MAC1), 1 * 1 + 2 * 10 + 3 * 100);
    EXPECT_EQ(sd(MAC2), 4 * 1 + 5 * 10 + 6 * 100);
    EXPECT_EQ(sd(MAC3), 7 * 1 + 8 * 10 + 9 * 100);
}

TEST_F(GteTest, MvmvaNegativeMatrixEntries) {
    wc(RT0, pk(-1, -2));
    wc(RT1, pk(-3, 0));
    vec(0, 1, 1, 1);
    run(mv(0, 0, 3, 0));
    EXPECT_EQ(sd(MAC1), -6);
}

// ---------------------------------------------------------------- DPCS / DPCT / INTPL

TEST_F(GteTest, DpcsNoInterpolationKeepsColour) {
    color(0x80, 0x40, 0x20, 0x2C);
    ir(0, 0, 0, 0);
    run(cmd(DPCS, 1));
    EXPECT_EQ(d(RGB2), 0x2C204080u);
    EXPECT_EQ(sd(IR1), 0x800);
    EXPECT_EQ(sd(IR2), 0x400);
    EXPECT_EQ(sd(IR3), 0x200);
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteTest, DpcsFullInterpolationReachesFarColour) {
    color(0x10, 0x20, 0x30, 0x2C);
    fc(0x800, 0x400, 0x200);
    wd(IR0, 0x1000);
    run(cmd(DPCS, 1));
    EXPECT_EQ(d(RGB2), 0x2C204080u);
}

TEST_F(GteTest, DpcsHalfInterpolation) {
    color(0x10, 0x20, 0x30, 0x2C);
    fc(0x800, 0x400, 0x200);
    wd(IR0, 0x800);
    run(cmd(DPCS, 1));
    EXPECT_EQ(d(RGB2), 0x2C283048u);
}

TEST_F(GteTest, DpcsPushesColourFifo) {
    color(1, 2, 3, 4);
    wd(RGB2, 0xAABBCCDD);
    wd(RGB1, 0x11223344);
    run(cmd(DPCS, 1));
    EXPECT_EQ(d(RGB0), 0x11223344u);
    EXPECT_EQ(d(RGB1), 0xAABBCCDDu);
    EXPECT_EQ(d(RGB2), 0x04030201u);
}

TEST_F(GteTest, DpcsColourSaturation) {
    color(0xFF, 0, 0);
    fc(0x7FFF, -0x7FFF, 0);
    wd(IR0, 0x1000);
    run(cmd(DPCS, 1));
    EXPECT_EQ(d(RGB2) & 0xFF, 0xFFu);
    EXPECT_EQ((d(RGB2) >> 8) & 0xFF, 0u);
    EXPECT_EQ(flag() & (F_CR | F_CG), F_CR | F_CG);
    EXPECT_EQ(flag() & F_SUM, 0u);  // colour saturation is not an error bit
}

TEST_F(GteTest, DpcsFarColourDifferenceSaturatesIr) {
    color(0, 0, 0);
    fc(0x7FFFFFF, 0, 0);
    run(cmd(DPCS, 1));
    EXPECT_EQ(flag() & F_IR1, F_IR1);
}

TEST_F(GteTest, DpcsSf0) {
    color(1, 0, 0);
    wd(IR0, 0);
    run(cmd(DPCS, 0));
    EXPECT_EQ(sd(MAC1), 1 << 16 >> 0);
    EXPECT_EQ(sd(IR1), 0x7FFF);  // 0x10000 saturates the IR
}

TEST_F(GteTest, DpctProcessesOldestColourThreeTimes) {
    wd(RGB0, 0x00000010);
    wd(RGB1, 0x00002000);
    wd(RGB2, 0x00300000);
    color(0, 0, 0, 0x55);
    wd(IR0, 0);
    run(cmd(DPCT, 1));
    EXPECT_EQ(d(RGB0), 0x55000010u);
    EXPECT_EQ(d(RGB1), 0x55002000u);
    EXPECT_EQ(d(RGB2), 0x55300000u);
}

TEST_F(GteTest, DpctInterpolatesToFarColour) {
    wd(RGB0, 0);
    wd(RGB1, 0);
    wd(RGB2, 0);
    color(0, 0, 0, 0x11);
    fc(0x100, 0x200, 0x300);
    wd(IR0, 0x1000);
    run(cmd(DPCT, 1));
    EXPECT_EQ(d(RGB2), 0x11302010u);
}

TEST_F(GteTest, IntplNoWeightKeepsIr) {
    ir(0x100, 0x200, 0x300, 0);
    color(0, 0, 0, 0x3C);
    run(cmd(INTPL, 1));
    EXPECT_EQ(d(RGB2), 0x3C302010u);
    EXPECT_EQ(sd(MAC1), 0x100);
}

TEST_F(GteTest, IntplFullWeightGivesFarColour) {
    ir(0x100, 0x200, 0x300, 0x1000);
    fc(0, 0, 0);
    run(cmd(INTPL, 1));
    EXPECT_EQ(sd(MAC1), 0);
    EXPECT_EQ(sd(IR3), 0);
    EXPECT_EQ(d(RGB2), 0u);
}

TEST_F(GteTest, IntplHalfWay) {
    ir(0x100, 0x200, 0x300, 0x800);
    fc(0x300, 0x200, 0x100);
    run(cmd(INTPL, 1));
    EXPECT_EQ(sd(MAC1), 0x200);
    EXPECT_EQ(sd(MAC2), 0x200);
    EXPECT_EQ(sd(MAC3), 0x200);
}

TEST_F(GteTest, IntplLmClampsIr) {
    ir(-0x100, 0, 0, 0);
    run(cmd(INTPL, 1, 1));
    EXPECT_EQ(sd(IR1), 0);
    EXPECT_EQ(sd(MAC1), -0x100);
}

// ---------------------------------------------------------------- GPF / GPL

TEST_F(GteTest, GpfScalesIr) {
    ir(0x100, 0x200, 0x300, 0x800);
    color(0, 0, 0);
    run(cmd(GPF, 1));
    EXPECT_EQ(sd(MAC1), 0x80);
    EXPECT_EQ(sd(MAC2), 0x100);
    EXPECT_EQ(sd(MAC3), 0x180);
    EXPECT_EQ(d(RGB2), 0x00181008u);
}

TEST_F(GteTest, GpfSf0) {
    ir(3, 3, 3, 5);
    run(cmd(GPF, 0));
    EXPECT_EQ(sd(MAC1), 15);
    EXPECT_EQ(sd(IR1), 15);
}

TEST_F(GteTest, GpfIgnoresPreviousMac) {
    ir(1, 1, 1, 1);
    wd(MAC1, 0x12345);
    run(cmd(GPF, 0));
    EXPECT_EQ(sd(MAC1), 1);
}

TEST_F(GteTest, GpfKeepsCode) {
    color(1, 2, 3, 0x7E);
    ir(0, 0, 0, 0);
    run(cmd(GPF, 1));
    EXPECT_EQ(d(RGB2) >> 24, 0x7Eu);
}

TEST_F(GteTest, GpfNegativeLm) {
    ir(-0x100, 0, 0, 0x1000);
    run(cmd(GPF, 1, 1));
    EXPECT_EQ(sd(MAC1), -0x100);
    EXPECT_EQ(sd(IR1), 0);
    EXPECT_EQ(flag() & F_IR1, F_IR1);
    EXPECT_EQ(flag() & F_CR, F_CR);
}

TEST_F(GteTest, GplAccumulates) {
    ir(0x100, 0x200, 0x300, 0x800);
    wd(MAC1, 0x10);
    wd(MAC2, 0x20);
    wd(MAC3, 0x30);
    run(cmd(GPL, 1));
    EXPECT_EQ(sd(MAC1), 0x90);
    EXPECT_EQ(sd(MAC2), 0x120);
    EXPECT_EQ(sd(MAC3), 0x1B0);
    EXPECT_EQ(d(RGB2), 0x001B1209u);
}

TEST_F(GteTest, GplSf0) {
    ir(2, 2, 2, 3);
    wd(MAC1, 100);
    wd(MAC2, 200);
    wd(MAC3, 300);
    run(cmd(GPL, 0));
    EXPECT_EQ(sd(MAC1), 106);
    EXPECT_EQ(sd(MAC2), 206);
    EXPECT_EQ(sd(MAC3), 306);
}

TEST_F(GteTest, GplMacOverflowFlag) {
    // base shifted left by 12 sits just below 2^43; a small product keeps it in range, a large one overflows
    ir(1, 0, 0, 1);
    wd(MAC1, 0x7FFFFFFF);
    run(cmd(GPL, 1));
    EXPECT_EQ(flag() & F_MAC1P, 0u);
    ir(0x7FFF, 0, 0, 0x7FFF);
    wd(MAC1, 0x7FFFFFFF);
    run(cmd(GPL, 1));
    EXPECT_EQ(flag() & F_MAC1P, F_MAC1P);
}

// ---------------------------------------------------------------- lighting

class GteLight : public GteTest {
protected:
    void SetUp() override {
        light_id();
        color(0x80, 0x40, 0x20, 0x22);
    }
};

TEST_F(GteLight, NcsPassesLightThrough) {
    vec(0, 0x800, 0x400, 0x200);
    run(cmd(NCS, 1));
    EXPECT_EQ(d(RGB2), 0x22204080u);
    EXPECT_EQ(sd(IR1), 0x800);
    EXPECT_EQ(sd(MAC1), 0x800);
    EXPECT_EQ(flag(), 0u);
}

TEST_F(GteLight, NcsAddsBackgroundColour) {
    vec(0, 0x800, 0x400, 0x200);
    bk(0x100, 0, 0);
    run(cmd(NCS, 1));
    EXPECT_EQ(d(RGB2) & 0xFF, 0x90u);
}

TEST_F(GteLight, NcsUsesV0Only) {
    vec(0, 0x800, 0, 0);
    vec(1, 0, 0x800, 0);
    run(cmd(NCS, 1));
    EXPECT_EQ(d(RGB2), 0x22000080u);
}

TEST_F(GteLight, NcsColourSaturation) {
    vec(0, 0x1000, 0, 0);
    run(cmd(NCS, 1));
    EXPECT_EQ(d(RGB2) & 0xFF, 0xFFu);
    EXPECT_EQ(flag(), F_CR);
}

TEST_F(GteLight, NcsColourSaturationGreenBlue) {
    vec(0, 0, 0x1000, 0x1000);
    run(cmd(NCS, 1));
    EXPECT_EQ(flag(), F_CG | F_CB);
}

TEST_F(GteLight, NcsNegativeLightLmClamps) {
    vec(0, -0x800, 0, 0);
    run(cmd(NCS, 1, 1));
    EXPECT_EQ(d(RGB2) & 0xFF, 0u);
    EXPECT_EQ(flag() & F_IR1, F_IR1);
}

TEST_F(GteLight, NcsLightMatrixSelectsRows) {
    wc(L0, pk(0x1000, 0));  // L11
    wc(L1, pk(0, 0x1000));  // L21 = 1.0
    wc(L2, pk(0, 0));
    wc(L3, pk(0, 0));
    wc(L4, 0);
    vec(0, 0x400, 0, 0);
    run(cmd(NCS, 1));
    EXPECT_EQ(sd(IR1), 0x400);
    EXPECT_EQ(sd(IR2), 0x400);
}

TEST_F(GteLight, NctProcessesThreeVectors) {
    vec(0, 0x800, 0, 0);
    vec(1, 0, 0x800, 0);
    vec(2, 0, 0, 0x800);
    color(0, 0, 0, 0);
    run(cmd(NCT, 1));
    EXPECT_EQ(d(RGB0), 0x00000080u);
    EXPECT_EQ(d(RGB1), 0x00008000u);
    EXPECT_EQ(d(RGB2), 0x00800000u);
}

TEST_F(GteLight, NccsMultipliesByColour) {
    vec(0, 0x800, 0x400, 0x200);
    run(cmd(NCCS, 1));
    EXPECT_EQ(sd(MAC1), 0x400);
    EXPECT_EQ(sd(MAC2), 0x100);
    EXPECT_EQ(sd(MAC3), 0x40);
    EXPECT_EQ(d(RGB2), 0x22041040u);
    EXPECT_EQ(sd(IR1), 0x400);
}

TEST_F(GteLight, NccsFullLightKeepsColour) {
    vec(0, 0x1000, 0x1000, 0x1000);
    run(cmd(NCCS, 1));
    EXPECT_EQ(d(RGB2), 0x22204080u);
}

TEST_F(GteLight, NccsWithBackgroundAmbientOnly) {
    bk(0x1000, 0x1000, 0x1000);
    vec(0, 0, 0, 0);
    run(cmd(NCCS, 1));
    EXPECT_EQ(d(RGB2), 0x22204080u);
}

TEST_F(GteLight, NccsSf0) {
    // sf=0: IR values are 12-bit scaled; colour*IR is not shifted back down.
    vec(0, 1, 0, 0);
    run(cmd(NCCS, 0));
    // light step: MAC1 = 0x1000*1 = 0x1000, IR1 = 0x1000. colour step: (0x80<<4) * 0x1000
    EXPECT_EQ(sd(MAC1), 0x800 * 0x7FFF);  // IR1 saturated by the colour-matrix stage
    EXPECT_EQ(sd(IR1), 0x7FFF);
}

TEST_F(GteLight, NcctThreeVectors) {
    color(0x80, 0x80, 0x80, 0);
    vec(0, 0x800, 0, 0);
    vec(1, 0, 0x800, 0);
    vec(2, 0, 0, 0x800);
    run(cmd(NCCT, 1));
    EXPECT_EQ(d(RGB0), 0x00000040u);
    EXPECT_EQ(d(RGB1), 0x00004000u);
    EXPECT_EQ(d(RGB2), 0x00400000u);
}

TEST_F(GteLight, NcdsNoInterpolationEqualsNccs) {
    vec(0, 0x800, 0x400, 0x200);
    wd(IR0, 0);
    run(cmd(NCDS, 1));
    EXPECT_EQ(d(RGB2), 0x22041040u);
}

TEST_F(GteLight, NcdsFullInterpolationGivesFarColour) {
    vec(0, 0x800, 0x400, 0x200);
    fc(0x800, 0x400, 0x200);
    wd(IR0, 0x1000);
    run(cmd(NCDS, 1));
    EXPECT_EQ(d(RGB2), 0x22204080u);
}

TEST_F(GteLight, NcdsFullInterpolationToBlack) {
    vec(0, 0x800, 0x400, 0x200);
    wd(IR0, 0x1000);
    run(cmd(NCDS, 1));
    EXPECT_EQ(d(RGB2), 0x22000000u);
}

TEST_F(GteLight, NcdsHalfInterpolation) {
    vec(0, 0x1000, 0x1000, 0x1000);
    fc(0, 0, 0);
    wd(IR0, 0x800);
    run(cmd(NCDS, 1));
    EXPECT_EQ(d(RGB2), 0x22102040u);
}

TEST_F(GteLight, NcdtThreeVectors) {
    color(0x80, 0x80, 0x80, 0);
    vec(0, 0x1000, 0, 0);
    vec(1, 0, 0x1000, 0);
    vec(2, 0, 0, 0x1000);
    wd(IR0, 0);
    run(cmd(NCDT, 1));
    EXPECT_EQ(d(RGB0), 0x00000080u);
    EXPECT_EQ(d(RGB1), 0x00008000u);
    EXPECT_EQ(d(RGB2), 0x00800000u);
}

TEST_F(GteLight, CcMultipliesIrByColour) {
    ir(0x800, 0x400, 0x200);
    run(cmd(CC, 1));
    EXPECT_EQ(d(RGB2), 0x22041040u);
}

TEST_F(GteLight, CcAppliesColourMatrixAndBackground) {
    mat(LC0, 0x2000);
    bk(0x100, 0, 0);
    ir(0x400, 0, 0);
    run(cmd(CC, 1));
    // colour stage: 0x100 + 2*0x400 = 0x900, then (0x80 << 4) * 0x900 >> 12
    EXPECT_EQ(sd(IR1), 0x480);
}

TEST_F(GteLight, CdpWithoutInterpolation) {
    ir(0x800, 0x400, 0x200, 0);
    run(cmd(CDP, 1));
    EXPECT_EQ(d(RGB2), 0x22041040u);
}

TEST_F(GteLight, CdpFullInterpolation) {
    ir(0x800, 0x400, 0x200, 0x1000);
    fc(0x800, 0x400, 0x200);
    run(cmd(CDP, 1));
    EXPECT_EQ(d(RGB2), 0x22204080u);
}

TEST_F(GteLight, DcplWithoutInterpolation) {
    ir(0x800, 0x400, 0x200, 0);
    run(cmd(DCPL, 1));
    EXPECT_EQ(d(RGB2), 0x22041040u);
}

TEST_F(GteLight, DcplFullInterpolation) {
    ir(0x800, 0x400, 0x200, 0x1000);
    fc(0x100, 0x200, 0x300);
    run(cmd(DCPL, 1));
    EXPECT_EQ(d(RGB2), 0x22302010u);
}

TEST_F(GteLight, DcplDoesNotTouchLightMatrix) {
    ir(0x800, 0x400, 0x200, 0);
    mat(LC0, 0);
    run(cmd(DCPL, 1));
    EXPECT_EQ(d(RGB2), 0x22041040u);
}

TEST_F(GteLight, ColourFifoShiftsOnLightCommands) {
    vec(0, 0x800, 0, 0);
    run(cmd(NCS, 1));
    vec(0, 0, 0x800, 0);
    run(cmd(NCS, 1));
    vec(0, 0, 0, 0x800);
    run(cmd(NCS, 1));
    EXPECT_EQ(d(RGB0), 0x22000080u);
    EXPECT_EQ(d(RGB1), 0x22008000u);
    EXPECT_EQ(d(RGB2), 0x22800000u);
}

TEST_F(GteLight, RotatedLightDirection) {
    // light matrix swaps X and Y
    wc(L0, pk(0, 0x1000));
    wc(L1, pk(0, 0));
    wc(L2, pk(0, 0));
    wc(L3, pk(0, 0));
    wc(L4, 0x1000);
    wc(L0, pk(0, 0x1000));
    wc(L1, pk(0, 0x1000));  // L13=0, L21=1.0
    wc(L2, pk(0, 0));
    vec(0, 0x800, 0, 0);
    run(cmd(NCS, 1));
    EXPECT_EQ(sd(IR2), 0x800);
    EXPECT_EQ(sd(IR1), 0);
}

// ---------------------------------------------------------------- flag summary / misc

TEST_F(GteTest, UnknownCommandDoesNothingButClearsFlag) {
    wc(FLAG, 0x2000);
    run(cmd(0x00));
    EXPECT_EQ(c(FLAG), 0u);
}

TEST_F(GteTest, FlagBit31SetByErrorBit) {
    rot_id();
    vec(0, 8, 0, 0);
    run(mv(0, 0, 3, 0));
    EXPECT_EQ(flag() >> 31, 1u);
}

TEST_F(GteTest, FlagBit31ClearForNonErrorBitsOnly) {
    ir(0, 0, 0x100);
    run(cmd(SQR));
    EXPECT_EQ(flag(), F_IR3);
    EXPECT_EQ(flag() >> 31, 0u);
}

TEST_F(GteTest, FlagBit31ClearForColourSaturationOnly) {
    light_id();
    color(0, 0, 0);
    vec(0, 0x1000, 0, 0);
    run(cmd(NCS, 1));
    EXPECT_EQ(flag(), F_CR);
}

TEST_F(GteTest, CommandIgnoresHighBitsOfCommandWord) {
    wd(SXY0, pk(0, 0));
    wd(SXY1, pk(10, 0));
    wd(SXY2, pk(0, 10));
    run((0xFFFFFFFFu & ~0x3Fu) | NCLIP);
    EXPECT_EQ(sd(MAC0), 100);
}

TEST_F(GteTest, RtpsIr1MatchesIrgbRoundTrip) {
    proj_setup();
    vec(0, 0x80, 0x100, 0x1000);
    run(cmd(RTPS, 1));
    EXPECT_EQ(d(ORGB), 1u | 2u << 5 | 31u << 10);  // IR3 = 0x1000 saturates to 31
}

}  // namespace
