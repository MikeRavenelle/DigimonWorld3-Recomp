// SPU DSP building blocks: ADPCM decode, Gaussian interpolation, envelopes, noise, reverb.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <numeric>
#include <vector>

#include "dw3/spu_dsp.h"

using namespace dw3::spu_dsp;

namespace {

std::array<uint8_t, 16> block(uint8_t header, uint8_t flags, const std::array<uint8_t, 14>& data) {
    std::array<uint8_t, 16> b{};
    b[0] = header;
    b[1] = flags;
    for (std::size_t i = 0; i < 14; ++i) b[2 + i] = data[i];
    return b;
}

std::array<uint8_t, 14> all_nibbles(unsigned n) {
    std::array<uint8_t, 14> d{};
    d.fill(static_cast<uint8_t>(n | (n << 4)));
    return d;
}

std::array<int16_t, 28> decode(const std::array<uint8_t, 16>& b, int32_t s1 = 0, int32_t s2 = 0) {
    std::array<int16_t, 28> out{};
    decode_adpcm_block(b.data(), out.data(), s1, s2);
    return out;
}

// Straightforward reference decoder written from the psx-spx description (filter 0..4).
int16_t ref_sample(int nib, int shift, int filter, int old, int older) {
    static const int pos[5] = {0, 60, 115, 98, 122}, neg[5] = {0, 0, -52, -55, -60};
    int t = ((nib & 8) ? nib - 16 : nib) * 4096;  // sign-extended nibble placed in the top 4 bits
    if (shift > 12) shift = 9;
    t >>= shift;
    int s = t + ((old * pos[filter] + older * neg[filter] + 32) >> 6);
    return static_cast<int16_t>(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
}

}  // namespace

// ---- ADPCM ----

TEST(SpuAdpcm, ShiftZeroFilterZeroLiteral) {
    auto out = decode(block(0x00, 0, {0x21, 0x87}));
    EXPECT_EQ(out[0], 4096);    // low nibble first
    EXPECT_EQ(out[1], 8192);
    EXPECT_EQ(out[2], 28672);   // 7 << 12
    EXPECT_EQ(out[3], -32768);  // 8 = -8 << 12
}

TEST(SpuAdpcm, ZeroDataDecodesToSilence) {
    for (int filter = 0; filter < 5; ++filter) {
        auto out = decode(block(static_cast<uint8_t>(filter << 4), 0, {}));
        for (auto v : out) EXPECT_EQ(v, 0) << filter;
    }
}

TEST(SpuAdpcm, Filter1HandComputed) {
    // shift 12, filter 1 (f0=60): 1; 2+((1*60+32)>>6)=3; 7+((3*60+32)>>6)=10; 0+((10*60+32)>>6)=9.
    auto out = decode(block(0x1C, 0, {0x21, 0x07}));
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 3);
    EXPECT_EQ(out[2], 10);
    EXPECT_EQ(out[3], 9);
}

TEST(SpuAdpcm, Filter2HandComputed) {
    auto out = decode(block(0x2C, 0, {0x01, 0x00}));
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
    EXPECT_EQ(out[2], 3);
    EXPECT_EQ(out[3], 4);
}

TEST(SpuAdpcm, Filter4ClampsToInt16) {
    auto out = decode(block(0x40, 0, {0x77}));
    EXPECT_EQ(out[0], 28672);
    EXPECT_EQ(out[1], 32767);
}

TEST(SpuAdpcm, NegativeClamp) {
    auto out = decode(block(0x40, 0, {0x88}));
    EXPECT_EQ(out[0], -32768);
    EXPECT_EQ(out[1], -32768);
}

TEST(SpuAdpcm, ShiftAbove12ActsAsNine) {
    auto out = decode(block(0x0D, 0, {0x07}));
    EXPECT_EQ(out[0], 28672 >> 9);
    EXPECT_EQ(decode(block(0x0F, 0, {0x07}))[0], 28672 >> 9);
}

TEST(SpuAdpcm, Filters5To7ActAsFilter4) {
    for (uint8_t f = 5; f < 8; ++f) {
        auto a = decode(block(static_cast<uint8_t>(f << 4), 0, {0x13, 0x25, 0x7A}));
        auto b = decode(block(0x40, 0, {0x13, 0x25, 0x7A}));
        EXPECT_EQ(a, b) << int(f);
    }
}

