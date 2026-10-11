#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "dw3/cdrom.h"
#include "hw_test_util.h"

using dw3::Cdrom;
using dw3::Dma;
using dw3::IrqController;
using dw3::SectorSource;
using dw3::XaSink;
using Bytes = std::vector<uint8_t>;

namespace {
constexpr uint32_t kReg = 0x1F801800u;
constexpr int64_t kP1 = Cdrom::kCyclesPerSector1x, kP2 = Cdrom::kCyclesPerSector2x;
constexpr uint8_t kRtAudio = 0x64;  // form2 | real-time | audio
constexpr uint8_t kData = 0x08;     // end-of-record style data submode

uint8_t bcd(unsigned v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
uint8_t pat(uint32_t lba, int i) { return static_cast<uint8_t>(lba * 5 + i * 3 + 1); }

struct FakeDisc : SectorSource {
    std::vector<std::array<uint8_t, 2352>> s;
    uint32_t reads = 0;
    bool fail = false;
    explicit FakeDisc(uint32_t n = 300) {
        for (uint32_t l = 0; l < n; ++l) s.push_back(make(l, 1, 0, kData));
    }
    static std::array<uint8_t, 2352> make(uint32_t lba, uint8_t file, uint8_t chan, uint8_t sm) {
        std::array<uint8_t, 2352> r{};
        r[0] = 0;
        for (int i = 1; i < 11; ++i) r[i] = 0xFF;
        r[11] = 0;
        uint32_t a = lba + 150;
        r[12] = bcd(a / 75 / 60);
        r[13] = bcd(a / 75 % 60);
        r[14] = bcd(a % 75);
        r[15] = 2;
        r[16] = r[20] = file;
        r[17] = r[21] = chan;
        r[18] = r[22] = sm;
        r[19] = r[23] = 0;
        for (int i = 24; i < 2352; ++i) r[i] = pat(lba, i);
        return r;
    }
    uint32_t sector_count() const override { return static_cast<uint32_t>(s.size()); }
    bool read_raw(uint32_t lba, uint8_t out[2352]) override {
        ++reads;
        if (fail || lba >= s.size()) return false;
        std::memcpy(out, s[lba].data(), 2352);
        return true;
    }
};

struct Sink : XaSink {
    std::vector<std::array<uint8_t, 2352>> got;
    void xa_sector(const uint8_t* raw) override {
        std::array<uint8_t, 2352> a;
        std::memcpy(a.data(), raw, 2352);
        got.push_back(a);
    }
    // LBA recovered from the header MSF.
    static int lba_of(const std::array<uint8_t, 2352>& a) {
        auto d = [](uint8_t b) { return (b >> 4) * 10 + (b & 15); };
        return (d(a[12]) * 60 + d(a[13])) * 75 + d(a[14]) - 150;
    }
};

struct Reply {
    int n = 0;
    Bytes d;
    int64_t t = 0;
};

struct Ev {
    int n;
    int64_t t;
    int64_t next_lba;
};

struct Rig : dw3test::Clock {
    IrqController irq{bus};
    Dma dma{bus, sched, irq};
    FakeDisc disc;
    Sink sink;
    Cdrom cd{bus, sched, irq, dma};

    explicit Rig(uint32_t sectors = 300) : disc(sectors) {
        cd.set_disc(&disc);
        cd.set_xa_sink(&sink);
    }
    uint32_t r8(uint32_t off) { return bus.read(kReg + off, 1); }
    void idx(int i) { w8(kReg, i); }
    uint8_t status() { return static_cast<uint8_t>(r8(0)); }
    void send(uint8_t cmd, Bytes params = {}) {
        idx(0);
        for (auto p : params) w8(kReg + 2, p);
        w8(kReg + 1, cmd);
    }
    Bytes drain_resp() {
        Bytes b;
        while (status() & Cdrom::kStsRespReady) b.push_back(static_cast<uint8_t>(r8(1)));
        return b;
    }
    void ack(uint8_t v = 0x1F) {
        idx(1);
        w8(kReg + 3, v);
    }
    // Runs until an interrupt is pending (or `limit` cycles pass), reads the response and acknowledges it.
    Reply wait(int64_t limit = 40000000) {
        int64_t end = ctx().cycles + limit;
        while (cd.irq_flag() == 0 && ctx().next_event <= end) {
            ctx().cycles = std::max(ctx().cycles, ctx().next_event);
            sched.run_due();
        }
        Reply r;
        r.n = cd.irq_flag();
        r.t = ctx().cycles;
        if (r.n) {
            r.d = drain_resp();
            ack();
        }
        return r;
    }
    Reply cmd(uint8_t c, Bytes p = {}) {
        send(c, p);
        return wait();
    }
    // Runs to `until`, acknowledging every interrupt as it appears; returns them.
    std::vector<Ev> run_acking(int64_t until) {
        std::vector<Ev> out;
        while (ctx().next_event <= until) {
            ctx().cycles = std::max(ctx().cycles, ctx().next_event);
            sched.run_due();
            if (cd.irq_flag()) {
                out.push_back({cd.irq_flag(), ctx().cycles, cd.next_lba()});
                drain_resp();
                ack();
            }
        }
        ctx().cycles = std::max(ctx().cycles, until);
        return out;
    }
    static Bytes msf(int lba) {
        unsigned a = static_cast<unsigned>(lba + 150);
        return {bcd(a / 75 / 60), bcd(a / 75 % 60), bcd(a % 75)};
    }
    void setloc(int lba) {
        Reply r = cmd(0x02, msf(lba));
        ASSERT_EQ(r.n, 3);
    }
    // Starts reading at `lba` and consumes the INT3.
    void start_read(int lba, uint8_t c = 0x06) {
        setloc(lba);
        Reply r = cmd(c);
        ASSERT_EQ(r.n, 3);
    }
    void bfrd() {
        idx(0);
        w8(kReg + 3, 0x80);
    }
    Bytes read_fifo(size_t n) {
        Bytes b;
        for (size_t i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(r8(2)));
        return b;
    }
    void set_mode(uint8_t m) {
        Reply r = cmd(0x0E, {m});
        ASSERT_EQ(r.n, 3);
    }
    int count_int(const std::vector<Ev>& v, int n) {
        int c = 0;
        for (auto& e : v) c += e.n == n;
        return c;
    }
};
}  // namespace

// ---- registers / FIFOs ----

TEST(CdRegs, StatusAfterReset) {
    Rig r;
    EXPECT_EQ(r.status(), Cdrom::kStsParamEmpty | Cdrom::kStsParamReady);
}

class CdIndex : public ::testing::TestWithParam<int> {};
TEST_P(CdIndex, IndexBitsReadBack) {
    Rig r;
    r.idx(GetParam());
    EXPECT_EQ(r.status() & 3, GetParam());
}
INSTANTIATE_TEST_SUITE_P(All, CdIndex, ::testing::Values(0, 1, 2, 3));

TEST(CdRegs, IndexIgnoresUpperBitsOnWrite) {
    Rig r;
    r.w8(kReg, 0xFE);
    EXPECT_EQ(r.status() & 3, 2);
}

TEST(CdRegs, ParamFifoStatusBits) {
    Rig r;
    r.idx(0);
    r.w8(kReg + 2, 1);
    EXPECT_FALSE(r.status() & Cdrom::kStsParamEmpty);
    EXPECT_TRUE(r.status() & Cdrom::kStsParamReady);
}

TEST(CdRegs, ParamFifoFullClearsReady) {
    Rig r;
    r.idx(0);
    for (int i = 0; i < 16; ++i) r.w8(kReg + 2, static_cast<uint32_t>(i));
    EXPECT_FALSE(r.status() & Cdrom::kStsParamReady);
}

TEST(CdRegs, ParamFifoOverflowIgnored) {
    Rig r;
    r.idx(0);
    for (int i = 0; i < 20; ++i) r.w8(kReg + 2, 0x55);
    r.ack(0x40);
    EXPECT_TRUE(r.status() & Cdrom::kStsParamEmpty);
}

TEST(CdRegs, AckBit6ResetsParamFifo) {
    Rig r;
    r.idx(0);
    r.w8(kReg + 2, 9);
    r.ack(0x40);
    EXPECT_TRUE(r.status() & Cdrom::kStsParamEmpty);
}

TEST(CdRegs, AckWithoutBit6KeepsParams) {
    Rig r;
    r.idx(0);
    r.w8(kReg + 2, 9);
    r.ack(0x07);
    EXPECT_FALSE(r.status() & Cdrom::kStsParamEmpty);
}

TEST(CdRegs, InterruptEnableReadsViaIndex0And2) {
    Rig r;
    r.idx(1);
    r.w8(kReg + 2, 0x1B);
    r.idx(0);
    EXPECT_EQ(r.r8(3), 0xE0u | 0x1B);
    r.idx(2);
    EXPECT_EQ(r.r8(3), 0xE0u | 0x1B);
    EXPECT_EQ(r.cd.irq_enable(), 0x1B);
}

TEST(CdRegs, InterruptEnableMasksTo5Bits) {
    Rig r;
    r.idx(1);
    r.w8(kReg + 2, 0xFF);
    EXPECT_EQ(r.cd.irq_enable(), 0x1F);
}

TEST(CdRegs, FlagReadsViaIndex1And3) {
    Rig r;
    r.send(0x01);
    r.wait();  // consumed + acked
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.idx(1);
    EXPECT_EQ(r.r8(3), 0xE0u | 3);
    r.idx(3);
    EXPECT_EQ(r.r8(3), 0xE0u | 3);
}

TEST(CdRegs, FlagUpperBitsReadAsOnes) {
    Rig r;
    r.idx(1);
    EXPECT_EQ(r.r8(3), 0xE0u);
}

TEST(CdRegs, ResponseReadableFromAnyIndex) {
    for (int i = 0; i < 4; ++i) {
        Rig r;
        r.send(0x01);
        while (r.cd.irq_flag() == 0) {
            r.ctx().cycles = r.ctx().next_event;
            r.sched.run_due();
        }
        r.idx(i);
        EXPECT_EQ(r.r8(1), 0x02u) << i;
    }
}

TEST(CdRegs, ResponseFifoDrainsAndReadyClears) {
    Rig r;
    r.send(0x13);  // GetTN: 3 bytes
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    EXPECT_TRUE(r.status() & Cdrom::kStsRespReady);
    r.r8(1);
    r.r8(1);
    EXPECT_TRUE(r.status() & Cdrom::kStsRespReady);
    r.r8(1);
    EXPECT_FALSE(r.status() & Cdrom::kStsRespReady);
    EXPECT_EQ(r.r8(1), 0u);
}

TEST(CdRegs, BusyBitFromCommandUntilResponse) {
    Rig r;
    r.send(0x01);
    EXPECT_TRUE(r.status() & Cdrom::kStsBusy);
    r.ctx().cycles = r.ctx().next_event;
    r.sched.run_due();
    EXPECT_FALSE(r.status() & Cdrom::kStsBusy);
}

TEST(CdRegs, CommandIgnoredWhileBusy) {
    Rig r;
    r.send(0x01);
    r.send(0x1A);  // would add a second response
    Reply a = r.wait();
    EXPECT_EQ(a.n, 3);
    r.run_to(r.ctx().cycles + 1000000);
    EXPECT_EQ(r.cd.irq_flag(), 0);
}

TEST(CdRegs, ParamsClearedWhenCommandStarts) {
    Rig r;
    r.send(0x0E, {0x20});
    EXPECT_TRUE(r.status() & Cdrom::kStsParamEmpty);
}

TEST(CdRegs, VolumeAppliedOnlyOnApply) {
    Rig r;
    r.idx(2);
    r.w8(kReg + 2, 0x11);  // LL
    r.w8(kReg + 3, 0x22);  // LR
    r.idx(3);
    r.w8(kReg + 1, 0x33);  // RR
    r.w8(kReg + 2, 0x44);  // RL
    EXPECT_EQ(r.cd.volume().ll, 0x80);
    r.w8(kReg + 3, 0x20);
    EXPECT_EQ(r.cd.volume().ll, 0x11);
    EXPECT_EQ(r.cd.volume().lr, 0x22);
    EXPECT_EQ(r.cd.volume().rr, 0x33);
    EXPECT_EQ(r.cd.volume().rl, 0x44);
    EXPECT_FALSE(r.cd.adpcm_muted());
}

TEST(CdRegs, AdpcmMuteBit) {
    Rig r;
    r.idx(3);
    r.w8(kReg + 3, 0x01);
    EXPECT_TRUE(r.cd.adpcm_muted());
    r.w8(kReg + 3, 0x00);
    EXPECT_FALSE(r.cd.adpcm_muted());
}

TEST(CdRegs, DefaultVolume) {
    Rig r;
    EXPECT_EQ(r.cd.volume().ll, 0x80);
    EXPECT_EQ(r.cd.volume().rr, 0x80);
    EXPECT_EQ(r.cd.volume().lr, 0);
}

TEST(CdRegs, SoundMapWritesAreHarmless) {
    Rig r;
    r.idx(1);
    r.w8(kReg + 1, 0x12);
    r.idx(2);
    r.w8(kReg + 1, 0x34);
    EXPECT_EQ(r.cd.irq_flag(), 0);
}

TEST(CdRegs, HalfwordWriteWritesTwoRegisters) {
    Rig r;
    r.w16(kReg, 0x0001);  // index 1 then (1801 idx1 = sound map)
    EXPECT_EQ(r.status() & 3, 1);
}

TEST(CdRegs, ResetRestoresState) {
    Rig r;
    r.cmd(0x0E, {0x80});
    r.idx(1);
    r.w8(kReg + 2, 0x1F);
    r.cd.reset();
    EXPECT_EQ(r.cd.mode(), 0);
    EXPECT_EQ(r.cd.irq_enable(), 0);
    EXPECT_EQ(r.cd.irq_flag(), 0);
    EXPECT_EQ(r.status(), Cdrom::kStsParamEmpty | Cdrom::kStsParamReady);
    EXPECT_EQ(r.cd.stat(), Cdrom::kStatMotor);
}

TEST(CdRegs, ResetDropsInFlightCommand) {
    Rig r;
    r.send(0x01);
    r.cd.reset();
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_EQ(r.cd.irq_flag(), 0);
}

// ---- simple commands ----

TEST(CdCmd, GetstatReturnsStatWithMotor) {
    Rig r;
    Reply a = r.cmd(0x01);
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x02}));
}

