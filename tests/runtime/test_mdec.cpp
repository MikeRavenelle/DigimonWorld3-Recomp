// MDEC: registers, table uploads, RLE/zigzag/dequant, IDCT vs float reference, YUV->RGB, output formats, DMA.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/mdec.h"
#include "hw_test_util.h"

using dw3::Dma;
using dw3::Mdec;

namespace {

constexpr uint32_t kData = 0x1F801820u, kCtrl = 0x1F801824u;
constexpr uint16_t kEob = 0xFE00;
constexpr uint8_t kZig[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                              12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                              35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                              58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Rig : dw3test::Clock {
    dw3::IrqController irq{bus};
    Dma dma{bus, sched, irq};
    Mdec mdec{bus, sched, dma};

    void send(uint32_t w) { w32(kData, w); }
    void send_all(const std::vector<uint32_t>& v) {
        for (uint32_t w : v) send(w);
    }
    uint32_t status() { return r32(kCtrl); }
    void settle() { run_to(ctx().cycles + 10'000'000); }

    static uint32_t decode_cmd(uint32_t depth, bool sgn, bool b15, uint32_t words) {
        return (1u << 29) | (depth << 27) | (uint32_t{sgn} << 26) | (uint32_t{b15} << 25) | words;
    }
    void set_quant(uint8_t luma, uint8_t chroma = 0, bool both = false) {
        send((2u << 29) | (both ? 1u : 0u));
        uint32_t lw = luma * 0x01010101u, cw = chroma * 0x01010101u;
        for (int i = 0; i < 16; ++i) send(lw);
        if (both)
            for (int i = 0; i < 16; ++i) send(cw);
    }
    void set_quant_dc(uint8_t dc_luma, uint8_t other) {  // qt[0] differs from the rest
        send(2u << 29 | 1u);
        for (int t = 0; t < 2; ++t)
            for (int i = 0; i < 16; ++i) {
                uint32_t w = other * 0x01010101u;
                if (i == 0) w = (w & ~0xFFu) | dc_luma;
                send(w);
            }
    }
    void set_scale() {
        send(3u << 29);
        auto t = Mdec::default_scale_table();
        for (int i = 0; i < 32; ++i)
            send(static_cast<uint16_t>(t[2 * i]) | (static_cast<uint32_t>(static_cast<uint16_t>(t[2 * i + 1])) << 16));
    }
    // Feed a halfword stream as a decode command.
    void decode(uint32_t depth, bool sgn, bool b15, std::vector<uint16_t> hw) {
        if (hw.size() & 1) hw.push_back(kEob);
        send(decode_cmd(depth, sgn, b15, static_cast<uint32_t>(hw.size() / 2)));
        for (size_t i = 0; i < hw.size(); i += 2) send(hw[i] | (static_cast<uint32_t>(hw[i + 1]) << 16));
    }
    std::vector<uint32_t> drain() {
        settle();
        std::vector<uint32_t> v;
        while (mdec.available_words()) v.push_back(r32(kData));
        return v;
    }
};

uint16_t dc(int raw, uint32_t qs = 1) { return static_cast<uint16_t>((qs << 10) | (raw & 0x3FF)); }
std::vector<uint16_t> block_dc(int raw, uint32_t qs = 1) { return {dc(raw, qs), kEob}; }
void append(std::vector<uint16_t>& a, const std::vector<uint16_t>& b) { a.insert(a.end(), b.begin(), b.end()); }

// Macroblock with flat Cr, Cb and four equal Y blocks.
std::vector<uint16_t> flat_mb(int cr, int cb, int y) {
    std::vector<uint16_t> v;
    append(v, block_dc(cr));
    append(v, block_dc(cb));
    for (int i = 0; i < 4; ++i) append(v, block_dc(y));
    return v;
}

std::vector<uint8_t> bytes(const std::vector<uint32_t>& w) {
    std::vector<uint8_t> b(w.size() * 4);
    if (!w.empty()) std::memcpy(b.data(), w.data(), b.size());
    return b;
}
uint16_t px16(const std::vector<uint8_t>& b, int i) { return static_cast<uint16_t>(b[2 * i] | (b[2 * i + 1] << 8)); }

}  // namespace

// ---- registers ----

TEST(Mdec, StatusAfterResetValue) {
    Rig r;
    r.w32(kCtrl, 0x80000000u);
    EXPECT_EQ(r.status(), 0x80040000u);
}

TEST(Mdec, StatusInitialIsEmptyIdle) {
    Rig r;
    EXPECT_EQ(r.status() & 0x80000000u, 0x80000000u);
    EXPECT_EQ(r.status() & (1u << 29), 0u);
    EXPECT_EQ(r.status() & (1u << 30), 0u);
}

TEST(Mdec, EnableDataInSetsRequest) {
    Rig r;
    r.w32(kCtrl, 1u << 30);
    EXPECT_TRUE(r.status() & (1u << 28));
    r.w32(kCtrl, 0);
    EXPECT_FALSE(r.status() & (1u << 28));
}

TEST(Mdec, EnableDataOutNoRequestWhenEmpty) {
    Rig r;
    r.w32(kCtrl, 1u << 29);
    EXPECT_FALSE(r.status() & (1u << 27));
}

TEST(Mdec, ResetClearsEnables) {
    Rig r;
    r.w32(kCtrl, 3u << 29);
    r.w32(kCtrl, 0x80000000u);
    EXPECT_EQ(r.status() & (3u << 27), 0u);
}

TEST(Mdec, ResetWithEnableBitsKeepsThem) {
    Rig r;
    r.w32(kCtrl, 0x80000000u | (1u << 30));
    EXPECT_TRUE(r.status() & (1u << 28));
}

TEST(Mdec, MappedAtBaseAndSize8) {
    Rig r;
    EXPECT_EQ(r.bus.unmapped_reads(), 0u);
    r.r32(kData);
    r.r32(kCtrl);
    r.w32(kData, 0);
    r.w32(kCtrl, 0);
    EXPECT_EQ(r.bus.unmapped_reads(), 0u);
    EXPECT_EQ(r.bus.unmapped_writes(), 0u);
}

TEST(Mdec, ReadDataWhenEmptyIsZero) {
    Rig r;
    EXPECT_EQ(r.r32(kData), 0u);
}

TEST(Mdec, StatusHalfwordAndByteReads) {
    Rig r;
    r.w32(kCtrl, 0x80000000u | (1u << 30));
    uint32_t s = r.status();
    EXPECT_EQ(r.r16(kCtrl), s & 0xFFFF);
    EXPECT_EQ(r.r16(kCtrl + 2), s >> 16);
    EXPECT_EQ(r.bus.read(kCtrl + 3, 1), s >> 24);
    EXPECT_EQ(r.bus.read(kCtrl + 2, 1), (s >> 16) & 0xFF);
}

TEST(Mdec, ByteWritesAssembleControlWord) {
    Rig r;
    r.w8(kCtrl, 0);
    r.w8(kCtrl + 1, 0);
    r.w8(kCtrl + 2, 0);
    r.w8(kCtrl + 3, 0x40);  // bit 30
    EXPECT_TRUE(r.status() & (1u << 28));
}

TEST(Mdec, HalfwordWritesAssembleCommand) {
    Rig r;
    r.w16(kData, 0x0000);
    r.w16(kData + 2, 0x6000);  // command 3 = set scale table
    EXPECT_TRUE(r.status() & (1u << 29));
}

TEST(Mdec, CommandLatchesFormatBits) {
    Rig r;
    r.send(Rig::decode_cmd(Mdec::k15Bit, true, true, 0));
    uint32_t s = r.status();
    EXPECT_EQ((s >> 25) & 3, 3u);
    EXPECT_TRUE(s & (1u << 24));
    EXPECT_TRUE(s & (1u << 23));
}

TEST(Mdec, FormatBitsClearWithNewCommand) {
    Rig r;
    r.send(Rig::decode_cmd(3, true, true, 0));
    r.send(Rig::decode_cmd(0, false, false, 0));
    EXPECT_EQ(r.status() & (0x1Fu << 23), 0u);
}

class MdecDepthBits : public ::testing::TestWithParam<uint32_t> {};
TEST_P(MdecDepthBits, StatusDepthField) {
    Rig r;
    r.send(Rig::decode_cmd(GetParam(), false, false, 0));
    EXPECT_EQ((r.status() >> 25) & 3, GetParam());
}
INSTANTIATE_TEST_SUITE_P(Depths, MdecDepthBits, ::testing::Values(0u, 1u, 2u, 3u));

TEST(Mdec, ParamRemainingCountsDown) {
    Rig r;
    r.send(Rig::decode_cmd(2, false, false, 4));
    EXPECT_EQ(r.status() & 0xFFFF, 3u);
    r.send(0xFE00FE00u);
    EXPECT_EQ(r.status() & 0xFFFF, 2u);
    r.send(0xFE00FE00u);
    r.send(0xFE00FE00u);
    EXPECT_EQ(r.status() & 0xFFFF, 0u);
}

TEST(Mdec, CommandBusyWhileParamsPending) {
    Rig r;
    r.send(Rig::decode_cmd(2, false, false, 2));
    EXPECT_TRUE(r.status() & (1u << 29));
}

TEST(Mdec, CommandCompleteRemainingIsFFFF) {
    Rig r;
    r.send(Rig::decode_cmd(2, false, false, 1));
    r.send(0xFE00FE00u);
    r.settle();
    EXPECT_EQ(r.status() & 0xFFFF, 0xFFFFu);
    EXPECT_FALSE(r.status() & (1u << 29));
}

TEST(Mdec, CurrentBlockStartsAtCr) {
    Rig r;
    r.send(Rig::decode_cmd(2, false, false, 100));
    EXPECT_EQ((r.status() >> 16) & 7, 4u);
}

TEST(Mdec, CurrentBlockAdvancesCrCbY) {
    Rig r;
    r.set_quant(8, 8, true);
    r.send(Rig::decode_cmd(2, false, false, 100));
    r.send(dc(1) | (uint32_t{kEob} << 16));  // Cr done
    EXPECT_EQ((r.status() >> 16) & 7, 5u);
    r.send(dc(1) | (uint32_t{kEob} << 16));  // Cb done
    EXPECT_EQ((r.status() >> 16) & 7, 0u);
    r.send(dc(1) | (uint32_t{kEob} << 16));  // Y1 done
    EXPECT_EQ((r.status() >> 16) & 7, 1u);
}

TEST(Mdec, NoopCommandsTakeNoParameters) {
    for (uint32_t c : {0u, 4u, 5u, 6u, 7u}) {
        Rig r;
        r.send(c << 29);
        EXPECT_FALSE(r.status() & (1u << 29)) << c;
        EXPECT_EQ(r.mdec.queued_words(), 0u) << c;
    }
}

TEST(Mdec, NoopThenDecodeWorks) {
    Rig r;
    r.set_quant(8, 8, true);
    r.send(0u);
    r.send(5u << 29);
    r.decode(Mdec::k8Bit, false, false, block_dc(32));
    EXPECT_EQ(r.drain().size(), 16u);
}

TEST(Mdec, DecodeWithZeroWordsDoesNothing) {
    Rig r;
    r.send(Rig::decode_cmd(2, false, false, 0));
    EXPECT_FALSE(r.status() & (1u << 29));
    r.settle();
    EXPECT_EQ(r.mdec.queued_words(), 0u);
}

// ---- table uploads ----

TEST(Mdec, QuantLumaOnlyConsumes16Words) {
    Rig r;
    r.send(2u << 29);
    for (int i = 0; i < 15; ++i) r.send(0x01020304u);
    EXPECT_TRUE(r.status() & (1u << 29));
    r.send(0x01020304u);
    EXPECT_FALSE(r.status() & (1u << 29));
}

TEST(Mdec, QuantBothConsumes32Words) {
    Rig r;
    r.send((2u << 29) | 1u);
    for (int i = 0; i < 31; ++i) r.send(0);
    EXPECT_TRUE(r.status() & (1u << 29));
    r.send(0);
    EXPECT_FALSE(r.status() & (1u << 29));
}

TEST(Mdec, QuantLumaBytesStoredInOrder) {
    Rig r;
    r.send(2u << 29);
    for (uint32_t i = 0; i < 16; ++i) r.send((4 * i) | ((4 * i + 1) << 8) | ((4 * i + 2) << 16) | ((4 * i + 3) << 24));
    for (int i = 0; i < 64; ++i) EXPECT_EQ(r.mdec.luma_table()[i], i);
}

TEST(Mdec, QuantBothSplitsLumaChroma) {
    Rig r;
    r.set_quant(7, 9, true);
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(r.mdec.luma_table()[i], 7);
        EXPECT_EQ(r.mdec.chroma_table()[i], 9);
    }
}