TEST(SpuAdpcm, HistoryCarriesIntoNextBlock) {
    int32_t s1 = 0, s2 = 0;
    std::array<int16_t, 28> o1{}, o2{};
    auto b = block(0x10, 0, all_nibbles(3));
    decode_adpcm_block(b.data(), o1.data(), s1, s2);
    EXPECT_EQ(s1, o1[27]);
    EXPECT_EQ(s2, o1[26]);
    int32_t t1 = o1[27], t2 = o1[26];
    decode_adpcm_block(b.data(), o2.data(), s1, s2);
    std::array<int16_t, 28> o3{};
    decode_adpcm_block(b.data(), o3.data(), t1, t2);
    EXPECT_EQ(o2, o3);
}

TEST(SpuAdpcm, FlagsAreNotInterpretedByDecoder) {
    EXPECT_EQ(decode(block(0x00, 0x00, all_nibbles(5))), decode(block(0x00, 0x07, all_nibbles(5))));
}

TEST(SpuAdpcm, FlagBitValues) {
    EXPECT_EQ(kFlagLoopEnd, 1);
    EXPECT_EQ(kFlagLoopRepeat, 2);
    EXPECT_EQ(kFlagLoopStart, 4);
}

// Every filter x shift combination against the reference over a varied nibble stream.
class AdpcmFilterShift : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(AdpcmFilterShift, MatchesReference) {
    auto [filter, shift] = GetParam();
    std::array<uint8_t, 14> data{};
    for (std::size_t i = 0; i < 14; ++i) data[i] = static_cast<uint8_t>(0x5A + 37 * i);
    auto b = block(static_cast<uint8_t>((filter << 4) | shift), 0, data);
    auto out = decode(b, 1234, -777);
    int old = 1234, older = -777;
    for (int i = 0; i < 28; ++i) {
        int nib = (data[static_cast<std::size_t>(i / 2)] >> ((i & 1) * 4)) & 0xF;
        int16_t want = ref_sample(nib, shift, filter, old, older);
        ASSERT_EQ(out[static_cast<std::size_t>(i)], want) << "sample " << i;
        older = old;
        old = want;
    }
}

INSTANTIATE_TEST_SUITE_P(All, AdpcmFilterShift, ::testing::Combine(::testing::Range(0, 5), ::testing::Range(0, 16)));

// ---- Gaussian interpolation ----

TEST(SpuGauss, TableEndpoints) {
    EXPECT_EQ(kGauss[0x000], -1);
    EXPECT_EQ(kGauss[0x00F], -1);
    EXPECT_EQ(kGauss[0x010], 0);
    EXPECT_EQ(kGauss[0x07F], 0x019C);
    EXPECT_EQ(kGauss[0x080], 0x01A8);
    EXPECT_EQ(kGauss[0x0FF], 0x12C7);
    EXPECT_EQ(kGauss[0x100], 0x1307);
    EXPECT_EQ(kGauss[0x17F], 0x3DEF);
    EXPECT_EQ(kGauss[0x180], 0x3E4C);
    EXPECT_EQ(kGauss[0x1FF], 0x59B3);
}

class GaussPhaseSum : public ::testing::TestWithParam<int> {};

TEST_P(GaussPhaseSum, FourTapsSumNearUnity) {
    int i = GetParam();
    int sum = kGauss[static_cast<std::size_t>(i)] + kGauss[static_cast<std::size_t>(0xFF - i)] +
              kGauss[static_cast<std::size_t>(0x100 + i)] + kGauss[static_cast<std::size_t>(0x1FF - i)];
    EXPECT_GE(sum, 0x7F7F);
    EXPECT_LE(sum, 0x7F81);
}

INSTANTIATE_TEST_SUITE_P(Phases, GaussPhaseSum, ::testing::Range(0, 256, 5));