TEST(CdCmd, FirstResponseLatency) {
    Rig r;
    r.send(0x01);
    Reply a = r.wait();
    EXPECT_EQ(a.t, Cdrom::kFirstResponse);
}

TEST(CdCmd, NoDiscStatIsMotorOff) {
    Rig r;
    r.cd.set_disc(nullptr);
    EXPECT_EQ(r.cmd(0x01).d, (Bytes{0x00}));
}

TEST(CdCmd, ShellOpenBitInStat) {
    Rig r;
    r.cd.set_shell_open(true);
    EXPECT_EQ(r.cmd(0x01).d, (Bytes{0x12}));
}

TEST(CdCmd, SetlocOk) {
    Rig r;
    Reply a = r.cmd(0x02, {0x00, 0x02, 0x00});
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x02}));
}

TEST(CdCmd, SetlocRejectsNonBcd) {
    Rig r;
    Reply a = r.cmd(0x02, {0x00, 0x0A, 0x00});
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d, (Bytes{0x03, 0x10}));
}

TEST(CdCmd, SetlocRejectsSeconds60) {
    Rig r;
    EXPECT_EQ(r.cmd(0x02, {0x00, 0x60, 0x00}).n, 5);
}

TEST(CdCmd, SetlocRejectsFrame75) {
    Rig r;
    EXPECT_EQ(r.cmd(0x02, {0x00, 0x02, 0x75}).n, 5);
}