TEST(Mdec, QuantLumaOnlyKeepsChroma) {
    Rig r;
    r.set_quant(7, 9, true);
    r.set_quant(3);
    EXPECT_EQ(r.mdec.luma_table()[10], 3);
    EXPECT_EQ(r.mdec.chroma_table()[10], 9);
}

TEST(Mdec, ScaleTableStoredShifted) {
    Rig r;
    r.send(3u << 29);
    for (int i = 0; i < 32; ++i) r.send(0x0800'1000u + i);
    EXPECT_EQ(r.mdec.scale_table()[0], (0x1000 >> 3));
    EXPECT_EQ(r.mdec.scale_table()[1], (0x0800 >> 3));
    EXPECT_EQ(r.mdec.scale_table()[62], (0x1000 + 31) >> 3);
}

TEST(Mdec, ScaleTableSignedHalfwords) {
    Rig r;
    r.send(3u << 29);
    for (int i = 0; i < 32; ++i) r.send(0x8000'8000u);
    EXPECT_EQ(r.mdec.scale_table()[5], -4096);
}

TEST(Mdec, ScaleConsumes32Words) {
    Rig r;
    r.send(3u << 29);
    for (int i = 0; i < 31; ++i) r.send(0);
    EXPECT_TRUE(r.status() & (1u << 29));
    r.send(0);
    EXPECT_FALSE(r.status() & (1u << 29));
}

TEST(Mdec, DefaultScaleTableDcRow) {
    auto t = Mdec::default_scale_table();
    for (int x = 0; x < 8; ++x) EXPECT_EQ(t[x], 0x5A82);
    EXPECT_EQ(t[8], 0x7D8A);
}

TEST(Mdec, UploadsDoNotProduceOutput) {
    Rig r;
    r.set_quant(2, 2, true);
    r.set_scale();
    r.settle();
    EXPECT_EQ(r.mdec.queued_words(), 0u);
}

TEST(Mdec, ResetKeepsTables) {
    Rig r;
    r.set_quant(7, 9, true);
    r.w32(kCtrl, 0x80000000u);
    EXPECT_EQ(r.mdec.luma_table()[0], 7);
}

// ---- DC-only flat output ----
// Hand check: qt=8, raw DC 32 -> coef 256 -> pass1 (256*2896+4095)>>13 = 90, pass2 (90*2896+4095)>>13 = 32.

TEST(Mdec, FlatMono8Unsigned) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, block_dc(32));
    auto w = r.drain();
    ASSERT_EQ(w.size(), 16u);
    for (uint32_t x : w) EXPECT_EQ(x, 0xA0A0A0A0u);
}

