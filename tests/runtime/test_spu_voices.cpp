// SPU voice engine, mixer, capture, CD input and timed generation (driven through the register interface).

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "dw3/bus.h"
#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"
#include "dw3/spu.h"

using namespace dw3;

namespace {

struct Rig {
    Bus bus;
    Scheduler sched{bus.ctx()};
    IrqController irq{bus};
    Dma dma{bus, sched, irq};
    Spu spu{bus, irq, dma};

    void w16(uint32_t off, uint16_t v) { bus.write(Spu::kBase + off, v, 2); }
    uint16_t r16(uint32_t off) { return static_cast<uint16_t>(bus.read(Spu::kBase + off, 2)); }
    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) spu.generate_sample();
    }

    // Writes a 16-byte ADPCM block into SPU RAM at `addr`.
    void put_block(uint32_t addr, uint8_t header, uint8_t flags, uint8_t fill_byte) {
        auto ram = spu.ram_mut();
        ram[addr] = header;
        ram[addr + 1] = flags;
        for (int i = 2; i < 16; ++i) ram[addr + static_cast<uint32_t>(i)] = fill_byte;
    }
    // Voice register helpers.
    void voice(int v, uint32_t reg, uint16_t val) { w16(static_cast<uint32_t>(v) * 16 + reg, val); }
    void setup(int v, uint32_t addr, uint16_t pitch = 0x1000, uint16_t adsr1 = 0x00FF, uint16_t adsr2 = 0x0000,
               uint16_t vol = 0x3FFF) {
        voice(v, 0x0, vol);
        voice(v, 0x2, vol);
        voice(v, 0x4, pitch);
        voice(v, 0x6, static_cast<uint16_t>(addr / 8));
        voice(v, 0x8, adsr1);
        voice(v, 0xA, adsr2);
    }
    void enable_output() {
        w16(Spu::kSpuCnt, 0xC000);
        w16(Spu::kMainVolL, 0x3FFF);
        w16(Spu::kMainVolR, 0x3FFF);
    }
    void key_on(int v) {
        if (v < 16) w16(Spu::kKeyOn, static_cast<uint16_t>(1u << v));
        else w16(Spu::kKeyOn + 2, static_cast<uint16_t>(1u << (v - 16)));
    }
    void key_off(int v) {
        if (v < 16) w16(Spu::kKeyOff, static_cast<uint16_t>(1u << v));
        else w16(Spu::kKeyOff + 2, static_cast<uint16_t>(1u << (v - 16)));
    }
    std::vector<int16_t> drain() {
        std::vector<int16_t> out(static_cast<std::size_t>(spu.audio().size()) * 2);
        spu.drain_samples(out);
        return out;
    }
};

// Instant attack (shift 0, step 0), instant sustain at max: ADSR1 = 0x00FF (attack fastest, decay shift 15, sustain 15).
constexpr uint16_t kFastAdsr1 = 0x00FF;

// A looping constant-level voice: block 0x1000 with nibble 7 repeated, loop end+repeat flags.
void constant_voice(Rig& r, int v, uint32_t addr = 0x1000, uint8_t fill = 0x77, uint16_t pitch = 0x1000) {
    r.put_block(addr, 0x00, 0x03, fill);
    r.setup(v, addr, pitch, kFastAdsr1);
}

}  // namespace

// ---- Key on / off ----

TEST(SpuVoice, KeyOnStartsAttackAtZeroLevel) {
    Rig r;
    constant_voice(r, 0);
    r.key_on(0);
    EXPECT_EQ(r.spu.voice_phase(0), Spu::Phase::Attack);
    EXPECT_EQ(r.spu.voice_level(0), 0);
    EXPECT_EQ(r.spu.voices_on(), 1u);
}

TEST(SpuVoice, KeyOnLoadsStartAddress) {
    Rig r;
    constant_voice(r, 5, 0x2468);
    r.key_on(5);
    EXPECT_EQ(r.spu.voice_address(5), 0x2468u);
    EXPECT_EQ(r.spu.voice_counter(5), 0u);
}

TEST(SpuVoice, AttackReachesFullLevelAndMovesToDecay) {
    Rig r;
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(3);
    EXPECT_EQ(r.spu.voice_level(0), 0x7FFF);
    EXPECT_NE(r.spu.voice_phase(0), Spu::Phase::Attack);
}

TEST(SpuVoice, DecaySettlesAtSustainLevel) {
    Rig r;
    constant_voice(r, 0);
    r.voice(0, 0x8, 0x0084);  // attack fastest, decay shift 8, sustain level 4 -> (4+1)*0x800
    r.key_on(0);
    int guard = 0;
    while (r.spu.voice_phase(0) != Spu::Phase::Sustain && guard++ < 5000) r.tick();
    EXPECT_EQ(r.spu.voice_phase(0), Spu::Phase::Sustain);
    EXPECT_LE(r.spu.voice_level(0), 5 * 0x800);
    EXPECT_GT(r.spu.voice_level(0), 5 * 0x800 - 0x800);
}

TEST(SpuVoice, SustainHoldsWithAllOnesRate) {
    Rig r;
    constant_voice(r, 0);
    r.voice(0, 0xA, 0xFFC0 & ~0x4000);  // sustain increase, shift 31 step 3: never steps
    r.key_on(0);
    r.tick(100);
    int32_t held = r.spu.voice_level(0);
    r.tick(3000);
    EXPECT_EQ(r.spu.voice_level(0), held);
}