TEST(SpuGauss, ZeroPhasePicksNewestMostly) {
    int16_t w[4] = {0, 0, 0, 20000};
    EXPECT_EQ(gauss_interpolate(w, 0), (kGauss[0] * 20000) >> 15);
    int16_t w2[4] = {0, 0, 20000, 0};
    EXPECT_EQ(gauss_interpolate(w2, 0), (kGauss[0x100] * 20000) >> 15);
    int16_t w3[4] = {0, 20000, 0, 0};
    EXPECT_EQ(gauss_interpolate(w3, 0), (kGauss[0x1FF] * 20000) >> 15);
    int16_t w4[4] = {20000, 0, 0, 0};
    EXPECT_EQ(gauss_interpolate(w4, 0), (kGauss[0xFF] * 20000) >> 15);
}

TEST(SpuGauss, IndexUsesLowEightBits) {
    int16_t w[4] = {100, -200, 300, -400};
    EXPECT_EQ(gauss_interpolate(w, 0x1AB), gauss_interpolate(w, 0xAB));
}

class GaussDc : public ::testing::TestWithParam<int> {};

TEST_P(GaussDc, ConstantSignalStaysConstant) {
    int16_t w[4] = {16000, 16000, 16000, 16000};
    int out = gauss_interpolate(w, static_cast<unsigned>(GetParam()));
    EXPECT_GE(out, 15900);
    EXPECT_LE(out, 16000);
}

INSTANTIATE_TEST_SUITE_P(Phases, GaussDc, ::testing::Values(0, 1, 17, 64, 100, 127, 128, 200, 255));

TEST(SpuGauss, NegativeConstantStaysNegative) {
    int16_t w[4] = {-30000, -30000, -30000, -30000};
    int out = gauss_interpolate(w, 77);
    EXPECT_LE(out, -29800);
    EXPECT_GE(out, -30001);
}

// ---- Envelope ----

namespace {
EnvRate rate(uint8_t shift, uint8_t step, bool dec = false, bool exp = false) {
    EnvRate r;
    r.shift = shift;
    r.step = step;
    r.decreasing = dec;
    r.exponential = exp;
    return r;
}
}  // namespace

TEST(SpuEnvelope, LinearFastestIncrease) {
    int32_t level = 0, counter = 0;
    envelope_tick(level, counter, rate(0, 0));
    EXPECT_EQ(level, 7 << 11);
    envelope_tick(level, counter, rate(0, 0));
    EXPECT_EQ(level, 14 << 11);
    envelope_tick(level, counter, rate(0, 0));
    EXPECT_EQ(level, 0x7FFF);  // 21 << 11 would exceed the range: saturates
    envelope_tick(level, counter, rate(0, 0));
    EXPECT_EQ(level, 0x7FFF);
}

TEST(SpuEnvelope, ShiftTwelveStepsEverySecondTick) {
    int32_t level = 0, counter = 0;
    std::vector<int32_t> seen;
    for (int i = 0; i < 4; ++i) {
        envelope_tick(level, counter, rate(12, 0));
        seen.push_back(level);
    }
    EXPECT_EQ(seen, (std::vector<int32_t>{0, 7, 7, 14}));
}

TEST(SpuEnvelope, ShiftElevenStepFour) {
    int32_t level = 100, counter = 0;
    envelope_tick(level, counter, rate(11, 3));
    EXPECT_EQ(level, 104);
}

TEST(SpuEnvelope, LinearDecreaseClampsAtZero) {
    int32_t level = 32767, counter = 0;
    envelope_tick(level, counter, rate(0, 0, true));
    EXPECT_EQ(level, 32767 - 16384);
    envelope_tick(level, counter, rate(0, 0, true));
    EXPECT_EQ(level, 0);
    envelope_tick(level, counter, rate(0, 0, true));
    EXPECT_EQ(level, 0);
}

TEST(SpuEnvelope, DecreaseStepsAreMinusEightToMinusFive) {
    for (int step = 0; step < 4; ++step) {
        int32_t level = 1000, counter = 0;
        envelope_tick(level, counter, rate(11, static_cast<uint8_t>(step), true));
        EXPECT_EQ(level, 1000 - (8 - step)) << step;
    }
}

TEST(SpuEnvelope, ExponentialDecreaseScalesWithLevel) {
    int32_t level = 0x4000, counter = 0;
    envelope_tick(level, counter, rate(0, 0, true, true));
    EXPECT_EQ(level, 0x2000);
}