TEST(CdCmd, SetlocAcceptsFrame74) {
    Rig r;
    EXPECT_EQ(r.cmd(0x02, {0x00, 0x02, 0x74}).n, 3);
}

TEST(CdCmd, PlayStubGivesInt3ThenPlays) {
    Rig r;
    EXPECT_EQ(r.cmd(0x03).d, (Bytes{0x02}));
    r.run_to(r.ctx().cycles + 3 * kP1);
    EXPECT_EQ(r.cmd(0x01).d, (Bytes{0x82}));
}

TEST(CdCmd, PlayWithTrackParam) {
    Rig r;
    EXPECT_EQ(r.cmd(0x03, {0x01}).n, 3);
}

TEST(CdCmd, PlayDeliversNoSectors) {
    Rig r;
    r.cmd(0x03);
    auto ev = r.run_acking(r.ctx().cycles + 6 * kP1);
    EXPECT_EQ(r.count_int(ev, 1), 0);
    EXPECT_GT(r.cd.next_lba(), 0);
}

TEST(CdCmd, ForwardBackwardStubs) {
    Rig r;
    EXPECT_EQ(r.cmd(0x04).d, (Bytes{0x02}));
    EXPECT_EQ(r.cmd(0x05).d, (Bytes{0x02}));
}

TEST(CdCmd, MotorOnWhenSpinningIsError) {
    Rig r;
    Reply a = r.cmd(0x07);
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d, (Bytes{0x03, 0x20}));
}

TEST(CdCmd, MotorOnFromStopped) {
    Rig r;
    r.cd.set_disc(nullptr);
    r.cd.set_disc(&r.disc);
    r.cmd(0x08);
    r.wait();  // INT2 of Stop
    EXPECT_FALSE(r.cd.motor_on());
    Reply a = r.cmd(0x07);
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x00}));
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x02}));
    EXPECT_TRUE(r.cd.motor_on());
}

TEST(CdCmd, StopInt3ThenInt2MotorOff) {
    Rig r;
    Reply a = r.cmd(0x08);
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x02}));
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x00}));
}

TEST(CdCmd, PauseIdleResponses) {
    Rig r;
    Reply a = r.cmd(0x09);
    EXPECT_EQ(a.n, 3);
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x02}));
    EXPECT_EQ(b.t - a.t, Cdrom::kPauseIdle);
}

TEST(CdCmd, PauseWhileReadingShowsReadBitThenClears) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + 10000000);
    Reply a = r.cmd(0x09);
    EXPECT_EQ(a.d, (Bytes{0x22}));
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x02}));
    EXPECT_GE(b.t - a.t, Cdrom::kPause2x);
}

TEST(CdCmd, InitResponses) {
    Rig r;
    Reply a = r.cmd(0x0A);
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x02}));
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x02}));
    EXPECT_EQ(b.t - a.t, Cdrom::kInitDelay);
}

TEST(CdCmd, InitResetsModeAndStopsReading) {
    Rig r;
    r.set_mode(0xA0);
    r.start_read(0);
    r.cmd(0x0A);
    r.wait();
    EXPECT_EQ(r.cd.mode(), 0);
    EXPECT_EQ(r.cd.state(), Cdrom::State::Idle);
    auto ev = r.run_acking(r.ctx().cycles + 5 * kP1);
    EXPECT_EQ(r.count_int(ev, 1), 0);
}

TEST(CdCmd, MuteDemute) {
    Rig r;
    EXPECT_EQ(r.cmd(0x0B).d, (Bytes{0x02}));
    EXPECT_TRUE(r.cd.cdda_muted());
    EXPECT_EQ(r.cmd(0x0C).d, (Bytes{0x02}));
    EXPECT_FALSE(r.cd.cdda_muted());
}

TEST(CdCmd, SetfilterGetparam) {
    Rig r;
    EXPECT_EQ(r.cmd(0x0D, {0x03, 0x07}).n, 3);
    r.cmd(0x0E, {0x48});
    Reply g = r.cmd(0x0F);
    EXPECT_EQ(g.n, 3);
    EXPECT_EQ(g.d, (Bytes{0x02, 0x48, 0x00, 0x03, 0x07}));
}

TEST(CdCmd, SetmodeStores) {
    Rig r;
    EXPECT_EQ(r.cmd(0x0E, {0xC8}).n, 3);
    EXPECT_EQ(r.cd.mode(), 0xC8);
}

TEST(CdCmd, GetlocLWithoutReadingIsError) {
    Rig r;
    Reply a = r.cmd(0x10);
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d, (Bytes{0x03, 0x80}));
}

TEST(CdCmd, GetlocLReportsHeader) {
    Rig r;
    r.start_read(11);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 11 * 24 + 3 * kP1);
    int lba = static_cast<int>(r.cd.next_lba()) - 1;
    Reply g = r.cmd(0x10);
    ASSERT_EQ(g.n, 3);
    ASSERT_EQ(g.d.size(), 8u);
    Bytes m = Rig::msf(lba);
    EXPECT_EQ(g.d[0], m[0]);
    EXPECT_EQ(g.d[1], m[1]);
    EXPECT_EQ(g.d[2], m[2]);
    EXPECT_EQ(g.d[3], 2);  // mode
}

TEST(CdCmd, GetlocLSubheaderBytes) {
    Rig r;
    r.disc.s[0] = FakeDisc::make(0, 5, 6, 0x08);
    r.disc.s[0][19] = 0x42;
    r.disc.s[1] = r.disc.s[0];
    r.disc.s[2] = r.disc.s[0];
    r.start_read(0);
    r.run_acking(r.ctx().cycles + 1500000);  // sector 0 read, only sector 0..2 equal subheader
    ASSERT_LE(r.cd.next_lba(), 3);
    Reply g = r.cmd(0x10);
    ASSERT_EQ(g.d.size(), 8u);
    EXPECT_EQ(g.d[4], 5);
    EXPECT_EQ(g.d[5], 6);
    EXPECT_EQ(g.d[6], 0x08);
    EXPECT_EQ(g.d[7], 0x42);
}

TEST(CdCmd, GetlocPAfterSeek) {
    Rig r;
    r.setloc(100);
    Reply s = r.cmd(0x15);
    EXPECT_EQ(s.n, 3);
    EXPECT_EQ(r.wait().n, 2);
    Reply g = r.cmd(0x11);
    EXPECT_EQ(g.n, 3);
    // track 1 index 1; relative 00:01:25; absolute 00:03:25
    EXPECT_EQ(g.d, (Bytes{0x01, 0x01, 0x00, 0x01, 0x25, 0x00, 0x03, 0x25}));
}

TEST(CdCmd, GetlocPAtStart) {
    Rig r;
    Reply g = r.cmd(0x11);
    EXPECT_EQ(g.d, (Bytes{0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00}));
}

TEST(CdCmd, GetlocPFollowsReading) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + 600000 + 4 * kP1);
    int last = static_cast<int>(r.cd.next_lba()) - 1;
    Reply g = r.cmd(0x11);
    ASSERT_EQ(g.n, 3);
    EXPECT_EQ(g.d[4], bcd(static_cast<unsigned>(last % 75)));
}

TEST(CdCmd, SetSessionOne) {
    Rig r;
    EXPECT_EQ(r.cmd(0x12, {1}).n, 3);
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x02}));
}

TEST(CdCmd, SetSessionZeroIsError) {
    Rig r;
    Reply a = r.cmd(0x12, {0});
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d[1], 0x10);
}