TEST(SpuVoice, KeyOffEntersReleaseAndDecaysToOff) {
    Rig r;
    constant_voice(r, 0);
    r.voice(0, 0xA, 0x0000);  // release linear shift 0: -8<<11 per tick
    r.key_on(0);
    r.tick(5);
    r.key_off(0);
    EXPECT_EQ(r.spu.voice_phase(0), Spu::Phase::Release);
    EXPECT_EQ(r.spu.voices_on(), 0u);
    r.tick(5);
    EXPECT_EQ(r.spu.voice_level(0), 0);
    EXPECT_EQ(r.spu.voice_phase(0), Spu::Phase::Off);
}

TEST(SpuVoice, ReleaseIsGradualAtSlowRate) {
    Rig r;
    constant_voice(r, 0);
    r.voice(0, 0xA, 0x0010);  // release shift 16
    r.key_on(0);
    r.tick(10);
    r.key_off(0);
    int32_t a = r.spu.voice_level(0);
    r.tick(200);
    EXPECT_LT(r.spu.voice_level(0), a);
    EXPECT_GT(r.spu.voice_level(0), 0);
}

TEST(SpuVoice, KeyOffOnIdleVoiceKeepsItOff) {
    Rig r;
    r.key_off(3);
    EXPECT_EQ(r.spu.voice_phase(3), Spu::Phase::Off);
}

TEST(SpuVoice, KeyOnRestartsPlayingVoice) {
    Rig r;
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(40);
    EXPECT_GT(r.spu.voice_counter(0), 0u);
    r.key_on(0);
    EXPECT_EQ(r.spu.voice_counter(0), 0u);
    EXPECT_EQ(r.spu.voice_level(0), 0);
    EXPECT_EQ(r.spu.voice_phase(0), Spu::Phase::Attack);
}

TEST(SpuVoice, HighHalfKeyOnAddressesVoices16To23) {
    Rig r;
    constant_voice(r, 20);
    r.key_on(20);
    EXPECT_EQ(r.spu.voices_on(), 1u << 20);
    EXPECT_EQ(r.spu.voice_phase(20), Spu::Phase::Attack);
    r.key_off(20);
    EXPECT_EQ(r.spu.voice_phase(20), Spu::Phase::Release);
}

TEST(SpuVoice, KeyOnMaskStartsSeveralVoices) {
    Rig r;
    for (int v : {1, 4, 9}) constant_voice(r, v, 0x1000 + static_cast<uint32_t>(v) * 16);
    r.w16(Spu::kKeyOn, (1u << 1) | (1u << 4) | (1u << 9));
    for (int v : {1, 4, 9}) EXPECT_EQ(r.spu.voice_phase(v), Spu::Phase::Attack) << v;
    EXPECT_EQ(r.spu.voice_phase(2), Spu::Phase::Off);
}

TEST(SpuVoice, KeyOnUpperByteIgnoresBitsAbove23) {
    Rig r;
    r.w16(Spu::kKeyOn + 2, 0xFF00);
    EXPECT_EQ(r.spu.voices_on(), 0u);
}

// ---- Looping and ENDX ----

TEST(SpuVoice, EndxSetsAtLoopEndBlockAfterItPlays) {
    Rig r;
    r.put_block(0x1000, 0x00, 0x00, 0x11);
    r.put_block(0x1010, 0x00, 0x01, 0x11);  // loop end, no repeat
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.key_on(0);
    r.tick(55);
    EXPECT_EQ(r.r16(Spu::kEndx) & 1, 0);
    r.tick(1);
    EXPECT_EQ(r.r16(Spu::kEndx) & 1, 1);
}

TEST(SpuVoice, LoopEndWithoutRepeatMutesVoice) {
    Rig r;
    r.put_block(0x1000, 0x00, 0x01, 0x77);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.key_on(0);
    r.tick(28);
    EXPECT_EQ(r.spu.voice_level(0), 0);
    EXPECT_EQ(r.spu.voices_on(), 0u);
    EXPECT_NE(r.spu.voice_phase(0), Spu::Phase::Attack);
}

TEST(SpuVoice, LoopEndWithRepeatKeepsPlaying) {
    Rig r;
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(28 * 5);
    EXPECT_GT(r.spu.voice_level(0), 0);
    EXPECT_EQ(r.spu.voices_on(), 1u);
    EXPECT_EQ(r.spu.endx() & 1, 1u);
}

TEST(SpuVoice, LoopJumpsToLoopStartBlock) {
    Rig r;
    r.put_block(0x1000, 0x00, 0x00, 0x11);
    r.put_block(0x1010, 0x00, 0x04, 0x22);  // loop start
    r.put_block(0x1020, 0x00, 0x03, 0x33);  // loop end + repeat
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.key_on(0);
    r.tick(28);
    EXPECT_EQ(r.spu.voice_address(0), 0x1010u);
    r.tick(28);
    EXPECT_EQ(r.spu.voice_address(0), 0x1020u);
    r.tick(28);
    EXPECT_EQ(r.spu.voice_address(0), 0x1010u);  // back to the loop start
    EXPECT_EQ(r.spu.endx() & 1, 1u);
}

TEST(SpuVoice, KeyOnClearsEndxBit) {
    Rig r;
    constant_voice(r, 2);
    r.key_on(2);
    r.tick(28);
    EXPECT_EQ(r.spu.endx() & 4, 4u);
    r.key_on(2);
    EXPECT_EQ(r.spu.endx() & 4, 0u);
}

TEST(SpuVoice, EndxHighHalfReadback) {
    Rig r;
    constant_voice(r, 17);
    r.key_on(17);
    r.tick(28);
    EXPECT_EQ(r.r16(Spu::kEndx + 2), 1u << 1);
    EXPECT_EQ(r.r16(Spu::kEndx), 0u);
}