TEST(Mdec, FlatMono8Signed) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, true, false, block_dc(32));
    for (uint32_t x : r.drain()) EXPECT_EQ(x, 0x20202020u);
}

TEST(Mdec, FlatMono8NegativeSigned) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, true, false, block_dc(-32));
    for (uint32_t x : r.drain()) EXPECT_EQ(x, 0xE0E0E0E0u);  // -32
}

TEST(Mdec, FlatMono8NegativeUnsigned) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, block_dc(-32));
    for (uint32_t x : r.drain()) EXPECT_EQ(x, 0x60606060u);  // 128-32
}

TEST(Mdec, FlatMono4Bit) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k4Bit, false, false, block_dc(32));
    auto w = r.drain();
    ASSERT_EQ(w.size(), 8u);
    for (uint32_t x : w) EXPECT_EQ(x, 0xAAAAAAAAu);
}

TEST(Mdec, FlatColour24Unsigned) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, false, flat_mb(0, 0, 32));
    auto b = bytes(r.drain());
    ASSERT_EQ(b.size(), 768u);
    for (uint8_t v : b) EXPECT_EQ(v, 0xA0);
}

TEST(Mdec, FlatColour24Signed) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, true, false, flat_mb(0, 0, 32));
    for (uint8_t v : bytes(r.drain())) EXPECT_EQ(v, 0x20);
}

TEST(Mdec, FlatColour15Unsigned) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k15Bit, false, false, flat_mb(0, 0, 32));
    auto b = bytes(r.drain());
    ASSERT_EQ(b.size(), 512u);
    for (int i = 0; i < 256; ++i) EXPECT_EQ(px16(b, i), 0x5294);  // 20,20,20
}

TEST(Mdec, FlatColour15Signed) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k15Bit, true, false, flat_mb(0, 0, 32));
    auto b = bytes(r.drain());
    for (int i = 0; i < 256; ++i) EXPECT_EQ(px16(b, i), 0x1084);
}

TEST(Mdec, FlatColour15Bit15Set) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k15Bit, false, true, flat_mb(0, 0, 32));
    auto b = bytes(r.drain());
    for (int i = 0; i < 256; ++i) EXPECT_EQ(px16(b, i), 0xD294);
}

TEST(Mdec, Bit15IgnoredFor24Bit) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, true, flat_mb(0, 0, 32));
    for (uint8_t v : bytes(r.drain())) EXPECT_EQ(v, 0xA0);
}

TEST(Mdec, ColourOutputSizes) {
    for (uint32_t d : {2u, 3u}) {
        Rig r;
        r.set_quant(8, 8, true);
        r.decode(d, false, false, flat_mb(0, 0, 0));
        EXPECT_EQ(r.drain().size(), d == 2 ? 192u : 128u);
    }
}

TEST(Mdec, ZeroDcGivesMidGrey) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, false, flat_mb(0, 0, 0));
    for (uint8_t v : bytes(r.drain())) EXPECT_EQ(v, 0x80);
}

class MdecFlatDc : public ::testing::TestWithParam<int> {};
TEST_P(MdecFlatDc, MonoMatchesDcOverEight) {
    int raw = GetParam();  // coef = raw*8 -> pixel ~ raw
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, true, false, block_dc(raw));
    auto b = bytes(r.drain());
    ASSERT_EQ(b.size(), 64u);
    int expect = std::max(-128, std::min(127, raw));
    for (uint8_t v : b) EXPECT_NEAR(static_cast<int8_t>(v), expect, 1) << raw;
    for (uint8_t v : b) EXPECT_EQ(v, b[0]);  // perfectly flat
}
INSTANTIATE_TEST_SUITE_P(Values, MdecFlatDc,
                         ::testing::Values(-128, -100, -77, -64, -33, -16, -8, -3, -1, 0, 1, 3, 8, 16, 31, 33, 64, 77,
                                           100, 127));

class MdecDcQuant : public ::testing::TestWithParam<int> {};
TEST_P(MdecDcQuant, DcUsesQuantTableEntryZeroNotScale) {
    int q = GetParam();
    Rig r;
    r.set_quant_dc(static_cast<uint8_t>(q), 1);
    // qscale 63 must not affect DC.
    r.decode(Mdec::k8Bit, true, false, block_dc(16, 63));
    int expect = 16 * q / 8;
    for (uint8_t v : bytes(r.drain())) EXPECT_NEAR(static_cast<int8_t>(v), std::min(127, expect), 1);
}
INSTANTIATE_TEST_SUITE_P(Q, MdecDcQuant, ::testing::Values(1, 2, 4, 8, 16, 24, 32, 40, 63));