TEST(CdCmd, SetSessionTwoFailsLater) {
    Rig r;
    EXPECT_EQ(r.cmd(0x12, {2}).n, 3);
    EXPECT_EQ(r.wait().n, 5);
}

TEST(CdCmd, GetTN) {
    Rig r;
    Reply a = r.cmd(0x13);
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x02, 0x01, 0x01}));
}

TEST(CdCmd, GetTDTrack1) {
    Rig r;
    EXPECT_EQ(r.cmd(0x14, {0x01}).d, (Bytes{0x02, 0x00, 0x02}));
}

TEST(CdCmd, GetTDLeadOut) {
    Rig r(75 * 60 * 2);  // 2 minutes of sectors
    EXPECT_EQ(r.cmd(0x14, {0x00}).d, (Bytes{0x02, 0x02, 0x02}));
}

TEST(CdCmd, GetTDLeadOutOddSize) {
    Rig r(75 * 61 + 10);
    EXPECT_EQ(r.cmd(0x14, {0x00}).d, (Bytes{0x02, 0x01, 0x03}));
}

TEST(CdCmd, GetTDBadTrack) {
    Rig r;
    Reply a = r.cmd(0x14, {0x02});
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d, (Bytes{0x03, 0x10}));
}

TEST(CdCmd, GetTDNonBcd) {
    Rig r;
    EXPECT_EQ(r.cmd(0x14, {0x0F}).n, 5);
}

TEST(CdCmd, SeekLSetsPosition) {
    Rig r;
    r.setloc(40);
    Reply a = r.cmd(0x15);
    EXPECT_EQ(a.d, (Bytes{0x02}));
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x02}));
    EXPECT_EQ(r.cd.next_lba(), 40);
    EXPECT_EQ(r.cd.state(), Cdrom::State::Idle);
}

TEST(CdCmd, SeekPSameAsSeekL) {
    Rig r;
    r.setloc(41);
    EXPECT_EQ(r.cmd(0x16).n, 3);
    EXPECT_EQ(r.wait().n, 2);
    EXPECT_EQ(r.cd.next_lba(), 41);
}

TEST(CdCmd, SeekShowsSeekBitUntilDone) {
    Rig r;
    r.setloc(200);
    r.cmd(0x15);
    EXPECT_EQ(r.cmd(0x01).d, (Bytes{0x42}));
    EXPECT_EQ(r.wait().n, 2);
    EXPECT_EQ(r.cmd(0x01).d, (Bytes{0x02}));
}

TEST(CdCmd, SeekDelayGrowsWithDistance) {
    Rig a, b;
    a.setloc(10);
    int64_t t0 = a.ctx().cycles;
    a.cmd(0x15);
    int64_t da = a.wait().t - t0;
    b.setloc(250);
    int64_t t1 = b.ctx().cycles;
    b.cmd(0x15);
    int64_t db = b.wait().t - t1;
    EXPECT_GT(db, da);
}

TEST(CdCmd, SeekBeyondDiscFails) {
    Rig r;
    r.setloc(5000);
    EXPECT_EQ(r.cmd(0x15).n, 3);
    Reply b = r.wait();
    EXPECT_EQ(b.n, 5);
    EXPECT_EQ(b.d[0] & 1, 1);
    EXPECT_EQ(b.d[1], 0x04);
}

TEST(CdCmd, SeekWithoutSetlocUsesCurrent) {
    Rig r;
    EXPECT_EQ(r.cmd(0x15).n, 3);
    EXPECT_EQ(r.wait().n, 2);
    EXPECT_EQ(r.cd.next_lba(), 0);
}

TEST(CdCmd, TestVersion) {
    Rig r;
    Reply a = r.cmd(0x19, {0x20});
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x94, 0x09, 0x19, 0xC0}));
}

TEST(CdCmd, TestSub04Stat) {
    Rig r;
    EXPECT_EQ(r.cmd(0x19, {0x04}).d, (Bytes{0x02}));
}

TEST(CdCmd, TestSub05Counters) {
    Rig r;
    EXPECT_EQ(r.cmd(0x19, {0x05}).d, (Bytes{0x00, 0x00}));
}

TEST(CdCmd, TestUnknownSub) {
    Rig r;
    Reply a = r.cmd(0x19, {0x77});
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d, (Bytes{0x03, 0x10}));
}

TEST(CdCmd, GetIdLicensedDisc) {
    Rig r;
    Reply a = r.cmd(0x1A);
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x02}));
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}));
    EXPECT_EQ(b.t - a.t, Cdrom::kGetIdDelay);
}

TEST(CdCmd, GetIdNoDisc) {
    Rig r;
    r.cd.set_disc(nullptr);
    EXPECT_EQ(r.cmd(0x1A).n, 3);
    Reply b = r.wait();
    EXPECT_EQ(b.n, 5);
    EXPECT_EQ(b.d, (Bytes{0x08, 0x40, 0, 0, 0, 0, 0, 0}));
}

TEST(CdCmd, ReadTocResponses) {
    Rig r;
    EXPECT_EQ(r.cmd(0x1E).n, 3);
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d, (Bytes{0x02}));
}

TEST(CdCmd, UnknownCommandInt5) {
    Rig r;
    Reply a = r.cmd(0x77);
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d, (Bytes{0x03, 0x40}));
}

TEST(CdCmd, UnknownCommandZero) {
    Rig r;
    EXPECT_EQ(r.cmd(0x00).d, (Bytes{0x03, 0x40}));
}

TEST(CdCmd, SecretUnlockCommandsUnsupported) {
    Rig r;
    EXPECT_EQ(r.cmd(0x50).n, 5);
}

TEST(CdCmd, UnknownCommandStillTimed) {
    Rig r;
    r.send(0xFF);
    EXPECT_EQ(r.wait().t, Cdrom::kFirstResponse);
}

TEST(CdCmd, ErrorStatCarriesMotorBit) {
    Rig r;
    r.cd.set_disc(nullptr);
    EXPECT_EQ(r.cmd(0x77).d, (Bytes{0x01, 0x40}));
}

struct CountCase {
    uint8_t cmd;
    size_t good;
};
class CdParamCount : public ::testing::TestWithParam<CountCase> {};
TEST_P(CdParamCount, WrongCountIsInt5Err20) {
    Rig r;
    CountCase c = GetParam();
    Reply a = r.cmd(c.cmd, Bytes(c.good + 1, 0x01));
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d, (Bytes{0x03, 0x20}));
}
INSTANTIATE_TEST_SUITE_P(Cmds, CdParamCount,
                         ::testing::Values(CountCase{0x01, 0}, CountCase{0x02, 3}, CountCase{0x06, 0},
                                           CountCase{0x08, 0}, CountCase{0x09, 0}, CountCase{0x0A, 0},
                                           CountCase{0x0B, 0}, CountCase{0x0C, 0}, CountCase{0x0D, 2},
                                           CountCase{0x0E, 1}, CountCase{0x0F, 0}, CountCase{0x10, 0},
                                           CountCase{0x11, 0}, CountCase{0x12, 1}, CountCase{0x13, 0},
                                           CountCase{0x14, 1}, CountCase{0x15, 0}, CountCase{0x16, 0},
                                           CountCase{0x19, 1}, CountCase{0x1A, 0}, CountCase{0x1B, 0},
                                           CountCase{0x1E, 0}));

class CdTooFew : public ::testing::TestWithParam<CountCase> {};
TEST_P(CdTooFew, MissingParamsIsInt5Err20) {
    Rig r;
    Reply a = r.cmd(GetParam().cmd, Bytes(GetParam().good - 1, 0x01));
    EXPECT_EQ(a.n, 5);
    EXPECT_EQ(a.d, (Bytes{0x03, 0x20}));
}
INSTANTIATE_TEST_SUITE_P(Cmds, CdTooFew,
                         ::testing::Values(CountCase{0x02, 3}, CountCase{0x0D, 2}, CountCase{0x0E, 1},
                                           CountCase{0x12, 1}, CountCase{0x14, 1}, CountCase{0x19, 1}));