TEST(SpuEnvelope, ExponentialDecreaseFloorsAtZero) {
    int32_t level = 5, counter = 0;
    for (int i = 0; i < 40; ++i) envelope_tick(level, counter, rate(11, 0, true, true));
    EXPECT_EQ(level, 0);
}

TEST(SpuEnvelope, ExponentialIncreaseSlowsAboveSixThousandHex) {
    int32_t level = 0x7000, counter = 0;
    envelope_tick(level, counter, rate(0, 0, false, true));
    EXPECT_EQ(level, 0x7000 + (7 << 11) / 4);
    int32_t low = 0x1000;
    counter = 0;
    envelope_tick(low, counter, rate(0, 0, false, true));
    EXPECT_EQ(low, 0x1000 + (7 << 11));
}

TEST(SpuEnvelope, ExponentialIncreaseHighShiftQuartersTheCounterRate) {
    // shift 12 above 0x6000: counter increment 0x4000 -> 0x1000, so a step every 8 ticks.
    int32_t level = 0x7000, counter = 0;
    int first = 0;
    for (int i = 1; i <= 16 && !first; ++i) {
        int32_t before = level;
        envelope_tick(level, counter, rate(12, 0, false, true));
        if (level != before) first = i;
    }
    EXPECT_EQ(first, 8);
}

TEST(SpuEnvelope, NeverStepsWhenAllRateBitsSet) {
    EnvRate r = rate(31, 3);
    r.never_steps = true;
    int32_t level = 1234, counter = 0;
    for (int i = 0; i < 100000; ++i) envelope_tick(level, counter, r);
    EXPECT_EQ(level, 1234);
}

TEST(SpuEnvelope, SlowestRateStillStepsEventually) {
    int32_t level = 0, counter = 0;
    int ticks = 0;
    while (level == 0 && ticks < 100000) {
        envelope_tick(level, counter, rate(31, 3));
        ++ticks;
    }
    EXPECT_EQ(ticks, 32768);
    EXPECT_EQ(level, 4);
}

TEST(SpuEnvelope, NegativePhaseIncreaseGoesNegative) {
    EnvRate r = rate(0, 0);
    r.phase_negative = true;
    int32_t level = 0, counter = 0;
    envelope_tick(level, counter, r);
    EXPECT_EQ(level, -16384);
    envelope_tick(level, counter, r);
    EXPECT_EQ(level, -32768);
    envelope_tick(level, counter, r);
    EXPECT_EQ(level, -32768);
}

TEST(SpuEnvelope, NegativePhaseDecreaseRisesToZero) {
    EnvRate r = rate(0, 0, true);
    r.phase_negative = true;
    int32_t level = -20000, counter = 0;
    envelope_tick(level, counter, r);
    EXPECT_EQ(level, -20000 + (7 << 11));  // decreasing XOR negative phase: the step points upwards
    envelope_tick(level, counter, r);
    EXPECT_EQ(level, 0);  // never overshoots zero
}

// First step occurs after ceil(0x8000 / inc) ticks; its size is (7 - step) << max(0, 11 - shift).
class EnvelopeRate : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(EnvelopeRate, FirstStepTimingAndSize) {
    auto [shift, step] = GetParam();
    int32_t level = 0, counter = 0;
    int ticks = 0;
    while (level == 0 && ticks < 40000) {
        envelope_tick(level, counter, rate(static_cast<uint8_t>(shift), static_cast<uint8_t>(step)));
        ++ticks;
    }
    int inc = std::max(1, 0x8000 >> std::max(0, shift - 11));
    EXPECT_EQ(ticks, (0x8000 + inc - 1) / inc);
    EXPECT_EQ(level, std::min(0x7FFF, (7 - step) << std::max(0, 11 - shift)));
}

INSTANTIATE_TEST_SUITE_P(AllRates, EnvelopeRate, ::testing::Combine(::testing::Range(0, 32), ::testing::Range(0, 4)));

// ---- Noise ----

TEST(SpuNoise, FastestShiftSequenceLiteral) {
    Noise n;
    for (int i = 0; i < 10; ++i) n.tick(15, 0);
    EXPECT_EQ(n.level, 0x03FF);  // parity bit is 1 while bits 15/12/11/10 are clear
    n.tick(15, 0);
    EXPECT_EQ(n.level, 0x07FF);
    n.tick(15, 0);
    EXPECT_EQ(n.level, 0x0FFE);  // bit 10 set -> parity 0
}

