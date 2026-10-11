// XA-ADPCM decoder, 37.8 -> 44.1 kHz resampler, CD volume matrix and the audio ring buffer.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "dw3/audio_ring.h"
#include "dw3/bus.h"
#include "dw3/cdrom.h"
#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"
#include "dw3/spu_dsp.h"
#include "dw3/xa.h"

using namespace dw3;

namespace {

constexpr uint8_t kStereo = 1, kHalfRate = 4, kEightBit = 0x10;

// Raw 2352-byte sector builder: sound group g starts at 24 + 128*g.
struct Sector {
    std::array<uint8_t, 2352> raw{};
    explicit Sector(uint8_t coding = kStereo) { raw[19] = raw[23] = coding; }
    uint8_t* group(int g) { return raw.data() + 24 + 128 * g; }
    void header(int g, int unit, uint8_t h) {
        group(g)[4 + unit] = h;
        group(g)[unit < 4 ? unit : 8 + unit - 4] = h;  // copies
    }
    // 4-bit: sample j of unit u lives in data word j, byte u/2, nibble u%2.
    void nib(int g, int unit, int j, unsigned v) {
        uint8_t& b = group(g)[16 + j * 4 + unit / 2];
        unsigned sh = (unit & 1) * 4;
        b = static_cast<uint8_t>((b & ~(0xF << sh)) | ((v & 0xF) << sh));
    }
    void fill_nib(int g, int unit, unsigned v) {
        for (int j = 0; j < 28; ++j) nib(g, unit, j, v);
    }
    // 8-bit: sample j of unit u is byte u of data word j.
    void byte(int g, int unit, int j, uint8_t v) { group(g)[16 + j * 4 + unit] = v; }
};

struct Decoded {
    std::array<std::vector<int16_t>, 2> ch;
    bool stereo = false, half = false;
    std::size_t n = 0;
};

int s4(int v) { return v > 7 ? v - 16 : v; }  // signed 4-bit

Decoded decode(const Sector& s, XaDecoder& d) {
    Decoded r;
    r.n = d.decode_sector(s.raw.data(), r.ch, r.stereo, r.half);
    return r;
}

struct CdRig {
    Bus bus;
    Scheduler sched{bus.ctx()};
    IrqController irq{bus};
    Dma dma{bus, sched, irq};
    Cdrom cd{bus, sched, irq, dma};
    void reg(unsigned r, uint8_t v) { bus.write(0x1F801800 + r, v, 1); }
    void idx(unsigned i) { reg(0, static_cast<uint8_t>(i)); }
    void set_volume(uint8_t ll, uint8_t lr, uint8_t rl, uint8_t rr, bool mute = false) {
        idx(2);
        reg(2, ll);
        reg(3, lr);
        idx(3);
        reg(1, rr);
        reg(2, rl);
        reg(3, static_cast<uint8_t>(0x20 | (mute ? 1 : 0)));
    }
};

}  // namespace

// ---- Sector decode ----

TEST(XaDecode, FourBitStereoLengths) {
    XaDecoder d;
    Sector s(kStereo);
    auto r = decode(s, d);
    EXPECT_TRUE(r.stereo);
    EXPECT_FALSE(r.half);
    EXPECT_EQ(r.ch[0].size(), 2016u);
    EXPECT_EQ(r.ch[1].size(), 2016u);
}

TEST(XaDecode, FourBitMonoLengths) {
    XaDecoder d;
    Sector s(0);
    auto r = decode(s, d);
    EXPECT_FALSE(r.stereo);
    EXPECT_EQ(r.ch[0].size(), 4032u);
    EXPECT_TRUE(r.ch[1].empty());
}

TEST(XaDecode, EightBitStereoLengths) {
    XaDecoder d;
    Sector s(kStereo | kEightBit);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0].size(), 1008u);
    EXPECT_EQ(r.ch[1].size(), 1008u);
}

TEST(XaDecode, EightBitMonoLengths) {
    XaDecoder d;
    Sector s(kEightBit);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0].size(), 2016u);
}

TEST(XaDecode, CodingByteFlags) {
    XaDecoder d;
    for (uint8_t coding : {uint8_t(0), kStereo, kHalfRate, uint8_t(kStereo | kHalfRate)}) {
        Sector s(coding);
        auto r = decode(s, d);
        EXPECT_EQ(r.stereo, (coding & kStereo) != 0) << int(coding);
        EXPECT_EQ(r.half, (coding & kHalfRate) != 0) << int(coding);
    }
}