TEST(SpuVoice, WithoutLoopFlagsPlaybackContinuesToNextBlock) {
    Rig r;
    r.put_block(0x1000, 0x00, 0x00, 0x11);
    r.put_block(0x1010, 0x00, 0x00, 0x11);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.key_on(0);
    r.tick(28);
    EXPECT_EQ(r.spu.voice_address(0), 0x1010u);
    EXPECT_EQ(r.spu.endx(), 0u);
}

TEST(SpuVoice, LoopStartWritesRepeatRegisterEquivalent) {
    Rig r;
    r.put_block(0x1000, 0x00, 0x04, 0x11);  // loop start on the very first block
    r.put_block(0x1010, 0x00, 0x03, 0x11);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.key_on(0);
    r.tick(56);
    EXPECT_EQ(r.spu.voice_address(0), 0x1000u);
}

TEST(SpuVoice, RepeatAddressRegisterRedirectsLoop) {
    Rig r;
    r.put_block(0x1000, 0x00, 0x01, 0x11);  // end without start: jumps to the repeat address register
    r.put_block(0x3000, 0x00, 0x03, 0x00);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.key_on(0);
    r.voice(0, 0xE, 0x3000 / 8);
    r.tick(28);
    EXPECT_EQ(r.spu.voice_address(0), 0x3000u);
}

// ---- Pitch ----

class PitchCounter : public ::testing::TestWithParam<int> {};

TEST_P(PitchCounter, AdvancesByPitchPerSample) {
    int pitch = GetParam();
    Rig r;
    constant_voice(r, 0, 0x1000, 0x77, static_cast<uint16_t>(pitch));
    r.key_on(0);
    r.tick(3);
    EXPECT_EQ(r.spu.voice_counter(0), static_cast<uint32_t>(pitch * 3));
}

INSTANTIATE_TEST_SUITE_P(Rates, PitchCounter, ::testing::Values(0, 1, 0x100, 0x555, 0x800, 0x1000, 0x1800, 0x2000, 0x3FFF));

TEST(SpuVoice, PitchAbove4000IsClamped) {
    Rig r;
    constant_voice(r, 0, 0x1000, 0x77, 0x7000);
    r.key_on(0);
    r.tick(1);
    EXPECT_EQ(r.spu.voice_counter(0), 0x4000u);
}

TEST(SpuVoice, PitchZeroStopsPlayback) {
    Rig r;
    constant_voice(r, 0, 0x1000, 0x77, 0);
    r.key_on(0);
    r.tick(500);
    EXPECT_EQ(r.spu.voice_counter(0), 0u);
    EXPECT_EQ(r.spu.voice_address(0), 0x1000u);
}

TEST(SpuVoice, BlockAdvancesEvery28SamplesAtUnityPitch) {
    Rig r;
    r.put_block(0x1000, 0, 0, 0x11);
    r.put_block(0x1010, 0, 0, 0x11);
    r.put_block(0x1020, 0, 0, 0x11);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.key_on(0);
    r.tick(27);
    EXPECT_EQ(r.spu.voice_address(0), 0x1000u);
    r.tick(1);
    EXPECT_EQ(r.spu.voice_address(0), 0x1010u);
    r.tick(28);
    EXPECT_EQ(r.spu.voice_address(0), 0x1020u);
}

TEST(SpuVoice, DoublePitchConsumesBlocksTwiceAsFast) {
    Rig r;
    for (uint32_t i = 0; i < 6; ++i) r.put_block(0x1000 + i * 16, 0, 0, 0x11);
    r.setup(0, 0x1000, 0x2000, kFastAdsr1);
    r.key_on(0);
    r.tick(28);
    EXPECT_EQ(r.spu.voice_address(0), 0x1000u + 2 * 16);
}

TEST(SpuVoice, PitchModulationWithSilentSourceKeepsRate) {
    Rig r;
    constant_voice(r, 1, 0x1100, 0x77, 0x1000);
    r.w16(Spu::kPitchMod, 1u << 1);
    r.key_on(1);
    r.tick(4);
    EXPECT_EQ(r.spu.voice_counter(1), 0x4000u);  // voice 0 outputs 0 -> factor 0x8000 -> unity
}

TEST(SpuVoice, PitchModulationByNegativeSourceSlowsVoice) {
    Rig r;
    constant_voice(r, 0, 0x1000, 0x88, 0x0100);  // -32768 constant
    constant_voice(r, 1, 0x1100, 0x77, 0x1000);
    r.w16(Spu::kPitchMod, 1u << 1);
    r.key_on(0);
    r.tick(200);  // let voice 0 ramp in from its zero history and reach full level
    ASSERT_LT(r.spu.voice_output(0), -30000);
    r.key_on(1);
    uint32_t before = r.spu.voice_counter(1);
    r.tick(10);
    EXPECT_LT(r.spu.voice_counter(1) - before, 0x1000u);  // far below 10 * 0x1000
}

TEST(SpuVoice, PitchModulationOnVoiceZeroIsIgnored) {
    Rig r;
    constant_voice(r, 0, 0x1000, 0x77, 0x1000);
    r.w16(Spu::kPitchMod, 1u);
    r.key_on(0);
    r.tick(3);
    EXPECT_EQ(r.spu.voice_counter(0), 0x3000u);
}

// ---- Output level, interpolation and volume ----

TEST(SpuMix, ConstantSampleReachesExpectedLevel) {
    Rig r;
    r.enable_output();
    constant_voice(r, 0);  // sample value 0x7000 = 28672
    r.key_on(0);
    r.tick(100);
    r.drain();
    r.tick(10);
    auto out = r.drain();
    // 28672 -> gauss ~ 28650 -> *0x7FFF env -> * 0x7FFE (vol) * main 0x7FFE: ~ 28672 * 0.9999^3
    EXPECT_NEAR(out[0], 28640, 150);
    EXPECT_NEAR(out[1], 28640, 150);
}