TEST(CdCmd, PlayAllowsZeroOrOneParam) {
    Rig r;
    EXPECT_EQ(r.cmd(0x03, {1}).n, 3);
    EXPECT_EQ(r.cmd(0x03, {1, 2}).n, 5);
}

// ---- reading ----

TEST(CdRead, ReadNGivesInt3ThenInt1WithStat22) {
    Rig r;
    r.setloc(5);
    Reply a = r.cmd(0x06);
    EXPECT_EQ(a.n, 3);
    EXPECT_EQ(a.d, (Bytes{0x02}));
    Reply b = r.wait();
    EXPECT_EQ(b.n, 1);
    EXPECT_EQ(b.d, (Bytes{0x22}));
}

TEST(CdRead, ReadSBehavesLikeReadN) {
    Rig r;
    r.start_read(5, 0x1B);
    Reply b = r.wait();
    EXPECT_EQ(b.n, 1);
    EXPECT_EQ(r.cd.state(), Cdrom::State::Reading);
}

TEST(CdRead, FirstSectorTiming1x) {
    Rig r;
    r.setloc(20);
    int64_t t0 = r.ctx().cycles;
    r.send(0x06);
    Reply a = r.wait();
    Reply b = r.wait();
    EXPECT_EQ(a.t, t0 + Cdrom::kFirstResponse);
    EXPECT_EQ(b.t, t0 + Cdrom::kFirstResponse + Cdrom::kSeekBase + 20 * Cdrom::kSeekPerSector + kP1);
}

TEST(CdRead, ReadingBitDuringSeek) {
    Rig r;
    r.start_read(100);
    EXPECT_EQ(r.cd.stat(), 0x42);
    r.wait();
    EXPECT_EQ(r.cd.stat(), 0x22);
}

TEST(CdRead, CadenceSingleSpeed) {
    Rig r;
    r.start_read(0);
    auto ev = r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 12 * kP1);
    std::vector<Ev> d;
    for (auto& e : ev) if (e.n == 1) d.push_back(e);
    ASSERT_GE(d.size(), 8u);
    for (size_t i = 1; i < d.size(); ++i) EXPECT_EQ(d[i].t - d[i - 1].t, kP1);
}

TEST(CdRead, CadenceDoubleSpeed) {
    Rig r;
    r.set_mode(Cdrom::kModeDouble);
    r.start_read(0);
    auto ev = r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 24 * kP2);
    std::vector<Ev> d;
    for (auto& e : ev) if (e.n == 1) d.push_back(e);
    ASSERT_GE(d.size(), 16u);
    for (size_t i = 1; i < d.size(); ++i) EXPECT_EQ(d[i].t - d[i - 1].t, kP2);
}

TEST(CdRead, SeventyFiveSectorsPerSecond) {
    EXPECT_EQ(kP1 * 75, 33868800);
    EXPECT_EQ(kP2 * 150, 33868800);
}

TEST(CdRead, SectorsAreConsecutive) {
    Rig r;
    r.start_read(30);
    auto ev = r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 30 * 24 + 6 * kP1);
    int64_t expect = 31;
    int ints = 0;
    for (auto& e : ev) {
        if (e.n != 1) continue;
        EXPECT_EQ(e.next_lba, expect++);
        ++ints;
    }
    EXPECT_GE(ints, 5);
}

TEST(CdRead, DoubleSpeedTakesEffectMidRead) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 4 * kP1);
    r.set_mode(Cdrom::kModeDouble);
    auto ev = r.run_acking(r.ctx().cycles + 10 * kP1);
    std::vector<Ev> d;
    for (auto& e : ev) if (e.n == 1) d.push_back(e);
    ASSERT_GE(d.size(), 6u);
    EXPECT_EQ(d.back().t - d[d.size() - 2].t, kP2);
}

TEST(CdRead, Data800Mode) {
    Rig r;
    r.start_read(12);
    ASSERT_EQ(r.wait().n, 1);
    r.bfrd();
    EXPECT_TRUE(r.status() & Cdrom::kStsDataReq);
    Bytes b = r.read_fifo(0x800);
    for (int i = 0; i < 0x800; ++i) ASSERT_EQ(b[i], r.disc.s[12][24 + i]) << i;
    EXPECT_FALSE(r.status() & Cdrom::kStsDataReq);
}

TEST(CdRead, Data924Mode) {
    Rig r;
    r.set_mode(Cdrom::kModeWholeSector);
    r.start_read(12);
    ASSERT_EQ(r.wait().n, 1);
    r.bfrd();
    Bytes b = r.read_fifo(0x924);
    for (int i = 0; i < 0x924; ++i) ASSERT_EQ(b[i], r.disc.s[12][12 + i]) << i;
    EXPECT_EQ(b[3], 2);  // mode byte right after the 3-byte MSF
    EXPECT_FALSE(r.status() & Cdrom::kStsDataReq);
}

TEST(CdRead, DataFifoEmptyBeforeBfrd) {
    Rig r;
    r.start_read(0);
    r.wait();
    EXPECT_FALSE(r.status() & Cdrom::kStsDataReq);
    EXPECT_EQ(r.r8(2), 0u);
}

TEST(CdRead, BfrdClearReleasesFifo) {
    Rig r;
    r.start_read(0);
    r.wait();
    r.bfrd();
    r.r8(2);
    r.idx(0);
    r.w8(kReg + 3, 0x00);
    EXPECT_FALSE(r.status() & Cdrom::kStsDataReq);
    EXPECT_EQ(r.cd.data_fifo_remaining(), 0u);
}

TEST(CdRead, HalfwordReads) {
    Rig r;
    r.start_read(7);
    r.wait();
    r.bfrd();
    uint32_t h0 = r.bus.read(kReg + 2, 2);
    uint32_t h1 = r.bus.read(kReg + 2, 2);
    EXPECT_EQ(h0, static_cast<uint32_t>(r.disc.s[7][24] | (r.disc.s[7][25] << 8)));
    EXPECT_EQ(h1, static_cast<uint32_t>(r.disc.s[7][26] | (r.disc.s[7][27] << 8)));
}

TEST(CdRead, WordReadsAdvanceFour) {
    Rig r;
    r.start_read(7);
    r.wait();
    r.bfrd();
    uint32_t w = r.bus.read(kReg + 2, 4);
    EXPECT_EQ(w, static_cast<uint32_t>(r.disc.s[7][24] | (r.disc.s[7][25] << 8) | (r.disc.s[7][26] << 16) |
                                      (r.disc.s[7][27] << 24)));
    EXPECT_EQ(r.r8(2), r.disc.s[7][28]);
}

TEST(CdRead, ReadPastEndOfFifoIsZero) {
    Rig r;
    r.start_read(1);
    r.wait();
    r.bfrd();
    r.read_fifo(0x800);
    EXPECT_EQ(r.r8(2), 0u);
}

TEST(CdRead, BfrdClearThenSetReloadsFromStart) {
    Rig r;
    r.start_read(1);
    r.wait();
    r.bfrd();
    r.read_fifo(10);
    r.idx(0);
    r.w8(kReg + 3, 0x00);  // clearing BFRD empties the FIFO
    r.bfrd();
    EXPECT_EQ(r.r8(2), r.disc.s[1][24]);
}

TEST(CdRead, DataFifoHoldsSectorUntilNextBfrd) {
    Rig r;
    r.start_read(1);
    r.wait();
    r.bfrd();
    r.run_acking(r.ctx().cycles + 3 * kP1);  // later sectors arrive; fifo contents stay
    EXPECT_EQ(r.r8(2), r.disc.s[1][24]);
}