TEST(XaDecode, StereoLeftRightUnitsLiteral) {
    XaDecoder d;
    Sector s(kStereo);
    s.header(0, 0, 0x0C);  // range 12, filter 0: sample = signed nibble
    s.header(0, 1, 0x0C);
    s.fill_nib(0, 0, 5);
    s.fill_nib(0, 1, 0xF);  // -1
    auto r = decode(s, d);
    for (int j = 0; j < 28; ++j) {
        EXPECT_EQ(r.ch[0][static_cast<std::size_t>(j)], 5) << j;
        EXPECT_EQ(r.ch[1][static_cast<std::size_t>(j)], -1) << j;
    }
    EXPECT_EQ(r.ch[0][28], 0);  // second unit pair untouched
}

TEST(XaDecode, MonoUsesUnitsInTimeOrder) {
    XaDecoder d;
    Sector s(0);
    for (int u = 0; u < 8; ++u) {
        s.header(0, u, 0x0C);
        s.fill_nib(0, u, static_cast<unsigned>(u + 1));
    }
    auto r = decode(s, d);
    for (int u = 0; u < 8; ++u) EXPECT_EQ(r.ch[0][static_cast<std::size_t>(u * 28 + 3)], s4(u + 1)) << u;
}

TEST(XaDecode, StereoBlocksInterleaveLeftRight) {
    XaDecoder d;
    Sector s(kStereo);
    for (int u = 0; u < 8; ++u) {
        s.header(0, u, 0x0C);
        s.fill_nib(0, u, static_cast<unsigned>(u + 1));
    }
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][0], 1);
    EXPECT_EQ(r.ch[1][0], 2);
    EXPECT_EQ(r.ch[0][28], 3);
    EXPECT_EQ(r.ch[1][28], 4);
    EXPECT_EQ(r.ch[0][84], 7);
    EXPECT_EQ(r.ch[1][84], -8);  // nibble 8 is -8
}

TEST(XaDecode, RangeShiftsSampleDown) {
    XaDecoder d;
    Sector s(kStereo);
    s.header(0, 0, 0x08);  // range 8: (7 << 12) >> 8 = 112
    s.fill_nib(0, 0, 7);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][0], 112);
}

TEST(XaDecode, RangeAbove12ActsAsNine) {
    XaDecoder d;
    Sector s(kStereo);
    s.header(0, 0, 0x0D);
    s.fill_nib(0, 0, 7);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][0], 28672 >> 9);
}

TEST(XaDecode, Filter1HandComputed) {
    XaDecoder d;
    Sector s(kStereo);
    s.header(0, 0, 0x1C);  // filter 1, range 12
    s.fill_nib(0, 0, 1);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][0], 1);
    EXPECT_EQ(r.ch[0][1], 2);
    EXPECT_EQ(r.ch[0][2], 3);
    EXPECT_EQ(r.ch[0][3], 4);
}

TEST(XaDecode, Filter3UsesBothTaps) {
    XaDecoder d;
    Sector s(kStereo);
    s.header(0, 0, 0x3C);  // f0 = 98, f1 = -55
    s.nib(0, 0, 0, 4);
    s.nib(0, 0, 1, 0);
    s.nib(0, 0, 2, 0);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][0], 4);
    EXPECT_EQ(r.ch[0][1], (4 * 98 + 32) >> 6);               // 6
    EXPECT_EQ(r.ch[0][2], (6 * 98 + 4 * -55 + 32) >> 6);     // 6
}

TEST(XaDecode, Filter4IsTreatedAsFilter3Bits) {
    // Only two filter bits exist in XA headers: bit 6 is "unused" and ignored.
    XaDecoder d1, d2;
    Sector a(kStereo), b(kStereo);
    a.header(0, 0, 0x3C);
    b.header(0, 0, 0x7C);
    for (Sector* s : {&a, &b}) s->fill_nib(0, 0, 3);
    EXPECT_EQ(decode(a, d1).ch[0], decode(b, d2).ch[0]);
}

TEST(XaDecode, EightBitSamples) {
    XaDecoder d;
    Sector s(kStereo | kEightBit);
    s.header(0, 0, 0x08);  // range 8: sample = signed byte
    s.header(0, 1, 0x08);
    s.byte(0, 0, 0, 0x10);
    s.byte(0, 1, 0, 0xF0);  // -16
    s.byte(0, 0, 1, 0x7F);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][0], 16);
    EXPECT_EQ(r.ch[1][0], -16);
    EXPECT_EQ(r.ch[0][1], 127);
}