TEST(SpuMix, VoiceOutputIsEnvelopeScaledSample) {
    Rig r;
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(60);
    EXPECT_NEAR(r.spu.voice_output(0), 28650, 120);
}

TEST(SpuMix, NegativeVolumeInvertsPhase) {
    Rig r;
    r.enable_output();
    constant_voice(r, 0);
    r.voice(0, 0x0, 0x7FFF);  // -2 (bit14 acts as sign) is too small; use 0x4000 -> -32768
    r.voice(0, 0x0, 0x4000);
    r.key_on(0);
    r.tick(100);
    r.drain();
    r.tick(5);
    auto out = r.drain();
    EXPECT_LT(out[0], -20000);
    EXPECT_GT(out[1], 20000);
}

TEST(SpuMix, LeftAndRightVolumesAreIndependent) {
    Rig r;
    r.enable_output();
    constant_voice(r, 0);
    r.voice(0, 0x0, 0x2000);  // half
    r.voice(0, 0x2, 0x0000);  // zero
    r.key_on(0);
    r.tick(100);
    r.drain();
    r.tick(5);
    auto out = r.drain();
    EXPECT_NEAR(out[0], 28650 / 2 * 0.9999, 400);
    EXPECT_EQ(out[1], 0);
}

TEST(SpuMix, MainVolumeScalesOutput) {
    Rig r;
    r.enable_output();
    r.w16(Spu::kMainVolL, 0x2000);
    r.w16(Spu::kMainVolR, 0x1000);
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(100);
    r.drain();
    r.tick(5);
    auto out = r.drain();
    EXPECT_NEAR(out[0], 28650 / 2, 300);
    EXPECT_NEAR(out[1], 28650 / 4, 200);
}

TEST(SpuMix, MutedSpuOutputsSilence) {
    Rig r;
    r.enable_output();
    r.w16(Spu::kSpuCnt, 0x8000);  // unmute bit (14) clear -> muted
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(100);
    r.drain();
    r.tick(5);
    for (auto v : r.drain()) EXPECT_EQ(v, 0);
}

TEST(SpuMix, DisabledSpuOutputsSilence) {
    Rig r;
    r.enable_output();
    r.w16(Spu::kSpuCnt, 0x4000);
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(100);
    r.drain();
    r.tick(5);
    for (auto v : r.drain()) EXPECT_EQ(v, 0);
}

TEST(SpuMix, ZeroMainVolumeIsSilent) {
    Rig r;
    r.enable_output();
    r.w16(Spu::kMainVolL, 0);
    r.w16(Spu::kMainVolR, 0);
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(100);
    r.drain();
    r.tick(5);
    for (auto v : r.drain()) EXPECT_EQ(v, 0);
}

TEST(SpuMix, VoicesSumAndSaturate) {
    Rig r;
    r.enable_output();
    r.w16(Spu::kMainVolL, 0x3FFF);
    for (int v = 0; v < 4; ++v) {
        constant_voice(r, v, 0x1000 + static_cast<uint32_t>(v) * 16);
        r.voice(v, 0x0, 0x3FFF);
        r.voice(v, 0x2, 0x3FFF);
    }
    r.w16(Spu::kKeyOn, 0xF);
    r.tick(100);
    r.drain();
    r.tick(5);
    auto out = r.drain();
    EXPECT_GE(out[0], 32760);  // 4 x 28650 clamps at full scale before the main volume
}

TEST(SpuMix, FrameCountMatchesTicks) {
    Rig r;
    r.tick(123);
    EXPECT_EQ(r.spu.frames_generated(), 123u);
    EXPECT_EQ(r.spu.audio().size(), 123u);
}

TEST(SpuMix, GaussianFadesBetweenAdjacentSamples) {
    Rig r;
    r.enable_output();
    // alternate +/- samples at half rate yields a smooth waveform: check output is not one of the two extremes
    r.put_block(0x1000, 0x00, 0x03, 0x79);  // 9 (-7) and 7 alternating? nibble low 9, high 7
    r.setup(0, 0x1000, 0x0800, kFastAdsr1);
    r.key_on(0);
    r.tick(50);
    bool saw_mid = false;
    for (int i = 0; i < 40; ++i) {
        r.tick(1);
        if (std::abs(int(r.spu.voice_output(0))) < 20000) saw_mid = true;
    }
    EXPECT_TRUE(saw_mid);
}

// ---- Noise ----

TEST(SpuNoise, NoiseVoiceOutputsLfsrLevel) {
    Rig r;
    constant_voice(r, 0);
    r.w16(Spu::kNoiseOn, 1);
    r.w16(Spu::kSpuCnt, 0xC000 | (15 << 10));  // fastest noise
    r.key_on(0);
    r.tick(80);
    int expect = (static_cast<int16_t>(r.spu.noise().level) * r.spu.voice_level(0)) >> 15;
    EXPECT_EQ(r.spu.voice_output(0), expect);
}

TEST(SpuNoise, NoiseIgnoresVoicePitch) {
    Rig a, b;
    for (Rig* r : {&a, &b}) {
        constant_voice(*r, 0, 0x1000, 0x77, r == &a ? 0x0400 : 0x3000);
        r->w16(Spu::kNoiseOn, 1);
        r->w16(Spu::kSpuCnt, 0xC000 | (14 << 10));
        r->key_on(0);
        r->tick(300);
    }
    EXPECT_EQ(a.spu.noise().level, b.spu.noise().level);
    EXPECT_EQ(a.spu.voice_output(0), b.spu.voice_output(0));
}

