// Save-state round trips for SIO0 (pad / memory card protocol, transfers and ACK events in flight) and the MDEC
// (command state, tables, queued output and the pending wake-up).

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "dw3/dma.h"
#include "dw3/irq.h"
#include "dw3/mdec.h"
#include "dw3/sio.h"
#include "dw3/state.h"
#include "hw_test_util.h"

using dw3::Dma;
using dw3::IrqController;
using dw3::Mdec;
using dw3::SimplePads;
using dw3::Sio;
using dw3::StateError;
using dw3::StateIo;
using Bytes = std::vector<uint8_t>;

namespace {

template <class Dev>
Bytes save(dw3test::Clock& c, IrqController& irq, Dev& dev) {
    StateIo s;
    c.sched.io(s);
    irq.io(s);
    dev.io(s);
    return s.take();
}

template <class Dev>
void load(dw3test::Clock& c, IrqController& irq, Dev& dev, const Bytes& data) {
    StateIo s(data);
    c.sched.io(s);
    irq.io(s);
    dev.io(s);
    EXPECT_TRUE(s.at_end());
}

// ---- SIO ----

constexpr uint32_t kData = 0x1F801040u, kStat = 0x1F801044u, kMode = 0x1F801048u, kCtrl = 0x1F80104Au,
                   kBaud = 0x1F80104Eu, kIStat = 0x1F801070u;
constexpr int64_t kByte = 8 * 0x88;
constexpr int64_t kSettle = kByte + Sio::kAckDelay + Sio::kAckWidth + 1;
constexpr uint32_t kSel = 0x1003;

using Card = std::array<uint8_t, Sio::kCardSize>;

struct SioRig : dw3test::Clock {
    IrqController irq{bus};
    Sio sio{bus, sched, irq};

    SioRig(SimplePads& pad, Card& card) {
        sio.set_pad_source(0, &pad);
        sio.set_pad_source(1, &pad);
        EXPECT_TRUE(sio.attach_card(0, card));
        w16(kBaud, 0x88);
        w16(kMode, 0x000D);
    }
    void ctl(uint32_t v) { w16(kCtrl, v); }
    void ack_irq() {
        ctl(r16(kCtrl) | 0x10);
        w32(kIStat, ~0x80u);
    }
    // Exchange one byte and report everything observable afterwards.
    struct Seen {
        uint32_t stat, irq_stat, rx;
        bool operator==(const Seen&) const = default;
    };
    Seen xfer(uint8_t tx) {
        w8(kData, tx);
        run_to(ctx().cycles + kSettle);
        Seen s{r32(kStat), irq.stat(), bus.read(kData, 1)};
        ack_irq();
        return s;
    }
    // Starts a byte and runs `part` cycles into it without servicing the result.
    void start(uint8_t tx, int64_t part) {
        w8(kData, tx);
        run_to(ctx().cycles + part);
    }
};

struct SioPair {
    SimplePads pad;
    Card card{};
    Card card_b{};  // the second instance's own storage, so writes can be compared
    std::unique_ptr<SioRig> a, b;
    SioPair() {
        pad.plugged = {true, true};
        pad.pressed = {0x1234, 0x0F0F};
        for (size_t i = 0; i < card.size(); ++i) card[i] = static_cast<uint8_t>(i * 7 + i / 256);
        card_b = card;
        a = std::make_unique<SioRig>(pad, card);
        b = std::make_unique<SioRig>(pad, card_b);
    }
    // Copies a's state into b and checks that saving b gives the same bytes back.
    Bytes clone() {
        Bytes d = save(*a, a->irq, a->sio);
        b->ctx().cycles = a->ctx().cycles;
        load(*b, b->irq, b->sio, d);
        EXPECT_EQ(save(*b, b->irq, b->sio), d);
        return d;
    }
    // Steps both forward in small increments and requires identical register-visible behaviour.
    void run_same(int64_t cycles, int64_t step = 13) {
        for (int64_t t = 0; t < cycles; t += step) {
            a->run_to(a->ctx().cycles + step);
            b->run_to(b->ctx().cycles + step);
            ASSERT_EQ(a->r32(kStat), b->r32(kStat)) << "t=" << t;
            ASSERT_EQ(a->irq.stat(), b->irq.stat()) << "t=" << t;
            ASSERT_EQ(a->sched.pending_count(), b->sched.pending_count());
        }
    }
    void xfer_same(const Bytes& tx) {
        for (size_t i = 0; i < tx.size(); ++i) ASSERT_EQ(a->xfer(tx[i]), b->xfer(tx[i])) << "byte " << i;
    }
};

}  // namespace