TEST(XaDecode, EightBitMonoTimeOrder) {
    XaDecoder d;
    Sector s(kEightBit);
    for (int u = 0; u < 4; ++u) {
        s.header(0, u, 0x08);
        s.byte(0, u, 0, static_cast<uint8_t>(10 * (u + 1)));
    }
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][0], 10);
    EXPECT_EQ(r.ch[0][28], 20);
    EXPECT_EQ(r.ch[0][56], 30);
    EXPECT_EQ(r.ch[0][84], 40);
}

TEST(XaDecode, AllEighteenGroupsAreDecoded) {
    XaDecoder d;
    Sector s(kStereo);
    s.header(17, 6, 0x0C);
    s.fill_nib(17, 6, 3);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][17 * 112 + 3 * 28 + 5], 3);
    EXPECT_EQ(r.ch[0][16 * 112 + 3 * 28 + 5], 0);
}

TEST(XaDecode, HistoryPersistsAcrossSectors) {
    XaDecoder d;
    Sector s(kStereo);
    for (int u : {0, 2, 4, 6}) {  // left units of the last group settle at the filter-1 fixed point 9
        s.header(17, u, 0x1C);
        s.fill_nib(17, u, 1);
    }
    auto a = decode(s, d);
    EXPECT_EQ(a.ch[0][17 * 112 + 4 * 28 - 1], 9);
    Sector t(kStereo);
    t.header(0, 0, 0x1C);  // nibble 0: output is pure feedback from the previous sector's history
    auto b = decode(t, d);
    EXPECT_EQ(b.ch[0][0], (9 * 60 + 32) >> 6);
    XaDecoder fresh;
    EXPECT_EQ(decode(t, fresh).ch[0][0], 0);
}

TEST(XaDecode, ResetClearsHistory) {
    XaDecoder d;
    Sector s(kStereo);
    s.header(0, 0, 0x1C);
    s.fill_nib(0, 0, 1);
    decode(s, d);
    d.reset();
    auto b = decode(s, d);
    EXPECT_EQ(b.ch[0][0], 1);
}

TEST(XaDecode, SaturatesAtInt16) {
    XaDecoder d;
    Sector s(kStereo);
    s.header(0, 0, 0x30);  // filter 3, range 0: full-scale nibbles plus feedback overflow
    s.fill_nib(0, 0, 7);
    auto r = decode(s, d);
    EXPECT_EQ(r.ch[0][0], 28672);
    EXPECT_EQ(r.ch[0][1], 32767);
}

TEST(XaDecode, SilentSectorDecodesToZero) {
    XaDecoder d;
    Sector s(kStereo);
    auto r = decode(s, d);
    for (auto v : r.ch[0]) ASSERT_EQ(v, 0);
}

// ---- Resampler ----

TEST(XaResample, SixInputsGiveSevenFrames) {
    XaDecoder d;
    for (int i = 0; i < 5; ++i) d.resample_push(100, 100);
    EXPECT_EQ(d.queued(), 0u);
    d.resample_push(100, 100);
    EXPECT_EQ(d.queued(), 7u);
}

class ResampleRatio : public ::testing::TestWithParam<int> {};

TEST_P(ResampleRatio, SevenOutputsPerSixInputs) {
    int groups = GetParam();
    XaDecoder d;
    for (int i = 0; i < groups * 6; ++i) d.resample_push(static_cast<int16_t>(i), 0);
    EXPECT_EQ(d.queued(), static_cast<std::size_t>(groups * 7));
}

INSTANTIATE_TEST_SUITE_P(Counts, ResampleRatio, ::testing::Values(1, 2, 3, 10, 56, 336));

TEST(XaResample, SilenceStaysSilent) {
    XaDecoder d;
    for (int i = 0; i < 60; ++i) d.resample_push(0, 0);
    int16_t f[2];
    while (d.fifo().pop(f)) EXPECT_EQ((int(f[0]) | int(f[1])), 0);
}

TEST(XaResample, DcGainIsAboutNinetyOnePercent) {
    XaDecoder d;
    for (int i = 0; i < 300; ++i) d.resample_push(10000, -10000);
    int16_t f[2];
    std::vector<int16_t> l, r;
    while (d.fifo().pop(f)) {
        l.push_back(f[0]);
        r.push_back(f[1]);
    }
    for (std::size_t i = l.size() - 14; i < l.size(); ++i) {
        EXPECT_NEAR(l[i], 9060, 100);
        EXPECT_NEAR(r[i], -9060, 100);
    }
}

