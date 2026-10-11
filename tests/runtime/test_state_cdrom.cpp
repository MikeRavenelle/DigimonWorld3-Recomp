// Save-state round trips of the CD-ROM controller and the XA decoder: a state saved in the middle of a command
// or a sector stream and loaded into a fresh instance must behave exactly like the original afterwards.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "dw3/cdrom.h"
#include "dw3/state.h"
#include "dw3/xa.h"
#include "hw_test_util.h"

using namespace dw3;
using Bytes = std::vector<uint8_t>;

namespace {
constexpr uint32_t kReg = 0x1F801800u;
constexpr uint8_t kRtAudio = 0x64;
constexpr uint8_t kData = 0x08;

uint8_t bcd(unsigned v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

struct FakeDisc : SectorSource {
    std::vector<std::array<uint8_t, 2352>> s;
    explicit FakeDisc(uint32_t n = 300) {
        for (uint32_t l = 0; l < n; ++l) {
            // A band of real-time audio sectors in the middle so the XA path is exercised.
            uint8_t sm = (l >= 40 && l < 60 && (l & 1)) ? kRtAudio : kData;
            s.push_back(make(l, 1, 0, sm));
        }
    }
    static std::array<uint8_t, 2352> make(uint32_t lba, uint8_t file, uint8_t chan, uint8_t sm) {
        std::array<uint8_t, 2352> r{};
        for (int i = 1; i < 11; ++i) r[i] = 0xFF;
        uint32_t a = lba + 150;
        r[12] = bcd(a / 75 / 60);
        r[13] = bcd(a / 75 % 60);
        r[14] = bcd(a % 75);
        r[15] = 2;
        r[16] = r[20] = file;
        r[17] = r[21] = chan;
        r[18] = r[22] = sm;
        for (int i = 24; i < 2352; ++i) r[i] = static_cast<uint8_t>(lba * 7 + i * 3 + 1);
        return r;
    }
    uint32_t sector_count() const override { return static_cast<uint32_t>(s.size()); }
    bool read_raw(uint32_t lba, uint8_t out[2352]) override {
        if (lba >= s.size()) return false;
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
};

struct Rig : dw3test::Clock {
    IrqController irq{bus};
    Dma dma{bus, sched, irq};
    FakeDisc disc;
    Sink sink;
    Cdrom cd{bus, sched, irq, dma};

    Rig() {
        cd.set_disc(&disc);
        cd.set_xa_sink(&sink);
    }
    uint32_t r8(uint32_t off) { return bus.read(kReg + off, 1); }
    void idx(int i) { w8(kReg, i); }
    void send(uint8_t cmd, Bytes params = {}) {
        idx(0);
        for (auto p : params) w8(kReg + 2, p);
        w8(kReg + 1, cmd);
    }
    Bytes drain_resp() {
        Bytes b;
        while (r8(0) & Cdrom::kStsRespReady) b.push_back(static_cast<uint8_t>(r8(1)));
        return b;
    }
    void ack(uint8_t v = 0x1F) {
        idx(1);
        w8(kReg + 3, v);
    }
    void bfrd() {
        idx(0);
        w8(kReg + 3, 0x80);
    }
    // Runs until an interrupt is pending, then reads its response and acknowledges it.
    int wait() {
        run_to_irq();
        int n = cd.irq_flag();
        drain_resp();
        ack();
        return n;
    }
    int cmd(uint8_t c, Bytes p = {}) {
        send(c, p);
        return wait();
    }
    static Bytes msf(int lba) {
        unsigned a = static_cast<unsigned>(lba + 150);
        return {bcd(a / 75 / 60), bcd(a / 75 % 60), bcd(a % 75)};
    }
    // Runs until the first interrupt and leaves it unacknowledged.
    void run_to_irq() {
        while (cd.irq_flag() == 0) {
            ctx().cycles = std::max(ctx().cycles, ctx().next_event);
            sched.run_due();
        }
    }
};

Bytes save(Rig& r) {
    StateIo w;
    r.sched.io(w);
    r.cd.io(w);
    return w.take();
}

void load(Rig& r, const Bytes& data, int64_t cycles) {
    StateIo rd{std::span<const uint8_t>(data)};
    r.sched.io(rd);
    r.cd.io(rd);
    EXPECT_TRUE(rd.at_end());
    r.ctx().cycles = cycles;
}

// Everything observable from the host side while running forward to `until`: acknowledges every second
// interrupt (so overruns happen), pulls the data FIFO after INT1 and records register values.
std::vector<int64_t> trace(Rig& r, int64_t until) {
    std::vector<int64_t> t;
    int n = 0;
    while (r.ctx().next_event <= until) {
        r.ctx().cycles = std::max(r.ctx().cycles, r.ctx().next_event);
        r.sched.run_due();
        if (!r.cd.irq_flag()) continue;
        t.push_back(r.ctx().cycles);
        t.push_back(r.cd.irq_flag());
        t.push_back(r.cd.next_lba());
        t.push_back(r.cd.stat());
        t.push_back(static_cast<int64_t>(r.cd.overruns()));
        r.idx(0);
        t.push_back(r.r8(0));
        for (uint8_t b : r.drain_resp()) t.push_back(b);
        if (r.cd.irq_flag() == 1) {
            r.bfrd();
            t.push_back(static_cast<int64_t>(r.cd.data_fifo_remaining()));
            for (int i = 0; i < 16; ++i) t.push_back(r.r8(2));
        }
        if (++n % 2 == 0) r.ack();
        else r.ack(static_cast<uint8_t>(r.cd.irq_flag()));
        t.push_back(static_cast<int64_t>(r.cd.queued_responses()));
    }
    r.ctx().cycles = std::max(r.ctx().cycles, until);
    return t;
}

// Saves `a`, loads into a fresh rig, checks the re-saved bytes and the future behaviour of both.
void check_clone(Rig& a, int64_t horizon) {
    Bytes first = save(a);
    Rig b;
    load(b, first, a.ctx().cycles);
    ASSERT_EQ(save(b), first);
    EXPECT_EQ(b.cd.irq_flag(), a.cd.irq_flag());
    EXPECT_EQ(b.cd.state(), a.cd.state());
    int64_t until = a.ctx().cycles + horizon;
    auto ta = trace(a, until);
    auto tb = trace(b, until);
    EXPECT_FALSE(ta.empty());
    EXPECT_EQ(ta, tb);
    EXPECT_EQ(a.sink.got, b.sink.got);
    EXPECT_EQ(save(a), save(b));
}
}  // namespace

TEST(StateCdrom, UnacknowledgedInterruptWithInitPending) {
    Rig a;
    a.send(0x0A);
    a.run_to_irq();  // INT3 raised, response unread, Init completion still scheduled
    ASSERT_EQ(a.cd.irq_flag(), 3);
    check_clone(a, 2000000);
}

TEST(StateCdrom, ReadingWithSectorsStreaming) {
    Rig a;
    ASSERT_EQ(a.cmd(0x0A), 3);
    ASSERT_EQ(a.wait(), 2);
    ASSERT_EQ(a.cmd(0x0E, {0x40}), 3);  // XA enabled
    ASSERT_EQ(a.cmd(0x02, Rig::msf(36)), 3);
    ASSERT_EQ(a.cmd(0x06), 3);
    // Let a handful of sectors go by with INT1 left unacknowledged for some of them.
    for (int i = 0; i < 6; ++i) {
        a.run_to_irq();
        a.drain_resp();
        a.bfrd();
        for (int k = 0; k < 40; ++k) a.r8(2);
        if (i % 2) a.ack();
    }
    ASSERT_GT(a.cd.data_fifo_remaining(), 0u);
    check_clone(a, 12 * Cdrom::kCyclesPerSector1x);
    EXPECT_FALSE(a.sink.got.empty());
}

TEST(StateCdrom, DoubleSpeedFilteredXa) {
    Rig a;
    ASSERT_EQ(a.cmd(0x0A), 3);
    ASSERT_EQ(a.wait(), 2);
    ASSERT_EQ(a.cmd(0x0D, {1, 0}), 3);
    ASSERT_EQ(a.cmd(0x0E, {0x40 | 0x08 | 0x80}), 3);
    ASSERT_EQ(a.cmd(0x02, Rig::msf(38)), 3);
    ASSERT_EQ(a.cmd(0x06), 3);
    a.run_to_irq();
    check_clone(a, 15 * Cdrom::kCyclesPerSector2x);
}

TEST(StateCdrom, SecondResponseInFlightAndQueued) {
    Rig a;
    ASSERT_EQ(a.cmd(0x0A), 3);
    ASSERT_EQ(a.wait(), 2);
    ASSERT_EQ(a.cmd(0x02, Rig::msf(5)), 3);
    ASSERT_EQ(a.cmd(0x06), 3);
    a.run_to_irq();  // INT1 pending
    a.send(0x09);    // Pause while INT1 is unacknowledged: INT3 queues behind it
    a.ctx().cycles += Cdrom::kFirstResponse;
    a.sched.run_due();
    EXPECT_GE(a.cd.queued_responses(), 1u);
    check_clone(a, 3 * Cdrom::kPause1x);
}

TEST(StateCdrom, SeekAndMotorCommands) {
    Rig a;
    ASSERT_EQ(a.cmd(0x0A), 3);
    ASSERT_EQ(a.wait(), 2);
    ASSERT_EQ(a.cmd(0x02, Rig::msf(120)), 3);
    a.send(0x15);  // SeekL
    a.run_to_irq();
    a.ack();
    ASSERT_EQ(a.cd.state(), Cdrom::State::Seeking);
    check_clone(a, 2 * Cdrom::kSeekBase + 10000000);
    EXPECT_EQ(a.cd.next_lba(), 120);
}

TEST(StateCdrom, StopAndGetId) {
    Rig a;
    a.send(0x08);
    a.run_to_irq();
    check_clone(a, Cdrom::kStopDelay + 100000);
    Rig b;
    b.send(0x1A);
    b.run_to_irq();
    check_clone(b, 100000);
}

TEST(StateCdrom, RegistersAndVolumeRestored) {
    Rig a;
    a.idx(2);
    a.w8(kReg + 2, 0x40);
    a.idx(3);
    a.w8(kReg + 1, 0x11);
    a.w8(kReg + 3, 0x01 | 0x20);
    a.idx(1);
    a.w8(kReg + 2, 0x1F);
    a.send(0x0B);  // Mute
    a.wait();
    Bytes st = save(a);
    Rig b;
    load(b, st, a.ctx().cycles);
    EXPECT_EQ(b.cd.volume().ll, a.cd.volume().ll);
    EXPECT_EQ(b.cd.volume().rl, a.cd.volume().rl);
    EXPECT_TRUE(b.cd.adpcm_muted());
    EXPECT_TRUE(b.cd.cdda_muted());
    EXPECT_EQ(b.cd.irq_enable(), 0x1F);
}

TEST(StateCdrom, TruncatedDataThrows) {
    Rig a;
    ASSERT_EQ(a.cmd(0x0A), 3);
    a.send(0x01);
    a.run_to_irq();
    Bytes st = save(a);
    for (std::size_t cut : {std::size_t{0}, std::size_t{10}, st.size() / 2, st.size() - 1}) {
        Rig b;
        Bytes part(st.begin(), st.begin() + static_cast<std::ptrdiff_t>(cut));
        StateIo rd{std::span<const uint8_t>(part)};
        EXPECT_THROW(
            {
                b.sched.io(rd);
                b.cd.io(rd);
            },
            StateError)
            << "cut at " << cut;
    }
}

// ---- XA decoder ----

namespace {
std::array<uint8_t, 2352> xa_sector(uint8_t seed, uint8_t coding) {
    std::array<uint8_t, 2352> r{};
    r[15] = 2;
    r[18] = r[22] = kRtAudio;
    r[19] = r[23] = coding;
    for (int g = 0; g < 18; ++g) {
        uint8_t* p = r.data() + 24 + 128 * g;
        for (int i = 0; i < 128; ++i) p[i] = static_cast<uint8_t>(seed * 31 + g * 17 + i * 13);
        for (int u = 0; u < 8; ++u) p[4 + u] = p[u] = static_cast<uint8_t>((u * 3 + seed) % 13 | ((u & 3) << 4));
    }
    return r;
}

Bytes save_xa(XaDecoder& x) {
    StateIo w;
    x.io(w);
    return w.take();
}
}  // namespace

TEST(StateXa, MidStreamRoundTrip) {
    XaDecoder a(nullptr, 1u << 12), b(nullptr, 1u << 12);
    auto s1 = xa_sector(1, 1), s2 = xa_sector(2, 5), s3 = xa_sector(3, 0);
    a.xa_sector(s1.data());
    a.xa_sector(s2.data());
    int16_t l, r;
    for (int i = 0; i < 1000; ++i) a.next_frame(l, r);  // consume part of the FIFO
    a.resample_push(1234, -321);                        // leave the resampler off a block boundary
    Bytes st = save_xa(a);
    StateIo rd{std::span<const uint8_t>(st)};
    b.io(rd);
    EXPECT_TRUE(rd.at_end());
    EXPECT_EQ(save_xa(b), st);
    EXPECT_EQ(b.queued(), a.queued());
    EXPECT_EQ(b.sectors(), a.sectors());

    a.xa_sector(s3.data());
    b.xa_sector(s3.data());
    ASSERT_EQ(a.queued(), b.queued());
    while (a.queued()) {
        int16_t al, ar, bl, br;
        ASSERT_TRUE(a.next_frame(al, ar));
        ASSERT_TRUE(b.next_frame(bl, br));
        ASSERT_EQ(al, bl);
        ASSERT_EQ(ar, br);
    }
}

TEST(StateXa, WrappedRingKeepsOrder) {
    XaDecoder a(nullptr, 64), b(nullptr, 64);
    auto s = xa_sector(9, 1);
    a.xa_sector(s.data());  // overflows the tiny ring, so its head has moved
    Bytes st = save_xa(a);
    StateIo rd{std::span<const uint8_t>(st)};
    b.io(rd);
    EXPECT_EQ(save_xa(b), st);
    int16_t al, ar, bl, br;
    while (a.next_frame(al, ar)) {
        ASSERT_TRUE(b.next_frame(bl, br));
        ASSERT_EQ(al, bl);
        ASSERT_EQ(ar, br);
    }
}

TEST(StateXa, TruncatedDataThrows) {
    XaDecoder a(nullptr, 1u << 10);
    auto s = xa_sector(4, 1);
    a.xa_sector(s.data());
    Bytes st = save_xa(a);
    st.resize(st.size() - 1);
    XaDecoder b(nullptr, 1u << 10);
    StateIo rd{std::span<const uint8_t>(st)};
    EXPECT_THROW(b.io(rd), StateError);
}