TEST(StateSio, PadPollMidByteWithEventPending) {
    SioPair p;
    p.a->ctl(kSel);
    p.a->xfer(0x01);
    p.a->start(0x42, kByte / 2);
    ASSERT_TRUE(p.a->sio.busy());
    p.clone();
    EXPECT_TRUE(p.b->sio.busy());
    p.run_same(kSettle * 2);
    p.a->ack_irq();
    p.b->ack_irq();
    p.xfer_same({0x00, 0x00, 0x00});
}

TEST(StateSio, PadPollAckPending) {
    SioPair p;
    p.a->ctl(kSel);
    p.a->xfer(0x01);
    p.a->start(0x42, kByte + Sio::kAckDelay / 2);  // byte done, /ACK not yet low
    ASSERT_FALSE(p.a->sio.busy());
    p.clone();
    p.run_same(kSettle);
    EXPECT_TRUE(p.a->irq.stat() & 0x80);
    EXPECT_TRUE(p.b->irq.stat() & 0x80);
    p.a->ack_irq();
    p.b->ack_irq();
    p.xfer_same({0x00, 0x00, 0x00});
}

TEST(StateSio, PadAckLowAndEndPending) {
    SioPair p;
    p.a->ctl(kSel);
    p.a->start(0x01, kByte + Sio::kAckDelay + Sio::kAckWidth / 2);
    ASSERT_TRUE(p.a->sio.stat() & Sio::kStatAckLow);
    p.clone();
    EXPECT_TRUE(p.b->sio.stat() & Sio::kStatAckLow);
    p.run_same(Sio::kAckWidth * 2, 7);
    EXPECT_FALSE(p.b->sio.stat() & Sio::kStatAckLow);
}

TEST(StateSio, QueuedByteBehindTransfer) {
    SioPair p;
    p.a->ctl(kSel);
    p.a->w8(kData, 0x01);
    p.a->w8(kData, 0x42);  // held in the one-deep register
    p.a->run_to(p.a->ctx().cycles + kByte / 3);
    p.clone();
    p.run_same(kSettle * 3);
    EXPECT_EQ(p.a->r32(kStat), p.b->r32(kStat));
    EXPECT_EQ(p.a->bus.read(kData, 1), p.b->bus.read(kData, 1));
    EXPECT_EQ(p.a->bus.read(kData, 1), p.b->bus.read(kData, 1));
}