TEST(XaResample, ChannelsAreIndependent) {
    XaDecoder d;
    for (int i = 0; i < 120; ++i) d.resample_push(5000, 0);
    int16_t f[2];
    while (d.fifo().pop(f)) EXPECT_EQ(f[1], 0);
}

TEST(XaResample, ZigzagTablesSumToAbout91Percent) {
    for (std::size_t t = 0; t < 7; ++t) {
        int sum = 0;
        for (int v : spu_dsp::kZigzag[t]) sum += v;
        EXPECT_GT(sum, 29000) << t;
        EXPECT_LT(sum, 30000) << t;
    }
}

TEST(XaResample, ImpulseResponseUsesTableEntries) {
    XaDecoder d;
    d.resample_push(32767, 0);
    for (int i = 0; i < 5; ++i) d.resample_push(0, 0);
    // After 6 inputs the impulse is the oldest of the ring window: tap index i = 6 (p - 6 = 0).
    int16_t f[2];
    for (std::size_t t = 0; t < 7; ++t) {
        ASSERT_EQ(d.fifo().pop(f), 1u);
        EXPECT_EQ(f[0], spu_dsp::clamp16((32767 * spu_dsp::kZigzag[t][5]) / 0x8000)) << t;
    }
}

// ---- Sector -> FIFO ----

TEST(XaSink, StereoFullRateFrameCount) {
    XaDecoder d;
    Sector s(kStereo);
    d.xa_sector(s.raw.data());
    EXPECT_EQ(d.queued(), 2016u * 7 / 6);
    EXPECT_EQ(d.sectors(), 1u);
}

TEST(XaSink, HalfRateDoublesFrameCount) {
    XaDecoder d;
    Sector s(kStereo | kHalfRate);
    d.xa_sector(s.raw.data());
    EXPECT_EQ(d.queued(), 2016u * 2 * 7 / 6);
}

TEST(XaSink, MonoDuplicatesToBothChannels) {
    XaDecoder d;
    Sector s(0);
    for (int u = 0; u < 8; ++u) {
        s.header(0, u, 0x0C);
        s.fill_nib(0, u, 5);
    }
    d.xa_sector(s.raw.data());
    EXPECT_EQ(d.queued(), 4032u * 7 / 6);
    int16_t f[2];
    for (int i = 0; i < 50; ++i) {
        d.fifo().pop(f);
        EXPECT_EQ(f[0], f[1]);
    }
}

TEST(XaSink, SectorsAccumulateInOrder) {
    XaDecoder d;
    Sector s(kStereo);
    d.xa_sector(s.raw.data());
    d.xa_sector(s.raw.data());
    EXPECT_EQ(d.queued(), 2u * 2016 * 7 / 6);
}

TEST(XaSink, FifoDropsOldestWhenFull) {
    XaDecoder d(nullptr, 1000);
    Sector s(kStereo);
    d.xa_sector(s.raw.data());
    EXPECT_EQ(d.queued(), 1000u);
    EXPECT_GT(d.fifo().overruns(), 0u);
}

// ---- CD volume matrix / mute ----

TEST(XaVolume, DefaultIsStraightThrough) {
    CdRig c;
    XaDecoder d(&c.cd);
    d.fifo().push(1000, -2000);
    int16_t l, r;
    EXPECT_TRUE(d.next_frame(l, r));
    EXPECT_EQ(l, 1000);
    EXPECT_EQ(r, -2000);
}

TEST(XaVolume, MatrixMixesChannels) {
    CdRig c;
    c.set_volume(0x40, 0x40, 0x40, 0x40);  // both outputs = (L + R) / 2
    XaDecoder d(&c.cd);
    d.fifo().push(1000, 3000);
    int16_t l, r;
    d.next_frame(l, r);
    EXPECT_EQ(l, 2000);
    EXPECT_EQ(r, 2000);
}

TEST(XaVolume, SwapChannels) {
    CdRig c;
    c.set_volume(0, 0x80, 0x80, 0);
    XaDecoder d(&c.cd);
    d.fifo().push(1000, -500);
    int16_t l, r;
    d.next_frame(l, r);
    EXPECT_EQ(l, -500);
    EXPECT_EQ(r, 1000);
}