TEST(CdRead, BfrdLoadsLatestDeliveredSector) {
    Rig r;
    r.start_read(1);
    r.wait();
    auto ev = r.run_acking(r.ctx().cycles + 2 * kP1 + 10);
    int last = static_cast<int>(r.cd.next_lba()) - 1;
    r.bfrd();
    EXPECT_EQ(r.r8(2), r.disc.s[last][24]);
}

TEST(CdRead, DmaChannel3Data800) {
    Rig r;
    r.start_read(9);
    ASSERT_EQ(r.wait().n, 1);
    r.bfrd();
    r.w32(0x1F8010F0, 0x0FFFFFFF);
    r.w32(0x1F8010B0, 0x1000);       // MADR3
    r.w32(0x1F8010B4, 0x200);        // BCR: 0x200 words
    r.w32(0x1F8010B8, 0x11000000);   // CHCR: start + trigger, to RAM
    for (int i = 0; i < 0x800; ++i) ASSERT_EQ(r.bus.ram()[0x1000 + i], r.disc.s[9][24 + i]) << i;
    EXPECT_EQ(r.bus.ram()[0x1800], 0);
    EXPECT_EQ(r.cd.data_fifo_remaining(), 0u);
}

TEST(CdRead, DmaChannel3Data924) {
    Rig r;
    r.set_mode(Cdrom::kModeWholeSector);
    r.start_read(9);
    ASSERT_EQ(r.wait().n, 1);
    r.bfrd();
    r.w32(0x1F8010F0, 0x0FFFFFFF);
    r.w32(0x1F8010B0, 0x2000);
    r.w32(0x1F8010B4, 0x249);
    r.w32(0x1F8010B8, 0x11000000);
    for (int i = 0; i < 0x924; ++i) ASSERT_EQ(r.bus.ram()[0x2000 + i], r.disc.s[9][12 + i]) << i;
}

TEST(CdRead, DmaPartialLeavesRemainder) {
    Rig r;
    r.start_read(9);
    r.wait();
    r.bfrd();
    r.w32(0x1F8010F0, 0x0FFFFFFF);
    r.w32(0x1F8010B0, 0x3000);
    r.w32(0x1F8010B4, 0x20);
    r.w32(0x1F8010B8, 0x11000000);
    EXPECT_EQ(r.cd.data_fifo_remaining(), 0x800u - 0x80u);
    EXPECT_EQ(r.r8(2), r.disc.s[9][24 + 0x80]);
}

TEST(CdRead, DmaDeviceDirect) {
    Rig r;
    r.start_read(2);
    r.wait();
    r.bfrd();
    std::array<uint32_t, 4> w{};
    r.cd.dma_read(w);
    EXPECT_EQ(w[0] & 0xFF, r.disc.s[2][24]);
    EXPECT_EQ(w[3] >> 24, r.disc.s[2][39]);
    EXPECT_EQ(r.cd.dma_cycles_per_word(), 24u);
}

TEST(CdRead, DmaBeforeBfrdReadsZeros) {
    Rig r;
    std::array<uint32_t, 2> w{5, 5};
    r.cd.dma_read(w);
    EXPECT_EQ(w[0], 0u);
    EXPECT_EQ(w[1], 0u);
}

TEST(CdRead, PauseStopsDelivery) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 3 * kP1);
    r.cmd(0x09);
    r.wait();
    int64_t lba = r.cd.next_lba();
    auto ev = r.run_acking(r.ctx().cycles + 6 * kP1);
    EXPECT_EQ(r.count_int(ev, 1), 0);
    EXPECT_EQ(r.cd.next_lba(), lba);
    EXPECT_EQ(r.cd.state(), Cdrom::State::Idle);
}

TEST(CdRead, StopStopsDelivery) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 2 * kP1);
    r.cmd(0x08);
    r.wait();
    auto ev = r.run_acking(r.ctx().cycles + 6 * kP1);
    EXPECT_EQ(r.count_int(ev, 1), 0);
    EXPECT_FALSE(r.cd.motor_on());
}

TEST(CdRead, ResumeAfterPauseContinuesFromNext) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 3 * kP1);
    r.cmd(0x09);
    r.wait();
    int64_t lba = r.cd.next_lba();
    ASSERT_EQ(r.cmd(0x06).n, 3);
    ASSERT_EQ(r.wait().n, 1);
    EXPECT_EQ(r.cd.next_lba(), lba + 1);
}

TEST(CdRead, SetlocMidReadDoesNotMoveUntilNextRead) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 3 * kP1);
    int64_t lba = r.cd.next_lba();
    r.setloc(200);
    auto ev = r.run_acking(r.ctx().cycles + 2 * kP1);
    ASSERT_FALSE(ev.empty());
    EXPECT_LT(ev.back().next_lba, 200);
    EXPECT_GE(ev.back().next_lba, lba);
}

TEST(CdRead, SetlocThenReadNRestartsAtNewPosition) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 3 * kP1);
    r.start_read(150);
    auto ev = r.run_acking(r.ctx().cycles + 600000 + 40 * Cdrom::kSeekPerSector * 5 + 6 * kP1 + 200 * 24);
    ASSERT_FALSE(ev.empty());
    bool saw = false;
    for (auto& e : ev) saw |= (e.n == 1 && e.next_lba >= 151);
    EXPECT_TRUE(saw);
    EXPECT_GE(r.cd.next_lba(), 151);
}

TEST(CdRead, SeekFromStoppedAddsSpinUp) {
    Rig r;
    r.cmd(0x08);
    r.wait();
    r.setloc(0);
    int64_t t0 = r.ctx().cycles;
    r.send(0x06);
    r.wait();
    Reply b = r.wait();
    EXPECT_EQ(b.n, 1);
    EXPECT_EQ(b.t, t0 + Cdrom::kFirstResponse + Cdrom::kSeekBase + Cdrom::kSpinUp + kP1);
    EXPECT_TRUE(r.cd.motor_on());
}

TEST(CdRead, EndOfDiscRaisesInt4) {
    Rig r(10);
    r.start_read(8);
    auto ev = r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 10 * kP1);
    EXPECT_EQ(r.count_int(ev, 1), 2);
    EXPECT_EQ(r.count_int(ev, 4), 1);
    EXPECT_EQ(r.cd.state(), Cdrom::State::Idle);
}

TEST(CdRead, ReadErrorRaisesInt5) {
    Rig r;
    r.start_read(3);
    r.disc.fail = true;
    Reply b = r.wait();
    EXPECT_EQ(b.n, 5);
    EXPECT_EQ(r.cd.state(), Cdrom::State::Idle);
}

TEST(CdRead, NoDiscReadFails) {
    Rig r;
    r.cd.set_disc(nullptr);
    EXPECT_EQ(r.cmd(0x06).n, 3);
    Reply b = r.wait();
    EXPECT_EQ(b.n, 5);
    EXPECT_EQ(b.d[1], 0x80);
}

TEST(CdRead, SetDiscStopsReading) {
    Rig r;
    r.start_read(0);
    r.cd.set_disc(&r.disc);
    EXPECT_EQ(r.cd.state(), Cdrom::State::Idle);
    auto ev = r.run_acking(r.ctx().cycles + 4 * kP1);
    EXPECT_EQ(r.count_int(ev, 1), 0);
}

TEST(CdRead, ReadNWhileReadingKeepsStreaming) {
    Rig r;
    r.start_read(0);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 2 * kP1);
    int64_t before = r.cd.next_lba();
    ASSERT_EQ(r.cmd(0x06).n, 3);
    auto ev = r.run_acking(r.ctx().cycles + 3 * kP1);
    ASSERT_FALSE(ev.empty());
    EXPECT_GE(r.cd.next_lba(), before);
}

// ---- overrun / interrupt queueing ----