TEST(SpuNoise, SlowShiftHoldsLevel) {
    Noise n;
    n.tick(0, 0);  // reload 0x20000: first tick clocks, then waits a long time
    uint16_t first = n.level;
    for (int i = 0; i < 1000; ++i) n.tick(0, 0);
    EXPECT_EQ(n.level, first);
}

TEST(SpuNoise, TimerDoubleReload) {
    Noise n;
    n.timer = -100;
    n.tick(15, 3);  // reload value is 4; two reloads still leave the timer negative
    EXPECT_LT(n.timer, 0);
    EXPECT_EQ(n.level, 1);
}

class NoiseRate : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(NoiseRate, MatchesReferenceSimulation) {
    auto [shift, step] = GetParam();
    Noise n;
    int32_t timer = 0;
    uint16_t level = 0;
    for (int i = 0; i < 3000; ++i) {
        n.tick(static_cast<unsigned>(shift), static_cast<unsigned>(step));
        timer -= step + 4;
        int parity = ((level >> 15) ^ (level >> 12) ^ (level >> 11) ^ (level >> 10) ^ 1) & 1;
        for (int k = 0; k < 2 && timer < 0; ++k) {
            if (k == 0) level = static_cast<uint16_t>(level * 2 + parity);
            timer += 0x20000 >> shift;
        }
        ASSERT_EQ(n.level, level) << i;
    }
}

INSTANTIATE_TEST_SUITE_P(All, NoiseRate, ::testing::Combine(::testing::Range(0, 16, 3), ::testing::Range(0, 4)));

// ---- Reverb ----

namespace {

struct ReverbRig {
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x80000, 0);
    std::array<uint16_t, 32> regs{};
    Reverb rv;
    ReverbRig() { rv.set_start(0x80000 - 0x26C0); }
    void room_preset() {
        const uint16_t room[32] = {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6,
                                   0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000,
                                   0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000};
        std::copy(room, room + 32, regs.begin());
    }
    // Runs n ticks with the given input; returns the outputs.
    std::vector<int16_t> run(int n, int16_t in_l, int16_t in_r, bool write = true) {
        std::vector<int16_t> out;
        for (int i = 0; i < n; ++i) {
            int16_t l, r;
            rv.tick(ram.data(), regs.data(), write, in_l, in_r, l, r);
            out.push_back(l);
            out.push_back(r);
        }
        return out;
    }
};

}  // namespace

TEST(SpuReverb, FirKernelIsSymmetricHalfband) {
    for (std::size_t i = 0; i < 39; ++i) EXPECT_EQ(Reverb::kFir[i], Reverb::kFir[38 - i]) << i;
    int sum = 0, even = 0;
    for (std::size_t i = 0; i < 39; ++i) {
        sum += Reverb::kFir[i];
        if (i % 2 == 0) even += Reverb::kFir[i];
    }
    EXPECT_EQ(Reverb::kFir[19], 0x4000);
    EXPECT_EQ(even, 16382);
    EXPECT_EQ(sum, 32766);
}

TEST(SpuReverb, SilenceInSilenceOut) {
    ReverbRig r;
    r.room_preset();
    for (auto v : r.run(2000, 0, 0)) EXPECT_EQ(v, 0);
}

TEST(SpuReverb, ReverbOffPresetIsSilentAndWritesNothingAudible) {
    ReverbRig r;  // all registers zero
    for (auto v : r.run(500, 10000, -10000)) EXPECT_EQ(v, 0);
}

TEST(SpuReverb, ImpulseProducesTailInRoomPreset) {
    ReverbRig r;
    r.room_preset();
    std::vector<int16_t> out;
    for (int i = 0; i < 6000; ++i) {
        int16_t l, rr;
        r.rv.tick(r.ram.data(), r.regs.data(), true, i == 0 ? 20000 : 0, i == 0 ? 20000 : 0, l, rr);
        out.push_back(l);
    }
    int nonzero = 0, peak = 0;
    for (auto v : out) {
        nonzero += v != 0;
        peak = std::max(peak, std::abs(int(v)));
    }
    EXPECT_GT(nonzero, 200);
    EXPECT_GT(peak, 50);
}