TEST(Mdec, DcClampsToPlus3FF) {
    Rig r;
    r.set_quant(255, 255, true);
    r.decode(Mdec::k8Bit, true, false, block_dc(511));
    for (uint8_t v : bytes(r.drain())) EXPECT_EQ(static_cast<int8_t>(v), 127);  // 0x3FF/8 = 127.9 -> clamps
}

TEST(Mdec, DcClampsToMinus400) {
    Rig r;
    r.set_quant(255, 255, true);
    r.decode(Mdec::k8Bit, true, false, block_dc(-512));
    for (uint8_t v : bytes(r.drain())) EXPECT_EQ(static_cast<int8_t>(v), -128);
}

TEST(Mdec, QscaleZeroDcDoublesRawIgnoringQuant) {
    Rig r;
    r.set_quant(255, 255, true);
    // q_scale 0: DC = raw*2 = 512 -> 64 flat.
    r.decode(Mdec::k8Bit, true, false, block_dc(256, 0));
    for (uint8_t v : bytes(r.drain())) EXPECT_NEAR(static_cast<int8_t>(v), 64, 1);
}

TEST(Mdec, ScaleTableUploadAffectsOutput) {
    Rig r;
    r.set_quant(8, 8, true);
    r.send(3u << 29);
    for (int i = 0; i < 32; ++i) r.send(0);  // all-zero matrix
    r.decode(Mdec::k8Bit, true, false, block_dc(100));
    for (uint8_t v : bytes(r.drain())) EXPECT_EQ(v, 0);
}

TEST(Mdec, ScaleTableDoubledDoublesOutput) {
    Rig r;
    r.set_quant(8, 8, true);
    r.send(3u << 29);
    auto t = Mdec::default_scale_table();
    for (int i = 0; i < 32; ++i) {
        // Scaling only the DC row by sqrt(2) in each pass doubles the DC response.
        auto s = [&](int k) { return static_cast<uint16_t>(k < 8 ? t[k] * 1.41421356 : t[k]); };
        r.send(s(2 * i) | (static_cast<uint32_t>(s(2 * i + 1)) << 16));
    }
    r.decode(Mdec::k8Bit, true, false, block_dc(20));
    for (uint8_t v : bytes(r.drain())) EXPECT_NEAR(static_cast<int8_t>(v), 40, 1);
}

// ---- RLE parsing ----

TEST(Mdec, PaddingBeforeBlockSkipped) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, {kEob, kEob, dc(32), kEob});
    for (uint32_t x : r.drain()) EXPECT_EQ(x, 0xA0A0A0A0u);
}

TEST(Mdec, EndOfBlockTerminatesBlock) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, {dc(32), kEob, dc(-32), kEob});
    auto w = r.drain();
    ASSERT_EQ(w.size(), 32u);
    EXPECT_EQ(w[0], 0xA0A0A0A0u);
    EXPECT_EQ(w[16], 0x60606060u);
}

TEST(Mdec, RunExceedingBlockEndsIt) {
    Rig r;
    r.set_quant(8, 8, true);
    // Run 62 from n=0 lands on 63 (valid coefficient), then another halfword ends the block.
    std::vector<uint16_t> v{dc(32), static_cast<uint16_t>((62u << 10) | 0), kEob};
    r.decode(Mdec::k8Bit, false, false, v);
    EXPECT_EQ(r.drain().size(), 16u);
    EXPECT_EQ(r.mdec.macroblocks_decoded(), 1u);
}

TEST(Mdec, LongRunSkipsToEnd) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, {dc(32), static_cast<uint16_t>(63u << 10 | 5), dc(-32), kEob});
    // Block 1 ends at the second halfword (n=64), the rest start block 2.
    auto w = r.drain();
    ASSERT_EQ(w.size(), 32u);
    EXPECT_EQ(w[0], 0xA0A0A0A0u);
}

TEST(Mdec, SingleBlockMonoCountsOneMacroblock) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, block_dc(1));
    EXPECT_EQ(r.mdec.macroblocks_decoded(), 1u);
}

TEST(Mdec, ColourNeedsSixBlocks) {
    Rig r;
    r.set_quant(8, 8, true);
    std::vector<uint16_t> v;
    for (int i = 0; i < 5; ++i) append(v, block_dc(0));
    r.decode(Mdec::k24Bit, false, false, v);
    EXPECT_EQ(r.drain().size(), 0u);
    EXPECT_EQ(r.mdec.macroblocks_decoded(), 0u);
}

// ---- IDCT vs reference ----

static double ref_idct(const double F[64], int x, int y) {
    const double pi = std::acos(-1.0);
    double s = 0;
    for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 8; ++u) {
            double cu = u == 0 ? std::sqrt(0.5) : 1, cv = v == 0 ? std::sqrt(0.5) : 1;
            s += cu * cv * F[v * 8 + u] * std::cos((2 * x + 1) * u * pi / 16) * std::cos((2 * y + 1) * v * pi / 16);
        }
    return s / 4;
}

class MdecZigzag : public ::testing::TestWithParam<int> {};
TEST_P(MdecZigzag, SingleAcCoefficientMatchesReference) {
    int n = GetParam();
    Rig r;
    r.set_quant(8, 8, true);  // AC = (raw*8*1+4)>>3 = raw
    const int raw = 100;
    // DC=0 then run n-1 -> position n.
    r.decode(Mdec::k8Bit, true, false,
             {dc(0), static_cast<uint16_t>(((n - 1) << 10) | raw), kEob});
    auto b = bytes(r.drain());
    ASSERT_EQ(b.size(), 64u);
    double F[64] = {};
    F[kZig[n]] = raw;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            double e = std::max(-128.0, std::min(127.0, ref_idct(F, x, y)));
            EXPECT_NEAR(static_cast<int8_t>(b[y * 8 + x]), e, 1.0) << "n=" << n << " x=" << x << " y=" << y;
        }
}
INSTANTIATE_TEST_SUITE_P(AllPositions, MdecZigzag, ::testing::Range(1, 64));

TEST(Mdec, MixedAcPatternMatchesReference) {
    Rig r;
    r.set_quant(8, 8, true);
    std::vector<uint16_t> hw{dc(20)};
    double F[64] = {};
    F[0] = 160;
    int n = 0;
    const int runs[] = {0, 1, 2, 0, 5};
    const int vals[] = {40, -30, 25, -60, 15};
    for (int i = 0; i < 5; ++i) {
        n += runs[i] + 1;
        hw.push_back(static_cast<uint16_t>((runs[i] << 10) | (vals[i] & 0x3FF)));
        F[kZig[n]] = vals[i];
    }
    hw.push_back(kEob);
    r.decode(Mdec::k8Bit, true, false, hw);
    auto b = bytes(r.drain());
    ASSERT_EQ(b.size(), 64u);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            EXPECT_NEAR(static_cast<int8_t>(b[y * 8 + x]), std::max(-128.0, std::min(127.0, ref_idct(F, x, y))), 1.0);
}