TEST(CdIrq, UnackedInt1DropsLaterSectors) {
    Rig r;
    r.start_read(0);
    // Do not ack anything.
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    EXPECT_EQ(r.cd.irq_flag(), 1);
    r.run_to(r.ctx().cycles + 4 * kP1);
    EXPECT_EQ(r.cd.irq_flag(), 1);
    EXPECT_EQ(r.cd.overruns(), 4u);
    EXPECT_EQ(r.cd.next_lba(), 5);
}

TEST(CdIrq, ReadingResumesDeliveryAfterAck) {
    Rig r;
    r.start_read(0);
    r.wait(1000000000);                       // first INT1 (acked)
    r.run_to(r.ctx().cycles + 2 * kP1 + 5);   // two sectors with no ack of the next
    uint64_t ov = r.cd.overruns();
    r.ack();
    auto ev = r.run_acking(r.ctx().cycles + 3 * kP1);
    EXPECT_GE(r.count_int(ev, 1), 1);
    EXPECT_GE(ov, 0u);
}

TEST(CdIrq, IrqLineRaisedOnEnabledFlag) {
    Rig r;
    r.idx(1);
    r.w8(kReg + 2, 0x1F);
    r.send(0x01);
    EXPECT_FALSE(r.irq.stat() & 4);
    r.wait(1000000);
    EXPECT_TRUE(r.irq.stat() & 4);
}

TEST(CdIrq, NoIrqWhenDisabled) {
    Rig r;
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    EXPECT_FALSE(r.irq.stat() & 4);
}

TEST(CdIrq, EnablingLateRaisesIrq) {
    Rig r;
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.idx(1);
    r.w8(kReg + 2, 0x04);  // enable bit 2 only: INT3 is value 3 -> bits 0,1 -> mask 0x04 doesn't match
    EXPECT_FALSE(r.irq.stat() & 4);
    r.w8(kReg + 2, 0x03);
    EXPECT_TRUE(r.irq.stat() & 4);
}

TEST(CdIrq, EnableMaskBitsGateByIntNumber) {
    // INT3 = flag value 3: any overlapping enable bit raises.
    Rig r;
    r.idx(1);
    r.w8(kReg + 2, 0x01);
    r.send(0x01);
    r.wait(1000000);
    EXPECT_TRUE(r.irq.stat() & 4);
}

TEST(CdIrq, AckClearsFlag) {
    Rig r;
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.ack(0x07);
    EXPECT_EQ(r.cd.irq_flag(), 0);
}

TEST(CdIrq, PartialAckKeepsOtherBits) {
    Rig r;
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.ack(0x01);
    EXPECT_EQ(r.cd.irq_flag(), 2);
}

TEST(CdIrq, AckHighBitsIgnored) {
    Rig r;
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.ack(0xE0);
    EXPECT_EQ(r.cd.irq_flag(), 3);
}

TEST(CdIrq, SecondCommandQueuedBehindUnackedInterrupt) {
    Rig r;
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.send(0x13);  // GetTN while INT3 of Getstat is unacked
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_EQ(r.cd.irq_flag(), 3);
    EXPECT_EQ(r.cd.queued_responses(), 1u);
    EXPECT_EQ(r.drain_resp(), (Bytes{0x02}));  // still the first command's response
    r.ack();
    EXPECT_EQ(r.cd.irq_flag(), 0);
    Reply b = r.wait();
    EXPECT_EQ(b.n, 3);
    EXPECT_EQ(b.d, (Bytes{0x02, 0x01, 0x01}));
}

TEST(CdIrq, QueuedResponseDelayedByAckGap) {
    Rig r;
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.send(0x01);
    r.run_to(r.ctx().cycles + 50000);
    int64_t t = r.ctx().cycles;
    r.ack();
    Reply b = r.wait();
    EXPECT_EQ(b.t, t + Cdrom::kAckGap);
}

TEST(CdIrq, SecondResponseWaitsForAck) {
    Rig r;
    r.send(0x1A);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.run_to(r.ctx().cycles + 3 * Cdrom::kGetIdDelay);  // INT2 due but INT3 not acknowledged
    EXPECT_EQ(r.cd.irq_flag(), 3);
    EXPECT_EQ(r.cd.queued_responses(), 1u);
    r.ack();
    Reply b = r.wait();
    EXPECT_EQ(b.n, 2);
    EXPECT_EQ(b.d.size(), 8u);
}

TEST(CdIrq, ResponsesKeepOrder) {
    Rig r;
    r.send(0x1A);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    r.run_to(r.ctx().cycles + 3 * Cdrom::kGetIdDelay);  // INT2 queued behind the unacked INT3
    r.send(0x01);
    r.run_to(r.ctx().cycles + 200000);
    EXPECT_EQ(r.cd.queued_responses(), 2u);
    r.ack();
    EXPECT_EQ(r.wait().n, 2);  // GetID second response was queued first
    EXPECT_EQ(r.wait().n, 3);
}

TEST(CdIrq, IrqRaisedOncePerDelivery) {
    Rig r;
    r.idx(1);
    r.w8(kReg + 2, 0x1F);
    r.w32(0x1F801070, 0);  // clear I_STAT
    r.send(0x01);
    while (r.cd.irq_flag() == 0) {
        r.ctx().cycles = r.ctx().next_event;
        r.sched.run_due();
    }
    EXPECT_TRUE(r.irq.stat() & 4);
    r.w32(0x1F801070, 0);  // CPU acks the I_STAT bit but not the CD flag
    r.run_to(r.ctx().cycles + 100000);
    EXPECT_FALSE(r.irq.stat() & 4);  // line stays high, no new edge
}

TEST(CdIrq, IrqRaisedAgainAfterAckAndNewResponse) {
    Rig r;
    r.idx(1);
    r.w8(kReg + 2, 0x1F);
    r.cmd(0x01);
    r.w32(0x1F801070, 0);
    r.cmd(0x01);
    EXPECT_TRUE(r.irq.stat() & 4);
}

TEST(CdIrq, Int1RaisesIrq) {
    Rig r;
    r.idx(1);
    r.w8(kReg + 2, 0x1F);
    r.start_read(0);
    r.w32(0x1F801070, 0);
    r.wait();
    EXPECT_TRUE(r.irq.stat() & 4);
}

// ---- XA-ADPCM routing ----

struct XaRig : Rig {
    XaRig() {
        for (uint32_t l = 20; l < 30; ++l) disc.s[l] = FakeDisc::make(l, 1, (l % 2) ? 3 : 4, kRtAudio);
    }
    int run_read(int from, int sectors) {
        start_read(from);
        auto ev = run_acking(ctx().cycles + Cdrom::kSeekBase + 300 * 24 + (sectors + 2) * kP1);
        return count_int(ev, 1);
    }
};

TEST(CdXa, EnabledRoutesAudioToSinkNotData) {
    XaRig r;
    r.set_mode(Cdrom::kModeXa);
    int ints = r.run_read(20, 8);
    EXPECT_EQ(ints, 0);
    EXPECT_GE(r.sink.got.size(), 6u);
    EXPECT_EQ(Sink::lba_of(r.sink.got[0]), 20);
    EXPECT_EQ(Sink::lba_of(r.sink.got[1]), 21);
}

TEST(CdXa, SinkGetsRawSectorBytes) {
    XaRig r;
    r.set_mode(Cdrom::kModeXa);
    r.run_read(20, 3);
    ASSERT_FALSE(r.sink.got.empty());
    EXPECT_EQ(r.sink.got[0], r.disc.s[20]);
}

TEST(CdXa, DisabledDeliversAudioAsData) {
    XaRig r;
    int ints = r.run_read(20, 5);
    EXPECT_GE(ints, 4);
    EXPECT_TRUE(r.sink.got.empty());
}

TEST(CdXa, DataSectorsStillDeliveredWithXaOn) {
    XaRig r;
    r.set_mode(Cdrom::kModeXa);
    int ints = r.run_read(15, 12);
    EXPECT_EQ(ints, 5);  // sectors 15-19 are data
    EXPECT_FALSE(r.sink.got.empty());
}