TEST(XaVolume, DoubleVolumeSaturates) {
    CdRig c;
    c.set_volume(0xFF, 0, 0, 0xFF);
    XaDecoder d(&c.cd);
    d.fifo().push(20000, -20000);
    int16_t l, r;
    d.next_frame(l, r);
    EXPECT_EQ(l, 32767);
    EXPECT_EQ(r, -32768);
}

TEST(XaVolume, ZeroVolumeSilences) {
    CdRig c;
    c.set_volume(0, 0, 0, 0);
    XaDecoder d(&c.cd);
    d.fifo().push(1000, 1000);
    int16_t l, r;
    d.next_frame(l, r);
    EXPECT_EQ(l, 0);
    EXPECT_EQ(r, 0);
}

TEST(XaVolume, MuteSilencesButConsumes) {
    CdRig c;
    c.set_volume(0x80, 0, 0, 0x80, true);
    ASSERT_TRUE(c.cd.adpcm_muted());
    XaDecoder d(&c.cd);
    d.fifo().push(1000, 1000);
    int16_t l, r;
    EXPECT_TRUE(d.next_frame(l, r));
    EXPECT_EQ(l, 0);
    EXPECT_EQ(d.queued(), 0u);
}

TEST(XaVolume, EmptyFifoIsSilenceAndReportsFalse) {
    XaDecoder d;
    int16_t l = 7, r = 7;
    EXPECT_FALSE(d.next_frame(l, r));
    EXPECT_EQ(l, 0);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(d.fifo().underruns(), 0u);
}

TEST(XaVolume, HalfVolume) {
    CdRig c;
    c.set_volume(0x40, 0, 0, 0x40);
    XaDecoder d(&c.cd);
    d.fifo().push(8000, 4000);
    int16_t l, r;
    d.next_frame(l, r);
    EXPECT_EQ(l, 4000);
    EXPECT_EQ(r, 2000);
}

// ---- AudioRing ----

TEST(AudioRing, PushPopFifoOrder) {
    AudioRing ring(8);
    for (int i = 0; i < 5; ++i) ring.push(static_cast<int16_t>(i), static_cast<int16_t>(-i));
    std::array<int16_t, 10> out{};
    EXPECT_EQ(ring.pop(out), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(out[static_cast<std::size_t>(i * 2)], i);
        EXPECT_EQ(out[static_cast<std::size_t>(i * 2 + 1)], -i);
    }
    EXPECT_EQ(ring.size(), 0u);
}

TEST(AudioRing, OverrunDropsOldest) {
    AudioRing ring(4);
    for (int i = 0; i < 6; ++i) ring.push(static_cast<int16_t>(i), 0);
    EXPECT_EQ(ring.size(), 4u);
    EXPECT_EQ(ring.overruns(), 2u);
    std::array<int16_t, 8> out{};
    ring.pop(out);
    EXPECT_EQ(out[0], 2);
    EXPECT_EQ(out[6], 5);
}

TEST(AudioRing, UnderrunCountsMissingFrames) {
    AudioRing ring(4);
    ring.push(1, 1);
    std::array<int16_t, 8> out{};
    EXPECT_EQ(ring.pop(out), 1u);
    EXPECT_EQ(ring.underruns(), 3u);
}

TEST(AudioRing, WrapsAroundCapacity) {
    AudioRing ring(3);
    std::array<int16_t, 4> out{};
    for (int round = 0; round < 10; ++round) {
        ring.push(static_cast<int16_t>(round), 0);
        ring.push(static_cast<int16_t>(round + 100), 0);
        EXPECT_EQ(ring.pop(out), 2u);
        EXPECT_EQ(out[0], round);
        EXPECT_EQ(out[2], round + 100);
    }
}

TEST(AudioRing, PopWithOddSpanSizeUsesWholeFrames) {
    AudioRing ring(4);
    ring.push(1, 2);
    ring.push(3, 4);
    std::array<int16_t, 3> out{};
    EXPECT_EQ(ring.pop(out), 1u);
    EXPECT_EQ(ring.size(), 1u);
}

TEST(AudioRing, ClearEmpties) {
    AudioRing ring(4);
    ring.push(1, 1);
    ring.clear();
    EXPECT_EQ(ring.size(), 0u);
}

TEST(AudioRing, CapacityReported) {
    AudioRing ring(100);
    EXPECT_EQ(ring.capacity(), 100u);
}