TEST(Mdec, ZigzagOrderFirstThreeAcPositions) {
    // n=1 is horizontal frequency (varies with x), n=2 is vertical (varies with y).
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, true, false, {dc(0), static_cast<uint16_t>(100), kEob});
    auto b = bytes(r.drain());
    EXPECT_NE(b[0], b[7]);  // varies along x
    EXPECT_EQ(b[0], b[56]);  // constant along y
    r.decode(Mdec::k8Bit, true, false, {dc(0), static_cast<uint16_t>(100 | (1 << 10)), kEob});
    auto c = bytes(r.drain());
    EXPECT_NE(c[0], c[56]);
    EXPECT_EQ(c[0], c[7]);
}

TEST(Mdec, AcDequantUsesQscaleAndTable) {
    Rig r;
    r.set_quant(16, 16, true);
    // AC raw 10, qscale 2: (10*16*2+4)>>3 = 40; compare with reference single coefficient 40.
    r.decode(Mdec::k8Bit, true, false, {dc(0, 2), static_cast<uint16_t>((0u << 10) | 10), kEob});
    // Run field is bits 10-15 of the AC halfword: qscale comes from the DC halfword only.
    auto b = bytes(r.drain());
    double F[64] = {};
    F[1] = 40;
    for (int x = 0; x < 8; ++x) EXPECT_NEAR(static_cast<int8_t>(b[x]), ref_idct(F, x, 0), 1.0);
}

TEST(Mdec, AcDequantRoundsHalfUp) {
    Rig r;
    r.set_quant(1, 1, true);
    // (raw*1*1 + 4) >> 3 : raw 4 -> 1, raw 3 -> 0.
    r.decode(Mdec::k8Bit, true, false, {dc(0), 3, kEob});
    for (uint8_t v : bytes(r.drain())) EXPECT_EQ(v, 0);
    r.decode(Mdec::k8Bit, true, false, {dc(0), 4, kEob});
    auto b = bytes(r.drain());
    double F[64] = {};
    F[1] = 1;
    for (int x = 0; x < 8; ++x) EXPECT_NEAR(static_cast<int8_t>(b[x]), ref_idct(F, x, 0), 1.0);
}

TEST(Mdec, QscaleZeroBypassesZigzag) {
    Rig r;
    r.set_quant(8, 8, true);
    // q_scale 0: coefficient n=2 is stored at raster index 2 (not zigzag[2]=8), value raw*2.
    r.decode(Mdec::k8Bit, true, false, {dc(0, 0), static_cast<uint16_t>(1u << 10 | 50), kEob});
    auto b = bytes(r.drain());
    double F[64] = {};
    F[2] = 100;
    for (int x = 0; x < 8; ++x) EXPECT_NEAR(static_cast<int8_t>(b[x]), ref_idct(F, x, 0), 1.0);
}

TEST(Mdec, ChromaUsesChromaQuantTable) {
    Rig r;
    r.set_quant(8, 16, true);
    // Cr raw 16 with chroma qt 16 -> 256 -> 32 ; Cb 0; Y 0 -> R = 0 + 45.
    std::vector<uint16_t> v;
    append(v, block_dc(16));
    append(v, block_dc(0));
    for (int i = 0; i < 4; ++i) append(v, block_dc(0));
    r.decode(Mdec::k24Bit, true, false, v);
    auto b = bytes(r.drain());
    EXPECT_EQ(static_cast<int8_t>(b[0]), 45);
}

// ---- YUV -> RGB ----

TEST(Mdec, CrOnlyUnsigned24) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, false, flat_mb(32, 0, 0));
    auto b = bytes(r.drain());
    // cr=32: R=+45, G=-23, B=0 (hand computed with 16.16 coefficients).
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(b[3 * i], 128 + 45);
        EXPECT_EQ(b[3 * i + 1], 128 - 23);
        EXPECT_EQ(b[3 * i + 2], 128);
    }
}

TEST(Mdec, CbOnlyUnsigned24) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, false, flat_mb(0, 32, 0));
    auto b = bytes(r.drain());
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(b[3 * i], 128);
        EXPECT_EQ(b[3 * i + 1], 128 - 11);
        EXPECT_EQ(b[3 * i + 2], 128 + 57);
    }
}

TEST(Mdec, YPlusCrNoClamp) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, true, false, flat_mb(32, 0, 32));
    auto b = bytes(r.drain());
    EXPECT_EQ(static_cast<int8_t>(b[0]), 77);
    EXPECT_EQ(static_cast<int8_t>(b[1]), 9);
    EXPECT_EQ(static_cast<int8_t>(b[2]), 32);
}

TEST(Mdec, ChannelsClampHigh) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, false, flat_mb(100, 100, 100));
    auto b = bytes(r.drain());
    EXPECT_EQ(b[0], 255);  // R saturates
    EXPECT_EQ(b[2], 255);  // B saturates
    EXPECT_LT(b[1], 255);
}

TEST(Mdec, ChannelsClampLow) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, false, flat_mb(-100, -100, -100));
    auto b = bytes(r.drain());
    EXPECT_EQ(b[0], 0);
    EXPECT_EQ(b[2], 0);
}

TEST(Mdec, Pixel15PacksRedLowBlueHigh) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k15Bit, false, false, flat_mb(32, 0, 0));
    auto b = bytes(r.drain());
    uint16_t p = px16(b, 0);
    EXPECT_EQ(p & 31, (128 + 45) >> 3);
    EXPECT_EQ((p >> 5) & 31, (128 - 23) >> 3);
    EXPECT_EQ((p >> 10) & 31, 128 >> 3);
}

TEST(Mdec, FloatReferenceColourConversion) {
    // Compare to the floating-point conversion for a range of chroma values.
    for (int c = -60; c <= 60; c += 12) {
        Rig r;
        r.set_quant(8, 8, true);
        r.decode(Mdec::k24Bit, true, false, flat_mb(c, -c / 2, 10));
        auto b = bytes(r.drain());
        double cr = c, cb = -c / 2, y = 10;
        auto cl = [](double v) { return std::max(-128.0, std::min(127.0, v)); };
        EXPECT_NEAR(static_cast<int8_t>(b[0]), cl(y + 1.402 * cr), 1.5) << c;
        EXPECT_NEAR(static_cast<int8_t>(b[1]), cl(y - 0.3437 * cb - 0.7143 * cr), 1.5) << c;
        EXPECT_NEAR(static_cast<int8_t>(b[2]), cl(y + 1.772 * cb), 1.5) << c;
    }
}