TEST(SpuNoise, NoiseRateFollowsSpuCnt) {
    Rig a, b;
    a.w16(Spu::kSpuCnt, 0xC000 | (15 << 10));
    b.w16(Spu::kSpuCnt, 0xC000 | (8 << 10));
    a.tick(200);
    b.tick(200);
    EXPECT_NE(a.spu.noise().level, b.spu.noise().level);
}

// ---- Volume registers, sweep and readback ----

TEST(SpuVolume, DirectModeDoublesTheValue) {
    Rig r;
    r.voice(2, 0x0, 0x2000);
    EXPECT_EQ(r.spu.voice_volume(2, 0), 0x4000);
    r.voice(2, 0x2, 0x3FFF);
    EXPECT_EQ(r.spu.voice_volume(2, 1), 0x7FFE);
}

TEST(SpuVolume, DirectModeBit14IsSign) {
    Rig r;
    r.voice(2, 0x0, 0x4000);
    EXPECT_EQ(r.spu.voice_volume(2, 0), -32768);
    r.voice(2, 0x0, 0x7FFF);
    EXPECT_EQ(r.spu.voice_volume(2, 0), -2);
}

TEST(SpuVolume, RegisterReadsBackWrittenValue) {
    Rig r;
    r.voice(7, 0x0, 0x1234);
    EXPECT_EQ(r.r16(7 * 16), 0x1234);
}

TEST(SpuVolume, CurrentVolumeRegistersExposeLevels) {
    Rig r;
    r.voice(4, 0x0, 0x1000);
    r.voice(4, 0x2, 0x0800);
    EXPECT_EQ(r.r16(Spu::kVoiceVolX + 4 * 4), 0x2000);
    EXPECT_EQ(r.r16(Spu::kVoiceVolX + 4 * 4 + 2), 0x1000);
}

TEST(SpuVolume, LinearSweepIncreaseFastest) {
    Rig r;
    r.voice(0, 0x0, 0x8000);  // sweep, linear, increase, shift 0, step 0
    r.tick(1);
    EXPECT_EQ(r.spu.voice_volume(0, 0), 7 << 11);
    r.tick(2);
    EXPECT_EQ(r.spu.voice_volume(0, 0), 0x7FFF);
}

TEST(SpuVolume, LinearSweepDecreaseFromDirectLevel) {
    Rig r;
    r.voice(0, 0x2, 0x3FFF);              // level 0x7FFE
    r.voice(0, 0x2, 0x8000 | 0x2000);     // sweep decrease
    r.tick(1);
    EXPECT_EQ(r.spu.voice_volume(0, 1), 0x7FFE - (8 << 11));
}

TEST(SpuVolume, ExponentialSweepDecreaseScalesByLevel) {
    Rig r;
    r.voice(0, 0x0, 0x2000);  // 0x4000
    r.voice(0, 0x0, 0x8000 | 0x4000 | 0x2000);
    r.tick(1);
    EXPECT_EQ(r.spu.voice_volume(0, 0), 0x2000);
}

TEST(SpuVolume, SweepAllRateBitsNeverMoves) {
    Rig r;
    r.voice(0, 0x0, 0x2000);
    r.voice(0, 0x0, 0x8000 | 0x2000 | 0x7F);
    r.tick(5000);
    EXPECT_EQ(r.spu.voice_volume(0, 0), 0x4000);
}

TEST(SpuVolume, NegativePhaseSweepGoesNegative) {
    Rig r;
    r.voice(0, 0x0, 0x8000 | 0x1000);  // increase with negative phase
    r.tick(1);
    EXPECT_EQ(r.spu.voice_volume(0, 0), -(8 << 11));
}

TEST(SpuVolume, MainVolumeSweeps) {
    Rig r;
    r.w16(Spu::kMainVolL, 0x8000);
    r.tick(1);
    EXPECT_EQ(r.spu.main_volume(0), 7 << 11);
    EXPECT_EQ(r.r16(Spu::kMainVolXL), 7 << 11);
}

TEST(SpuVolume, MainVolumeDirectReadback) {
    Rig r;
    r.w16(Spu::kMainVolR, 0x1000);
    EXPECT_EQ(r.spu.main_volume(1), 0x2000);
    EXPECT_EQ(r.r16(Spu::kMainVolXR), 0x2000);
}

TEST(SpuVolume, EnvxReadsCurrentLevelAndIsWritable) {
    Rig r;
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(3);
    EXPECT_EQ(r.r16(0xC), 0x7FFF);
    r.voice(0, 0xC, 0x1234);
    EXPECT_EQ(r.spu.voice_level(0), 0x1234);
}

TEST(SpuVolume, EnvxOfIdleVoiceReadsZero) {
    Rig r;
    EXPECT_EQ(r.r16(5 * 16 + 0xC), 0);
}

// ---- IRQ ----

TEST(SpuIrq, VoiceBlockFetchAtIrqAddressRaisesInterrupt) {
    Rig r;
    r.put_block(0x1000, 0, 0, 0x11);
    r.put_block(0x1010, 0, 0, 0x11);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.w16(Spu::kIrqAddr, 0x1010 / 8);
    r.w16(Spu::kSpuCnt, 0xC040);
    r.key_on(0);
    EXPECT_FALSE(r.irq.stat() & (1u << 9));
    r.tick(28);
    EXPECT_TRUE(r.irq.stat() & (1u << 9));
    EXPECT_TRUE(r.r16(Spu::kSpuStat) & (1u << 6));
}