TEST(CdXa, FilterMatchingChannelPasses) {
    XaRig r;
    r.cmd(0x0D, {1, 4});
    r.set_mode(Cdrom::kModeXa | Cdrom::kModeFilter);
    r.run_read(20, 8);
    ASSERT_GE(r.sink.got.size(), 3u);
    for (auto& s : r.sink.got) EXPECT_EQ(s[17], 4);
}

TEST(CdXa, FilterMismatchDropsEverything) {
    XaRig r;
    r.cmd(0x0D, {1, 9});
    r.set_mode(Cdrom::kModeXa | Cdrom::kModeFilter);
    int ints = r.run_read(20, 8);
    EXPECT_EQ(ints, 0);
    EXPECT_TRUE(r.sink.got.empty());
}

TEST(CdXa, FilterWrongFileDrops) {
    XaRig r;
    r.cmd(0x0D, {2, 3});
    r.set_mode(Cdrom::kModeXa | Cdrom::kModeFilter);
    r.run_read(20, 8);
    EXPECT_TRUE(r.sink.got.empty());
}

TEST(CdXa, FilterOffPassesAllChannels) {
    XaRig r;
    r.cmd(0x0D, {1, 9});
    r.set_mode(Cdrom::kModeXa);
    r.run_read(20, 8);
    bool c3 = false, c4 = false;
    for (auto& s : r.sink.got) {
        c3 |= s[17] == 3;
        c4 |= s[17] == 4;
    }
    EXPECT_TRUE(c3 && c4);
}

TEST(CdXa, NonRealTimeAudioIsData) {
    XaRig r;
    for (uint32_t l = 20; l < 30; ++l) r.disc.s[l] = FakeDisc::make(l, 1, 3, 0x24);
    r.set_mode(Cdrom::kModeXa);
    int ints = r.run_read(20, 5);
    EXPECT_GE(ints, 4);
    EXPECT_TRUE(r.sink.got.empty());
}

TEST(CdXa, RealTimeWithoutAudioIsData) {
    XaRig r;
    for (uint32_t l = 20; l < 30; ++l) r.disc.s[l] = FakeDisc::make(l, 1, 3, 0x60);
    r.set_mode(Cdrom::kModeXa);
    r.run_read(20, 5);
    EXPECT_TRUE(r.sink.got.empty());
}

TEST(CdXa, NoSinkStillSwallowsSector) {
    XaRig r;
    r.cd.set_xa_sink(nullptr);
    r.set_mode(Cdrom::kModeXa);
    EXPECT_EQ(r.run_read(20, 6), 0);
}

TEST(CdXa, FilterAppliedOnlyWhenModeBitSet) {
    XaRig r;
    r.cmd(0x0D, {7, 7});
    r.set_mode(Cdrom::kModeXa);
    r.run_read(20, 4);
    EXPECT_FALSE(r.sink.got.empty());
}

TEST(CdXa, XaSectorsAdvanceLbaAtSectorRate) {
    XaRig r;
    r.set_mode(Cdrom::kModeXa);
    r.start_read(20);
    r.run_acking(r.ctx().cycles + Cdrom::kSeekBase + 20 * 24 + 5 * kP1);
    EXPECT_GE(r.cd.next_lba(), 24);
}

TEST(CdXa, XaSectorsDoNotAffectDataFifo) {
    XaRig r;
    r.set_mode(Cdrom::kModeXa);
    r.run_read(20, 3);
    r.bfrd();
    EXPECT_FALSE(r.status() & Cdrom::kStsDataReq);
}

TEST(CdXa, XaOnAt2xStillRoutes) {
    XaRig r;
    r.set_mode(Cdrom::kModeXa | Cdrom::kModeDouble);
    r.run_read(20, 8);
    EXPECT_GE(r.sink.got.size(), 8u);
}

// ---- misc ----

TEST(CdMisc, ModeBitsConstantsMatchSpec) {
    EXPECT_EQ(Cdrom::kModeDouble, 0x80);
    EXPECT_EQ(Cdrom::kModeXa, 0x40);
    EXPECT_EQ(Cdrom::kModeWholeSector, 0x20);
    EXPECT_EQ(Cdrom::kModeIgnore, 0x10);
    EXPECT_EQ(Cdrom::kModeFilter, 0x08);
    EXPECT_EQ(Cdrom::kModeReport, 0x04);
    EXPECT_EQ(Cdrom::kModeAutoPause, 0x02);
    EXPECT_EQ(Cdrom::kModeCdda, 0x01);
}

TEST(CdMisc, IgnoreBitDoesNotChangeSectorSize) {
    Rig r;
    r.set_mode(Cdrom::kModeIgnore);
    r.start_read(3);
    r.wait();
    r.bfrd();
    EXPECT_EQ(r.cd.data_fifo_remaining(), 0x800u);
}

TEST(CdMisc, WholeSectorSizeFollowsModeAtBfrd) {
    Rig r;
    r.set_mode(Cdrom::kModeWholeSector);
    r.start_read(3);
    r.wait();
    r.bfrd();
    EXPECT_EQ(r.cd.data_fifo_remaining(), 0x924u);
}

TEST(CdMisc, DmaAttachedToChannel3) {
    Rig r;
    r.start_read(0);
    r.wait();
    r.bfrd();
    r.w32(0x1F8010F0, 0x0FFFFFFF);
    r.w32(0x1F8010B0, 0x4000);
    r.w32(0x1F8010B4, 4);
    r.w32(0x1F8010B8, 0x11000000);
    EXPECT_EQ(r.bus.ram()[0x4000], r.disc.s[0][24]);
}

TEST(CdMisc, SetDiscNullStopsMotor) {
    Rig r;
    r.cd.set_disc(nullptr);
    EXPECT_FALSE(r.cd.motor_on());
}

TEST(CdMisc, StatReadingAndSeekingBitsExclusive) {
    Rig r;
    r.start_read(0);
    uint8_t s1 = r.cd.stat();
    r.wait();
    uint8_t s2 = r.cd.stat();
    EXPECT_TRUE(s1 & Cdrom::kStatSeek);
    EXPECT_FALSE(s1 & Cdrom::kStatRead);
    EXPECT_TRUE(s2 & Cdrom::kStatRead);
    EXPECT_FALSE(s2 & Cdrom::kStatSeek);
}

TEST(CdMisc, RegistersReachableThroughBus) {
    Rig r;
    EXPECT_EQ(r.bus.read(0x1F801800, 1) & 0x18, 0x18u);
}

// libcd reads the 12-byte header, re-asserts BFRD, then reads the data: the FIFO must not rewind.
TEST(CdRead, BfrdReassertDoesNotRewindFifo) {
    Rig r;
    r.set_mode(Cdrom::kModeWholeSector);
    r.start_read(12);
    ASSERT_EQ(r.wait().n, 1);
    r.bfrd();
    Bytes head = r.read_fifo(12);
    r.bfrd();
    Bytes data = r.read_fifo(0x800);
    for (int i = 0; i < 12; ++i) ASSERT_EQ(head[i], r.disc.s[12][12 + i]) << i;
    for (int i = 0; i < 0x800; ++i) ASSERT_EQ(data[i], r.disc.s[12][24 + i]) << i;
}

TEST(CdRead, BfrdOnEmptyFifoLoadsCurrentSector) {
    Rig r;
    r.start_read(12);
    ASSERT_EQ(r.wait().n, 1);
    r.bfrd();
    r.read_fifo(0x800);
    ASSERT_EQ(r.wait().n, 1);
    r.bfrd();
    Bytes next = r.read_fifo(4);
    for (int i = 0; i < 4; ++i) ASSERT_EQ(next[i], r.disc.s[13][24 + i]) << i;
}