TEST(Mdec, ChromaSharedAcrossTwoByTwo) {
    Rig r;
    r.set_quant(8, 8, true);
    // Cr with a horizontal gradient (AC n=1), flat elsewhere.
    std::vector<uint16_t> v{dc(0), static_cast<uint16_t>(120), kEob};
    append(v, block_dc(0));
    for (int i = 0; i < 4; ++i) append(v, block_dc(0));
    r.decode(Mdec::k24Bit, false, false, v);
    auto b = bytes(r.drain());
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; x += 2) EXPECT_EQ(b[3 * (y * 16 + x)], b[3 * (y * 16 + x + 1)]);
    for (int x = 0; x < 16; ++x) EXPECT_EQ(b[3 * x], b[3 * (16 + x)]);
}

TEST(Mdec, YBlockPlacement) {
    Rig r;
    r.set_quant(8, 8, true);
    std::vector<uint16_t> v;
    append(v, block_dc(0));
    append(v, block_dc(0));
    for (int y : {-64, -32, 32, 64}) append(v, block_dc(y));
    r.decode(Mdec::k24Bit, true, false, v);
    auto b = bytes(r.drain());
    auto g = [&](int x, int y) { return static_cast<int8_t>(b[3 * (y * 16 + x)]); };
    EXPECT_NEAR(g(0, 0), -64, 1);   // Y1 top-left
    EXPECT_NEAR(g(15, 0), -32, 1);  // Y2 top-right
    EXPECT_NEAR(g(0, 15), 32, 1);   // Y3 bottom-left
    EXPECT_NEAR(g(15, 15), 64, 1);  // Y4 bottom-right
    EXPECT_NEAR(g(7, 7), -64, 1);
    EXPECT_NEAR(g(8, 7), -32, 1);
    EXPECT_NEAR(g(7, 8), 32, 1);
    EXPECT_NEAR(g(8, 8), 64, 1);
}

TEST(Mdec, Rgb24PacksAcrossWords) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, false, flat_mb(32, 0, 0));
    auto w = r.drain();
    // Pixels: R0 G0 B0 R1 | G1 B1 R2 G2 | ...
    EXPECT_EQ(w[0], 173u | (105u << 8) | (128u << 16) | (173u << 24));
    EXPECT_EQ(w[1], 105u | (128u << 8) | (173u << 16) | (105u << 24));
}

// ---- multiple macroblocks / timing ----

TEST(Mdec, MultipleMacroblocksInOneCommand) {
    Rig r;
    r.set_quant(8, 8, true);
    std::vector<uint16_t> v;
    for (int y : {0, 32, -32}) append(v, flat_mb(0, 0, y));
    r.decode(Mdec::k24Bit, false, false, v);
    auto b = bytes(r.drain());
    ASSERT_EQ(b.size(), 3u * 768);
    EXPECT_EQ(b[0], 0x80);
    EXPECT_EQ(b[768], 0xA0);
    EXPECT_EQ(b[1536], 0x60);
    EXPECT_EQ(r.mdec.macroblocks_decoded(), 3u);
}

TEST(Mdec, MultipleMonoBlocksOrderPreserved) {
    Rig r;
    r.set_quant(8, 8, true);
    std::vector<uint16_t> v;
    for (int d : {32, -32, 0, 16}) append(v, block_dc(d));
    r.decode(Mdec::k8Bit, false, false, v);
    auto w = r.drain();
    ASSERT_EQ(w.size(), 64u);
    EXPECT_EQ(w[0], 0xA0A0A0A0u);
    EXPECT_EQ(w[16], 0x60606060u);
    EXPECT_EQ(w[32], 0x80808080u);
    EXPECT_EQ(w[48], 0x90909090u);
}

TEST(Mdec, TwoCommandsBackToBack) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, block_dc(32));
    r.decode(Mdec::k8Bit, true, false, block_dc(32));
    auto w = r.drain();
    ASSERT_EQ(w.size(), 32u);
    EXPECT_EQ(w[0], 0xA0A0A0A0u);
    EXPECT_EQ(w[16], 0x20202020u);
}

TEST(Mdec, OutputNotAvailableBeforeDelay) {
    Rig r;
    r.set_quant(8, 8, true);
    r.w32(kCtrl, 1u << 29);
    r.decode(Mdec::k24Bit, false, false, flat_mb(0, 0, 0));
    EXPECT_EQ(r.mdec.available_words(), 0u);
    EXPECT_TRUE(r.status() & (1u << 31));
    EXPECT_TRUE(r.status() & (1u << 29));
    EXPECT_FALSE(r.status() & (1u << 27));
    r.run_to(r.ctx().cycles + 6 * Mdec::kCyclesPerBlock);
    EXPECT_EQ(r.mdec.available_words(), 192u);
    EXPECT_FALSE(r.status() & (1u << 31));
    EXPECT_FALSE(r.status() & (1u << 29));
    EXPECT_TRUE(r.status() & (1u << 27));
}

TEST(Mdec, DataOutRequestNeedsEnable) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, block_dc(0));
    r.settle();
    EXPECT_FALSE(r.status() & (1u << 27));
    r.w32(kCtrl, 1u << 29);
    EXPECT_TRUE(r.status() & (1u << 27));
}

TEST(Mdec, ReadingFifoDrainsAndSetsEmpty) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k4Bit, false, false, block_dc(0));
    r.settle();
    for (int i = 0; i < 8; ++i) EXPECT_FALSE(r.status() & (1u << 31));
    for (int i = 0; i < 8; ++i) r.r32(kData);
    EXPECT_TRUE(r.status() & (1u << 31));
}

TEST(Mdec, ByteReadsPopAfterLastByte) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k8Bit, false, false, block_dc(32));
    r.settle();
    r.decode(Mdec::k8Bit, true, false, block_dc(32));  // second block
    r.settle();
    uint32_t before = r.mdec.queued_words();
    r.bus.read(kData, 1);
    r.bus.read(kData + 1, 1);
    r.bus.read(kData + 2, 1);
    EXPECT_EQ(r.mdec.queued_words(), before);
    r.bus.read(kData + 3, 1);
    EXPECT_EQ(r.mdec.queued_words(), before - 1);
}

TEST(Mdec, LargeFrameGrowsQueue) {
    Rig r;
    r.set_quant(8, 8, true);
    // 400 colour 24-bit macroblocks = 76800 words > initial capacity (65536).
    std::vector<uint16_t> v;
    for (int i = 0; i < 400; ++i) append(v, flat_mb(0, 0, 32));
    r.decode(Mdec::k24Bit, false, false, v);
    auto b = r.drain();
    ASSERT_EQ(b.size(), 400u * 192);
    EXPECT_EQ(b[400 * 192 - 1], 0xA0A0A0A0u);
}

// ---- reset ----