TEST(SpuReverb, WriteDisabledLeavesBufferUntouched) {
    ReverbRig r;
    r.room_preset();
    r.run(3000, 12000, 12000, false);
    for (std::size_t i = 0x80000 - 0x26C0; i < 0x80000; ++i) ASSERT_EQ(r.ram[i], 0) << i;
}

TEST(SpuReverb, WriteEnabledFillsBuffer) {
    ReverbRig r;
    r.room_preset();
    r.run(3000, 12000, 12000, true);
    int nonzero = 0;
    for (std::size_t i = 0x80000 - 0x26C0; i < 0x80000; ++i) nonzero += r.ram[i] != 0;
    EXPECT_GT(nonzero, 100);
}

TEST(SpuReverb, NeverWritesOutsideWorkArea) {
    ReverbRig r;
    r.room_preset();
    r.run(5000, 30000, -30000, true);
    for (std::size_t i = 0; i < 0x80000 - 0x26C0; ++i) ASSERT_EQ(r.ram[i], 0) << i;
}

TEST(SpuReverb, BufferAddressAdvancesTwoBytesPerReverbSample) {
    ReverbRig r;
    r.room_preset();
    uint32_t a0 = r.rv.current();
    r.run(2, 0, 0);  // one 22.05 kHz sample = two 44.1 kHz ticks
    EXPECT_EQ(r.rv.current(), a0 + 2);
    r.run(200, 0, 0);
    EXPECT_EQ(r.rv.current(), a0 + 2 + 200);
}

TEST(SpuReverb, BufferAddressWrapsToStart) {
    ReverbRig r;
    r.rv.set_start(0x7FFF0);
    EXPECT_EQ(r.rv.current(), 0x7FFF0u);
    r.run(2 * 8, 0, 0);  // 8 steps of 2 bytes reaches 0x80000 -> wraps to the start
    EXPECT_EQ(r.rv.current(), 0x7FFF0u);
}

TEST(SpuReverb, SetStartMovesCurrentAddress) {
    Reverb rv;
    rv.set_start(0x1000);
    EXPECT_EQ(rv.start(), 0x1000u);
    EXPECT_EQ(rv.current(), 0x1000u);
}

// A pass-through configuration: same-side stage with vIIR ~ 1 stores the input, comb 1 reads it back.
TEST(SpuReverb, DcPassesThroughResamplersWithNearUnityGain) {
    ReverbRig r;
    r.regs[Reverb::vIIR] = 0x7FFF;
    r.regs[Reverb::vCOMB1] = 0x7FFF;
    r.regs[Reverb::mLSAME] = 0x10;
    r.regs[Reverb::mRSAME] = 0x10 + 0x100;
    r.regs[Reverb::mLCOMB1] = 0x10;
    r.regs[Reverb::mRCOMB1] = 0x10 + 0x100;
    r.regs[Reverb::vLIN] = 0x7FFF;
    r.regs[Reverb::vRIN] = 0x7FFF;
    r.regs[Reverb::mLAPF1] = 0x20;
    r.regs[Reverb::mRAPF1] = 0x20 + 0x100;
    r.regs[Reverb::mLAPF2] = 0x30;
    r.regs[Reverb::mRAPF2] = 0x30 + 0x100;
    auto out = r.run(400, 8000, -8000);
    int16_t l = out[out.size() - 2], rr = out[out.size() - 1];
    EXPECT_NEAR(l, 8000, 120);
    EXPECT_NEAR(rr, -8000, 120);
}

TEST(SpuReverb, RegressionChecksumRoomPresetNoise) {
    ReverbRig r;
    r.room_preset();
    uint32_t seed = 12345;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 4096; ++i) {
        seed = seed * 1664525u + 1013904223u;
        int16_t in = static_cast<int16_t>((seed >> 16) % 4000) - 2000;
        int16_t l, rr;
        r.rv.tick(r.ram.data(), r.regs.data(), true, in, static_cast<int16_t>(-in), l, rr);
        h = (h ^ static_cast<uint16_t>(l)) * 16777619u;
        h = (h ^ static_cast<uint16_t>(rr)) * 16777619u;
    }
    EXPECT_EQ(h, 3225441099u) << "update the checksum after verifying the change is intentional";
}