TEST(SpuIrq, IrqInsideSecondHalfOfBlockAlsoFires) {
    Rig r;
    r.put_block(0x1000, 0, 0, 0x11);
    r.put_block(0x1010, 0, 0, 0x11);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.w16(Spu::kIrqAddr, 0x1018 / 8);
    r.w16(Spu::kSpuCnt, 0xC040);
    r.key_on(0);
    r.tick(28);
    EXPECT_TRUE(r.irq.stat() & (1u << 9));
}

TEST(SpuIrq, DisabledIrqDoesNotFire) {
    Rig r;
    r.put_block(0x1000, 0, 0, 0x11);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.w16(Spu::kIrqAddr, 0x1000 / 8);
    r.w16(Spu::kSpuCnt, 0xC000);
    r.key_on(0);
    r.tick(60);
    EXPECT_FALSE(r.irq.stat() & (1u << 9));
}

TEST(SpuIrq, IrqAtStartBlockFiresOnKeyOn) {
    Rig r;
    r.put_block(0x1000, 0, 0, 0x11);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.w16(Spu::kIrqAddr, 0x1000 / 8);
    r.w16(Spu::kSpuCnt, 0xC040);
    r.key_on(0);
    EXPECT_TRUE(r.irq.stat() & (1u << 9));
}

TEST(SpuIrq, AcknowledgeByClearingEnableBit) {
    Rig r;
    r.put_block(0x1000, 0, 0, 0x11);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.w16(Spu::kIrqAddr, 0x1000 / 8);
    r.w16(Spu::kSpuCnt, 0xC040);
    r.key_on(0);
    r.w16(Spu::kSpuCnt, 0xC000);
    EXPECT_FALSE(r.r16(Spu::kSpuStat) & (1u << 6));
}

TEST(SpuIrq, IdleVoicesStillTriggerIrqs) {
    Rig r;  // a voice parked after its release keeps reading RAM; here: never keyed, address 0 block
    r.put_block(0x1000, 0, 0x03, 0x00);
    r.setup(0, 0x1000, 0x1000, kFastAdsr1);
    r.key_on(0);
    r.key_off(0);
    r.tick(200);  // level reaches 0 and the voice goes Off but keeps looping
    r.w16(Spu::kIrqAddr, 0x1000 / 8);
    r.w16(Spu::kSpuCnt, 0xC040);
    r.tick(60);
    EXPECT_TRUE(r.irq.stat() & (1u << 9));
}

// ---- Capture buffers ----

TEST(SpuCapture, VoiceOneAndThreeAreCaptured) {
    Rig r;
    constant_voice(r, 1, 0x1000);
    constant_voice(r, 3, 0x1100, 0x55);
    r.key_on(1);
    r.key_on(3);
    r.tick(40);
    auto ram = r.spu.ram();
    int16_t v1 = static_cast<int16_t>(ram[0x800 + 39 * 2] | (ram[0x800 + 39 * 2 + 1] << 8));
    int16_t v3 = static_cast<int16_t>(ram[0xC00 + 39 * 2] | (ram[0xC00 + 39 * 2 + 1] << 8));
    EXPECT_EQ(v1, r.spu.voice_output(1));
    EXPECT_EQ(v3, r.spu.voice_output(3));
    EXPECT_NEAR(v3, 0x5000 * 0.9, 3000);
}

TEST(SpuCapture, WrapsAfter512Samples) {
    Rig r;
    r.tick(512);
    r.tick(3);
    EXPECT_EQ(r.r16(Spu::kSpuStat) & (1u << 11), 0);  // position 3 -> first half
    r.tick(0x100);
    EXPECT_NE(r.r16(Spu::kSpuStat) & (1u << 11), 0);
}

TEST(SpuCapture, StatBit11FollowsHalf) {
    Rig r;
    EXPECT_EQ(r.r16(Spu::kSpuStat) & (1u << 11), 0);
    r.tick(0x100);
    EXPECT_NE(r.r16(Spu::kSpuStat) & (1u << 11), 0);
    r.tick(0x100);
    EXPECT_EQ(r.r16(Spu::kSpuStat) & (1u << 11), 0);
}

TEST(SpuCapture, CaptureIrqNeedsTransferControlBits) {
    Rig r;
    r.w16(Spu::kIrqAddr, 0x000 / 8);
    r.w16(Spu::kSpuCnt, 0xC040);
    r.tick(4);
    EXPECT_FALSE(r.irq.stat() & (1u << 9));
    Rig q;
    q.w16(Spu::kTransferCtrl, 0x0004);
    q.w16(Spu::kIrqAddr, 0x000 / 8);
    q.w16(Spu::kSpuCnt, 0xC040);
    q.tick(4);
    EXPECT_TRUE(q.irq.stat() & (1u << 9));
}

// ---- CD input ----

struct FakeCd : CdAudioSource {
    int16_t l = 0, r = 0;
    int calls = 0;
    bool have = true;
    bool next_frame(int16_t& pl, int16_t& pr) override {
        ++calls;
        pl = have ? l : 0;
        pr = have ? r : 0;
        return have;
    }
};

TEST(SpuCd, CdInputMixesWhenEnabled) {
    Rig r;
    FakeCd cd;
    cd.l = 10000;
    cd.r = -6000;
    r.spu.set_cd_source(&cd);
    r.w16(Spu::kSpuCnt, 0xC001);
    r.w16(Spu::kCdVolL, 0x7FFF);
    r.w16(Spu::kCdVolR, 0x7FFF);
    r.w16(Spu::kMainVolL, 0x3FFF);
    r.w16(Spu::kMainVolR, 0x3FFF);
    r.tick(3);
    auto out = r.drain();
    EXPECT_NEAR(out[4], 10000, 5);
    EXPECT_NEAR(out[5], -6000, 5);
}