TEST(Mdec, ResetMidCommandDropsParams) {
    Rig r;
    r.set_quant(8, 8, true);
    r.send(Rig::decode_cmd(2, false, false, 10));
    r.send(dc(10));
    r.w32(kCtrl, 0x80000000u);
    EXPECT_FALSE(r.status() & (1u << 29));
    // A fresh command is accepted as a command, not as a parameter.
    r.decode(Mdec::k8Bit, false, false, block_dc(32));
    for (uint32_t x : r.drain()) EXPECT_EQ(x, 0xA0A0A0A0u);
}

TEST(Mdec, ResetMidBlockRestartsDecoder) {
    Rig r;
    r.set_quant(8, 8, true);
    r.send(Rig::decode_cmd(2, false, false, 10));
    r.send(dc(5) | (uint32_t{dc(5)} << 16));  // part of a block, no end marker yet
    r.w32(kCtrl, 0x80000000u);
    r.decode(Mdec::k8Bit, false, false, block_dc(32));
    auto w = r.drain();
    ASSERT_EQ(w.size(), 16u);
    EXPECT_EQ(w[0], 0xA0A0A0A0u);
}

TEST(Mdec, ResetDiscardsPendingOutput) {
    Rig r;
    r.set_quant(8, 8, true);
    r.decode(Mdec::k24Bit, false, false, flat_mb(0, 0, 0));
    r.w32(kCtrl, 0x80000000u);
    r.settle();
    EXPECT_EQ(r.mdec.available_words(), 0u);
    EXPECT_EQ(r.status(), 0x80040000u);
}

TEST(Mdec, ResetMidQuantUploadAbortsIt) {
    Rig r;
    r.set_quant(7, 7, true);
    r.send(2u << 29);
    for (int i = 0; i < 4; ++i) r.send(0x09090909u);
    r.w32(kCtrl, 0x80000000u);
    r.send(2u << 29);
    for (int i = 0; i < 16; ++i) r.send(0x05050505u);
    EXPECT_EQ(r.mdec.luma_table()[0], 5);
    EXPECT_FALSE(r.status() & (1u << 29));
}

// ---- DMA end to end ----

namespace {
struct DmaRig : Rig {
    static constexpr uint32_t kIn = 0x1F801080u, kOut = 0x1F801090u, kDpcr = 0x1F8010F0u;
    DmaRig() {
        w32(kDpcr, r32(kDpcr) | 0x8u | 0x80u);
        w32(kCtrl, 3u << 29);
    }
    void poke(uint32_t addr, uint32_t v) { std::memcpy(bus.ram() + addr, &v, 4); }
    uint32_t peek(uint32_t addr) {
        uint32_t v;
        std::memcpy(&v, bus.ram() + addr, 4);
        return v;
    }
    // Place a decode command + data into RAM at `addr`; returns word count.
    uint32_t load_decode(uint32_t addr, uint32_t depth, bool sgn, bool b15, std::vector<uint16_t> hw) {
        if (hw.size() & 1) hw.push_back(kEob);
        poke(addr, decode_cmd(depth, sgn, b15, static_cast<uint32_t>(hw.size() / 2)));
        for (size_t i = 0; i < hw.size(); i += 2)
            poke(addr + 4 + static_cast<uint32_t>(2 * i), hw[i] | (static_cast<uint32_t>(hw[i + 1]) << 16));
        return static_cast<uint32_t>(hw.size() / 2 + 1);
    }
    void start_in(uint32_t addr, uint32_t words) {
        w32(kIn, addr);
        w32(kIn + 4, words | (1u << 16));
        w32(kIn + 8, 0x01000201u);
    }
    void start_out(uint32_t addr, uint32_t words) {
        w32(kOut, addr);
        w32(kOut + 4, words | (1u << 16));
        w32(kOut + 8, 0x01000200u);
    }
    void upload_tables_dma() {
        set_quant(8, 8, true);
        set_scale();
    }
};
}  // namespace

TEST(MdecDma, InThenOutProduces15BitBlock) {
    DmaRig r;
    r.upload_tables_dma();
    uint32_t n = r.load_decode(0x1000, Mdec::k15Bit, false, false, flat_mb(0, 0, 32));
    r.start_in(0x1000, n);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_FALSE(r.dma.busy(dw3::DmaChannel::MdecIn));
    r.start_out(0x2000, 128);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_FALSE(r.dma.busy(dw3::DmaChannel::MdecOut));
    for (int i = 0; i < 128; ++i) EXPECT_EQ(r.peek(0x2000 + 4 * i), 0x52945294u) << i;
}

TEST(MdecDma, OutStartedBeforeInWaitsForData) {
    DmaRig r;
    r.upload_tables_dma();
    r.start_out(0x2000, 128);
    EXPECT_TRUE(r.dma.busy(dw3::DmaChannel::MdecOut));
    EXPECT_FALSE(r.dma.in_flight(dw3::DmaChannel::MdecOut));
    uint32_t n = r.load_decode(0x1000, Mdec::k15Bit, false, false, flat_mb(0, 0, 32));
    r.start_in(0x1000, n);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_FALSE(r.dma.busy(dw3::DmaChannel::MdecOut));
    EXPECT_EQ(r.peek(0x2000), 0x52945294u);
    EXPECT_EQ(r.peek(0x2000 + 127 * 4), 0x52945294u);
}

TEST(MdecDma, OutWaitsForDecodeDelay) {
    DmaRig r;
    r.upload_tables_dma();
    uint32_t n = r.load_decode(0x1000, Mdec::k15Bit, false, false, flat_mb(0, 0, 32));
    r.start_in(0x1000, n);
    r.start_out(0x2000, 128);
    // Decode needs 6 blocks of delay; DMA1 must not have run yet.
    EXPECT_FALSE(r.dma.in_flight(dw3::DmaChannel::MdecOut));
    EXPECT_EQ(r.peek(0x2000), 0u);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_EQ(r.peek(0x2000), 0x52945294u);
}

TEST(MdecDma, OutDeferredWithoutOutputEnable) {
    DmaRig r;
    r.w32(kCtrl, 1u << 30);  // out disabled
    r.upload_tables_dma();
    uint32_t n = r.load_decode(0x1000, Mdec::k15Bit, false, false, flat_mb(0, 0, 32));
    r.start_in(0x1000, n);
    r.run_to(r.ctx().cycles + 100000);
    r.start_out(0x2000, 128);
    EXPECT_FALSE(r.dma.in_flight(dw3::DmaChannel::MdecOut));
    r.w32(kCtrl, 3u << 29);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_EQ(r.peek(0x2000), 0x52945294u);
}