TEST(StateSio, ConfigModeSurvives) {
    SioPair p;
    auto run = [](SioRig& r, const Bytes& tx) {
        r.ctl(kSel);
        for (uint8_t b : tx) r.xfer(b);
        r.ctl(0x1000);
    };
    run(*p.a, {0x01, 0x43, 0x00, 0x01, 0x00});                          // enter config mode
    run(*p.a, {0x01, 0x44, 0x00, 0x01, 0x03, 0, 0, 0, 0});              // analog + lock
    run(*p.a, {0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF});  // rumble map
    ASSERT_TRUE(p.a->sio.pad_config_mode(0));
    p.clone();
    EXPECT_TRUE(p.b->sio.pad_config_mode(0));
    EXPECT_EQ(p.a->sio.pad_analog(0), p.b->sio.pad_analog(0));
    p.a->ctl(kSel);
    p.b->ctl(kSel);
    p.xfer_same({0x01, 0x4D, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
}

TEST(StateSio, CardReadMidFrame) {
    SioPair p;
    p.a->ctl(kSel);
    Bytes tx = {0x81, 0x52, 0, 0, 0x00, 0x05};
    tx.resize(10 + 128 + 2, 0);
    for (size_t i = 0; i < 40; ++i) p.a->xfer(tx[i]);
    p.a->start(0, kByte / 2);
    p.clone();
    p.run_same(kSettle);
    p.a->ack_irq();
    p.b->ack_irq();
    p.xfer_same(Bytes(tx.begin() + 41, tx.end()));
}

TEST(StateSio, CardWriteMidFrameCommitsIdentically) {
    SioPair p;
    p.a->ctl(kSel);
    uint8_t chk = 0x00 ^ 0x09;
    Bytes tx = {0x81, 0x57, 0, 0, 0x00, 0x09};
    for (size_t i = 0; i < 128; ++i) {
        tx.push_back(static_cast<uint8_t>(0xA0 + i));
        chk ^= tx.back();
    }
    tx.push_back(chk);
    tx.insert(tx.end(), {0, 0, 0});
    for (size_t i = 0; i < 70; ++i) p.a->xfer(tx[i]);
    p.clone();
    int writes_a = 0, writes_b = 0;
    p.a->sio.on_card_write = [&](int, uint32_t) { ++writes_a; };
    p.b->sio.on_card_write = [&](int, uint32_t) { ++writes_b; };
    p.xfer_same(Bytes(tx.begin() + 70, tx.end()));
    EXPECT_EQ(writes_a, 1);
    EXPECT_EQ(writes_b, 1);
    EXPECT_EQ(p.a->sio.card_flag(0), p.b->sio.card_flag(0));
    EXPECT_EQ(p.a->sio.card_flag(0) & 0x08, 0);
    EXPECT_EQ(p.card, p.card_b);
    EXPECT_EQ(p.card[9 * 128], 0xA0);
}

TEST(StateSio, CardFlagAndRegistersRoundTrip) {
    SioPair p;
    p.a->sio.clear_card_new(0);
    p.a->ctl(0x1000 | 0x0400);  // ACK and TX IRQ enable, not selected
    p.clone();
    EXPECT_EQ(p.b->sio.card_flag(0), p.a->sio.card_flag(0));
    EXPECT_EQ(p.b->sio.ctrl(), p.a->sio.ctrl());
    EXPECT_EQ(p.b->sio.baud(), 0x88);
    EXPECT_EQ(p.b->sio.mode(), 0x000D);
}

TEST(StateSio, TruncatedDataThrows) {
    SioPair p;
    p.a->ctl(kSel);
    p.a->xfer(0x01);
    p.a->start(0x42, kByte / 2);
    Bytes d = save(*p.a, p.a->irq, p.a->sio);
    for (size_t n = 0; n < d.size(); n += 3) {
        Bytes cut(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(n));
        StateIo s(cut);
        EXPECT_THROW(
            {
                p.b->sched.io(s);
                p.b->irq.io(s);
                p.b->sio.io(s);
            },
            StateError)
            << "cut at " << n;
    }
}

// ---- MDEC ----

namespace {

constexpr uint32_t kMdecData = 0x1F801820u, kMdecCtrl = 0x1F801824u;
constexpr uint16_t kEob = 0xFE00;

struct MdecRig : dw3test::Clock {
    IrqController irq{bus};
    Dma dma{bus, sched, irq};
    Mdec mdec{bus, sched, dma};

    void send(uint32_t w) { w32(kMdecData, w); }
    void set_tables() {
        send((2u << 29) | 1u);
        for (int i = 0; i < 32; ++i) send((i % 7 + 2) * 0x01010101u);
        send(3u << 29);
        auto t = Mdec::default_scale_table();
        for (int i = 0; i < 32; ++i)
            send(static_cast<uint16_t>(t[2 * i]) | (static_cast<uint32_t>(static_cast<uint16_t>(t[2 * i + 1])) << 16));
    }
    // 15-bit decode command followed by `count` macroblocks (DC plus one AC coefficient per block).
    static std::vector<uint32_t> macroblocks(int count) {
        std::vector<uint16_t> hw;
        for (int m = 0; m < count; ++m)
            for (int b = 0; b < 6; ++b) {
                hw.push_back(static_cast<uint16_t>((1u << 10) | ((m * 37 + b * 11) & 0x3FF)));
                hw.push_back(static_cast<uint16_t>(0x0001 | (b << 10)));
                hw.push_back(kEob);
            }
        if (hw.size() & 1) hw.push_back(kEob);
        std::vector<uint32_t> w;
        w.push_back((1u << 29) | (3u << 27) | (1u << 25) | static_cast<uint32_t>(hw.size() / 2));
        for (size_t i = 0; i < hw.size(); i += 2) w.push_back(hw[i] | (static_cast<uint32_t>(hw[i + 1]) << 16));
        return w;
    }
};

struct MdecPair {
    MdecRig a, b;
    Bytes clone() {
        Bytes d = save(a, a.irq, a.mdec);
        b.ctx().cycles = a.ctx().cycles;
        load(b, b.irq, b.mdec, d);
        EXPECT_EQ(save(b, b.irq, b.mdec), d);
        return d;
    }
    void run_same(int64_t cycles, int64_t step = 97) {
        for (int64_t t = 0; t < cycles; t += step) {
            a.run_to(a.ctx().cycles + step);
            b.run_to(b.ctx().cycles + step);
            ASSERT_EQ(a.r32(kMdecCtrl), b.r32(kMdecCtrl)) << "t=" << t;
            ASSERT_EQ(a.mdec.available_words(), b.mdec.available_words()) << "t=" << t;
            ASSERT_EQ(a.sched.pending_count(), b.sched.pending_count());
        }
    }
    void drain_same() {
        a.run_to(a.ctx().cycles + 1'000'000);
        b.run_to(b.ctx().cycles + 1'000'000);
        ASSERT_EQ(a.mdec.available_words(), b.mdec.available_words());
        while (a.mdec.available_words()) ASSERT_EQ(a.r32(kMdecData), b.r32(kMdecData));
        EXPECT_EQ(a.r32(kMdecCtrl), b.r32(kMdecCtrl));
    }
};

}  // namespace

TEST(StateMdec, QueuedOutputWithEventPending) {
    MdecPair p;
    p.a.set_tables();
    p.a.w32(kMdecCtrl, 3u << 29);
    for (uint32_t w : MdecRig::macroblocks(4)) p.a.send(w);
    p.a.run_to(p.a.ctx().cycles + Mdec::kCyclesPerBlock * 8);  // part of the output is visible, the rest queued
    ASSERT_GT(p.a.mdec.queued_words(), 0u);
    ASSERT_GT(p.a.mdec.available_words(), 0u);
    ASSERT_GT(p.a.sched.pending_count(), 0u);
    ASSERT_TRUE(p.a.mdec.decoding());
    for (int i = 0; i < 5; ++i) p.a.r32(kMdecData);  // so the buffer head is not at zero
    p.clone();
    EXPECT_EQ(p.b.mdec.queued_words(), p.a.mdec.queued_words());
    EXPECT_EQ(p.b.mdec.macroblocks_decoded(), p.a.mdec.macroblocks_decoded());
    EXPECT_EQ(p.b.mdec.luma_table(), p.a.mdec.luma_table());
    EXPECT_EQ(p.b.mdec.chroma_table(), p.a.mdec.chroma_table());
    EXPECT_EQ(p.b.mdec.scale_table(), p.a.mdec.scale_table());
    p.run_same(Mdec::kCyclesPerBlock * 40);
    p.drain_same();
}

TEST(StateMdec, MidCommandInput) {
    MdecPair p;
    p.a.set_tables();
    auto words = MdecRig::macroblocks(2);
    size_t half = words.size() / 2;
    for (size_t i = 0; i < half; ++i) p.a.send(words[i]);
    EXPECT_TRUE(p.a.r32(kMdecCtrl) & (1u << 29));  // command busy
    p.clone();
    EXPECT_EQ(p.a.r32(kMdecCtrl), p.b.r32(kMdecCtrl));
    for (size_t i = half; i < words.size(); ++i) {
        p.a.send(words[i]);
        p.b.send(words[i]);
        ASSERT_EQ(p.a.r32(kMdecCtrl), p.b.r32(kMdecCtrl)) << i;
    }
    p.run_same(Mdec::kCyclesPerBlock * 20);
    p.drain_same();
    EXPECT_EQ(p.a.mdec.macroblocks_decoded(), p.b.mdec.macroblocks_decoded());
}

TEST(StateMdec, MidTableUpload) {
    MdecPair p;
    p.a.send((2u << 29) | 1u);
    for (uint32_t i = 0; i < 11; ++i) p.a.send(0x05050505u + i);
    p.clone();
    for (uint32_t i = 11; i < 32; ++i) {
        p.a.send(0x05050505u + i);
        p.b.send(0x05050505u + i);
    }
    EXPECT_EQ(p.a.mdec.luma_table(), p.b.mdec.luma_table());
    EXPECT_EQ(p.a.mdec.chroma_table(), p.b.mdec.chroma_table());
    EXPECT_EQ(p.a.r32(kMdecCtrl), p.b.r32(kMdecCtrl));
}

TEST(StateMdec, IdleRoundTrip) {
    MdecPair p;
    p.a.set_tables();
    p.clone();
    EXPECT_EQ(p.a.r32(kMdecCtrl), p.b.r32(kMdecCtrl));
    EXPECT_EQ(p.a.mdec.scale_table(), p.b.mdec.scale_table());
}

TEST(StateMdec, TruncatedDataThrows) {
    MdecPair p;
    p.a.set_tables();
    p.a.w32(kMdecCtrl, 3u << 29);
    for (uint32_t w : MdecRig::macroblocks(3)) p.a.send(w);
    p.a.run_to(p.a.ctx().cycles + Mdec::kCyclesPerBlock * 4);
    Bytes d = save(p.a, p.a.irq, p.a.mdec);
    for (size_t n = 0; n < d.size(); n += 5) {
        Bytes cut(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(n));
        StateIo s(cut);
        EXPECT_THROW(
            {
                p.b.sched.io(s);
                p.b.irq.io(s);
                p.b.mdec.io(s);
            },
            StateError)
            << "cut at " << n;
    }
}