TEST(SpuCd, CdInputIgnoredWhenDisabled) {
    Rig r;
    FakeCd cd;
    cd.l = 10000;
    cd.r = 10000;
    r.spu.set_cd_source(&cd);
    r.w16(Spu::kSpuCnt, 0xC000);
    r.w16(Spu::kCdVolL, 0x7FFF);
    r.w16(Spu::kCdVolR, 0x7FFF);
    r.w16(Spu::kMainVolL, 0x3FFF);
    r.tick(3);
    for (auto v : r.drain()) EXPECT_EQ(v, 0);
    EXPECT_EQ(cd.calls, 3);  // the source is still consumed so it never backs up
}

TEST(SpuCd, CdVolumeAppliesBeforeMix) {
    Rig r;
    FakeCd cd;
    cd.l = 20000;
    cd.r = 20000;
    r.spu.set_cd_source(&cd);
    r.w16(Spu::kSpuCnt, 0xC001);
    r.w16(Spu::kCdVolL, 0x4000);
    r.w16(Spu::kCdVolR, 0);
    r.w16(Spu::kMainVolL, 0x3FFF);
    r.w16(Spu::kMainVolR, 0x3FFF);
    r.tick(2);
    auto out = r.drain();
    EXPECT_NEAR(out[2], 10000, 5);
    EXPECT_EQ(out[3], 0);
}

TEST(SpuCd, CdAudioPlaysWithSpuMutedAndDisabled) {
    Rig r;
    FakeCd cd;
    cd.l = cd.r = 8000;
    r.spu.set_cd_source(&cd);
    r.w16(Spu::kSpuCnt, 0x0001);  // SPU disabled and muted; CD input enabled
    r.w16(Spu::kCdVolL, 0x7FFF);
    r.w16(Spu::kCdVolR, 0x7FFF);
    r.w16(Spu::kMainVolL, 0x3FFF);
    r.w16(Spu::kMainVolR, 0x3FFF);
    r.tick(2);
    auto out = r.drain();
    EXPECT_NEAR(out[2], 8000, 5);
}

TEST(SpuCd, CdIsCapturedBeforeVolume) {
    Rig r;
    FakeCd cd;
    cd.l = 1234;
    cd.r = -4321;
    r.spu.set_cd_source(&cd);
    r.tick(5);
    auto ram = r.spu.ram();
    EXPECT_EQ(static_cast<int16_t>(ram[4 * 2] | (ram[4 * 2 + 1] << 8)), 1234);
    EXPECT_EQ(static_cast<int16_t>(ram[0x400 + 4 * 2] | (ram[0x400 + 4 * 2 + 1] << 8)), -4321);
}

TEST(SpuCd, CdMainVolumeApplies) {
    Rig r;
    FakeCd cd;
    cd.l = cd.r = 16000;
    r.spu.set_cd_source(&cd);
    r.w16(Spu::kSpuCnt, 0xC001);
    r.w16(Spu::kCdVolL, 0x7FFF);
    r.w16(Spu::kCdVolR, 0x7FFF);
    r.w16(Spu::kMainVolL, 0x2000);
    r.w16(Spu::kMainVolR, 0x2000);
    r.tick(2);
    auto out = r.drain();
    EXPECT_NEAR(out[2], 8000, 5);
}

// ---- Reverb integration ----

namespace {
void room_preset(Rig& r) {
    const uint16_t room[32] = {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6,
                               0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000,
                               0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000};
    for (uint32_t i = 0; i < 32; ++i) r.w16(Spu::kReverbRegs + i * 2, room[i]);
    r.w16(Spu::kReverbStart, static_cast<uint16_t>((0x80000 - 0x26C0) / 8));
}
}  // namespace

TEST(SpuReverbUnit, RegistersReadBack) {
    Rig r;
    room_preset(r);
    EXPECT_EQ(r.r16(Spu::kReverbRegs), 0x007D);
    EXPECT_EQ(r.r16(Spu::kReverbRegs + 0x3E), 0x8000);
    EXPECT_EQ(r.r16(Spu::kReverbStart), (0x80000 - 0x26C0) / 8);
}

TEST(SpuReverbUnit, EsaWriteSetsBufferAddress) {
    Rig r;
    r.w16(Spu::kReverbStart, 0x7000);
    EXPECT_EQ(r.spu.reverb().start(), 0x38000u);
    EXPECT_EQ(r.spu.reverb().current(), 0x38000u);
}

TEST(SpuReverbUnit, BufferAdvancesWithTime) {
    Rig r;
    room_preset(r);
    uint32_t a = r.spu.reverb().current();
    r.tick(100);
    EXPECT_EQ(r.spu.reverb().current(), a + 100);
}

TEST(SpuReverbUnit, MasterEnableGatesBufferWrites) {
    for (bool enable : {false, true}) {
        Rig r;
        room_preset(r);
        constant_voice(r, 0);
        r.w16(Spu::kReverbOn, 1);
        r.w16(Spu::kSpuCnt, enable ? 0xC080 : 0xC000);
        r.w16(Spu::kReverbVolL, 0x4000);
        r.w16(Spu::kReverbVolR, 0x4000);
        r.key_on(0);
        r.tick(2000);
        bool any = false;
        for (std::size_t i = 0x80000 - 0x26C0; i < 0x80000; ++i) any = any || r.spu.ram()[i] != 0;
        EXPECT_EQ(any, enable) << enable;
    }
}