TEST(MdecDma, InDeferredWithoutInputEnable) {
    DmaRig r;
    r.w32(kCtrl, 0);
    r.set_quant(8, 8, true);
    uint32_t n = r.load_decode(0x1000, Mdec::k8Bit, false, false, block_dc(32));
    r.start_in(0x1000, n);
    EXPECT_FALSE(r.dma.in_flight(dw3::DmaChannel::MdecIn));
    r.w32(kCtrl, 1u << 30);
    EXPECT_TRUE(r.dma.in_flight(dw3::DmaChannel::MdecIn));
}

TEST(MdecDma, SlicedOutputOfMultipleMacroblocks) {
    DmaRig r;
    r.upload_tables_dma();
    std::vector<uint16_t> v;
    for (int y : {0, 32, -32}) append(v, flat_mb(0, 0, y));
    uint32_t n = r.load_decode(0x1000, Mdec::k15Bit, false, false, v);
    r.start_in(0x1000, n);
    r.run_to(r.ctx().cycles + 1000000);
    const uint32_t expect[3] = {0x42104210u /* 16,16,16 */, 0x52945294u, 0x318C318Cu};
    for (int mb = 0; mb < 3; ++mb) {
        r.start_out(0x4000 + mb * 512, 128);
        r.run_to(r.ctx().cycles + 100000);
        EXPECT_EQ(r.peek(0x4000 + mb * 512), expect[mb]) << mb;
        EXPECT_EQ(r.peek(0x4000 + mb * 512 + 508), expect[mb]) << mb;
    }
}

TEST(MdecDma, OutRaisesDmaIrqWhenEnabled) {
    DmaRig r;
    r.w32(0x1F801074u, 0x7FF);
    r.w32(0x1F8010F4u, (1u << 23) | (1u << 17));  // master enable + channel 1 IRQ enable
    r.upload_tables_dma();
    uint32_t n = r.load_decode(0x1000, Mdec::k15Bit, false, false, flat_mb(0, 0, 32));
    r.start_in(0x1000, n);
    r.start_out(0x2000, 128);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_TRUE(r.r32(0x1F8010F4u) & (1u << 25));
    EXPECT_TRUE(r.r32(0x1F801070u) & (1u << 3));
}

TEST(MdecDma, TablesViaDma) {
    DmaRig r;
    r.poke(0x1000, 2u << 29 | 1u);
    for (int i = 0; i < 32; ++i) r.poke(0x1004 + 4 * i, 0x0A0A0A0Au);
    r.start_in(0x1000, 33);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_EQ(r.mdec.luma_table()[63], 10);
    EXPECT_EQ(r.mdec.chroma_table()[0], 10);
}

TEST(MdecDma, InWritesWholeStreamIncludingCommandInOneBurst) {
    DmaRig r;
    r.set_quant(8, 8, true);
    uint32_t n = r.load_decode(0x1000, Mdec::k8Bit, false, false, block_dc(32));
    r.start_in(0x1000, n);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_EQ(r.mdec.queued_words(), 16u);
}

TEST(MdecDma, DmaOutStallsUntilEnoughWordsDecoded) {
    DmaRig r;
    r.upload_tables_dma();
    uint32_t n = r.load_decode(0x1000, Mdec::k8Bit, false, false, block_dc(32));
    r.start_in(0x1000, n);
    r.run_to(r.ctx().cycles + 100000);
    r.start_out(0x3000, 32);  // only 16 words exist
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_TRUE(r.dma.busy(dw3::DmaChannel::MdecOut));
    EXPECT_FALSE(r.dma.in_flight(dw3::DmaChannel::MdecOut));
    // A second block arrives: the transfer now runs.
    n = r.load_decode(0x1000, Mdec::k8Bit, true, false, block_dc(32));
    r.start_in(0x1000, n);
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_FALSE(r.dma.busy(dw3::DmaChannel::MdecOut));
    EXPECT_EQ(r.peek(0x3000), 0xA0A0A0A0u);
    EXPECT_EQ(r.peek(0x3000 + 4 * 16), 0x20202020u);
}

TEST(MdecDma, ReadyOnlyWhenWholeRequestIsVisible) {
    DmaRig r;
    r.upload_tables_dma();
    std::vector<uint16_t> v;
    for (int i = 0; i < 2; ++i) append(v, flat_mb(0, 0, 32));
    uint32_t n = r.load_decode(0x1000, Mdec::k15Bit, false, false, v);
    r.start_in(0x1000, n);
    r.start_out(0x2000, 256);  // two macroblocks
    r.run_to(r.ctx().cycles + 6 * Mdec::kCyclesPerBlock);
    EXPECT_FALSE(r.dma.in_flight(dw3::DmaChannel::MdecOut));  // only the first one is visible
    EXPECT_EQ(r.mdec.available_words(), 128u);
    r.run_to(r.ctx().cycles + 6 * Mdec::kCyclesPerBlock);
    EXPECT_EQ(r.peek(0x2000 + 255 * 4), 0x52945294u);
}

TEST(Mdec, MacroblocksBecomeVisibleIndividually) {
    Rig r;
    r.set_quant(8, 8, true);
    std::vector<uint16_t> v;
    for (int i = 0; i < 3; ++i) append(v, flat_mb(0, 0, 0));
    r.decode(Mdec::k15Bit, false, false, v);
    for (uint32_t k = 1; k <= 3; ++k) {
        r.run_to(r.ctx().cycles + 6 * Mdec::kCyclesPerBlock);
        EXPECT_EQ(r.mdec.available_words(), 128u * k - 0) << k;
        // keep them queued: do not read
    }
}

TEST(MdecDma, LibpressStyleFrame) {
    // 320x240 frame as 20 columns x 15 rows of 16x16 blocks, decoded in one DecDCTin, read out per macroblock.
    DmaRig r;
    r.upload_tables_dma();
    std::vector<uint16_t> v;
    for (int i = 0; i < 300; ++i) append(v, flat_mb(0, 0, (i % 7) * 16 - 48));
    uint32_t n = r.load_decode(0x10000, Mdec::k15Bit, false, false, v);
    r.start_in(0x10000, n);
    for (int mb = 0; mb < 300; ++mb) {
        r.start_out(0x80000 + mb * 512, 128);
        r.run_to(r.ctx().cycles + 100000);
        ASSERT_FALSE(r.dma.busy(dw3::DmaChannel::MdecOut)) << mb;
        int y = ((mb % 7) * 16 - 48 + 128);
        uint32_t c = static_cast<uint32_t>(y >> 3);
        uint32_t p = c | (c << 5) | (c << 10);
        ASSERT_NEAR(static_cast<int>(r.peek(0x80000 + mb * 512) & 31), static_cast<int>(p & 31), 1) << mb;
    }
    EXPECT_EQ(r.mdec.macroblocks_decoded(), 300u);
}