TEST(SpuReverbUnit, ReverbAddsTailAfterKeyOff) {
    Rig r;
    room_preset(r);
    r.w16(Spu::kSpuCnt, 0xC080);
    r.w16(Spu::kMainVolL, 0x3FFF);
    r.w16(Spu::kMainVolR, 0x3FFF);
    r.w16(Spu::kReverbVolL, 0x3FFF);
    r.w16(Spu::kReverbVolR, 0x3FFF);
    r.w16(Spu::kReverbOn, 1);
    constant_voice(r, 0);
    r.voice(0, 0x0, 0);
    r.voice(0, 0x2, 0);  // dry path silent: only reverb can be heard
    r.voice(0, 0x0, 0x3FFF);
    r.voice(0, 0x2, 0x3FFF);
    r.voice(0, 0xA, 0x0000);  // quick release
    r.key_on(0);
    r.tick(200);
    r.key_off(0);
    r.tick(50);
    r.drain();
    r.tick(3000);
    auto out = r.drain();
    int peak = 0;
    for (auto v : out) peak = std::max(peak, std::abs(int(v)));
    EXPECT_GT(peak, 100);
}

TEST(SpuReverbUnit, NoReverbWhenVoiceNotRouted) {
    Rig r;
    room_preset(r);
    r.w16(Spu::kSpuCnt, 0xC080);
    r.w16(Spu::kMainVolL, 0x3FFF);
    r.w16(Spu::kMainVolR, 0x3FFF);
    r.w16(Spu::kReverbVolL, 0x3FFF);
    r.w16(Spu::kReverbVolR, 0x3FFF);
    constant_voice(r, 0);
    r.key_on(0);
    r.tick(200);
    r.key_off(0);
    r.voice(0, 0xA, 0x0000);
    r.tick(100);
    r.drain();
    r.tick(3000);
    for (auto v : r.drain()) ASSERT_EQ(v, 0);
}

TEST(SpuReverbUnit, CdReverbEnableBitRoutesCdIntoReverb) {
    for (bool route : {false, true}) {
        Rig r;
        FakeCd cd;
        cd.l = cd.r = 12000;
        r.spu.set_cd_source(&cd);
        room_preset(r);
        r.w16(Spu::kSpuCnt, static_cast<uint16_t>(0xC081 | (route ? 4 : 0)));
        r.w16(Spu::kCdVolL, 0x7FFF);
        r.w16(Spu::kCdVolR, 0x7FFF);
        r.tick(1500);
        bool any = false;
        for (std::size_t i = 0x80000 - 0x26C0; i < 0x80000; ++i) any = any || r.spu.ram()[i] != 0;
        EXPECT_EQ(any, route) << route;
    }
}

// ---- Timed generation ----

TEST(SpuTiming, RunToGeneratesOneFramePer768Cycles) {
    Rig r;
    r.spu.run_to(768 * 10);
    EXPECT_EQ(r.spu.frames_generated(), 10u);
    r.spu.run_to(768 * 10 + 767);
    EXPECT_EQ(r.spu.frames_generated(), 10u);
    r.spu.run_to(768 * 11);
    EXPECT_EQ(r.spu.frames_generated(), 11u);
}

TEST(SpuTiming, SchedulerDrivesBatches) {
    Rig r;
    r.spu.start(r.sched);
    int64_t t = 768 * 256 * 4;
    while (r.bus.ctx().next_event <= t) {
        r.bus.ctx().cycles = r.bus.ctx().next_event;
        r.sched.run_due();
    }
    EXPECT_EQ(r.spu.frames_generated(), 256u * 4);
}

TEST(SpuTiming, KeyOnCatchesUpToCurrentCycleFirst) {
    Rig r;
    r.spu.start(r.sched);
    constant_voice(r, 0);
    r.bus.ctx().cycles = 768 * 100;
    r.key_on(0);
    EXPECT_EQ(r.spu.frames_generated(), 100u);
}

TEST(SpuTiming, KeyOffCatchesUpToCurrentCycleFirst) {
    Rig r;
    r.spu.start(r.sched);
    r.bus.ctx().cycles = 768 * 40;
    r.key_off(0);
    EXPECT_EQ(r.spu.frames_generated(), 40u);
}

TEST(SpuTiming, HugeTimeJumpDoesNotGrind) {
    Rig r;
    r.spu.run_to(int64_t{1} << 40);
    EXPECT_LT(r.spu.frames_generated(), 100u);
}

TEST(SpuTiming, WithoutStartNothingAdvancesImplicitly) {
    Rig r;
    r.bus.ctx().cycles = 768 * 1000;
    r.key_on(0);
    EXPECT_EQ(r.spu.frames_generated(), 0u);
}

TEST(SpuTiming, DrainReturnsFramesInOrder) {
    Rig r;
    FakeCd cd;
    r.spu.set_cd_source(&cd);
    r.w16(Spu::kSpuCnt, 0xC001);
    r.w16(Spu::kCdVolL, 0x7FFF);
    r.w16(Spu::kCdVolR, 0x7FFF);
    r.w16(Spu::kMainVolL, 0x3FFF);
    r.w16(Spu::kMainVolR, 0x3FFF);
    cd.l = 100;
    cd.r = 200;
    r.tick(2);
    cd.l = 300;
    cd.r = 400;
    r.tick(1);
    std::array<int16_t, 6> out{};
    EXPECT_EQ(r.spu.drain_samples(out), 3u);
    EXPECT_NEAR(out[0], 100, 2);
    EXPECT_NEAR(out[3], 200, 2);
    EXPECT_NEAR(out[4], 300, 2);
    EXPECT_NEAR(out[5], 400, 2);
}

TEST(SpuTiming, DrainWithEmptyRingReturnsZero) {
    Rig r;
    std::array<int16_t, 8> out{};
    EXPECT_EQ(r.spu.drain_samples(out), 0u);
}

