// Save-state framework: StateIo encoding and versioning, file header checks, and resuming a guest PC in code laid
// out differently from the code that saved it.

#include <gtest/gtest.h>

#include <vector>

#include "dw3/machine.h"
#include "dw3/state.h"

using namespace dw3;

TEST(StateIo, RoundTripsScalarsAndContainers) {
    StateIo w;
    uint8_t a = 0xAB;
    int16_t b = -1234;
    uint32_t c = 0xDEADBEEF;
    int64_t d = -0x123456789ALL;
    bool e = true;
    float f = 1.5f;
    std::array<uint16_t, 3> arr{1, 2, 3};
    std::vector<uint8_t> bytes{9, 8, 7};
    std::vector<uint32_t> words{10, 20};
    std::string str = "hello";
    struct Item {
        uint8_t x = 0;
        uint32_t y = 0;
    };
    std::vector<Item> items{{1, 2}, {3, 4}};
    auto run = [&](StateIo& s) {
        StateIo::Section sec(s, "TEST");
        s.io(a);
        s.io(b);
        s.io(c);
        s.io(d);
        s.io(e);
        s.io(f);
        s.io(arr);
        s.io(bytes);
        s.io(words);
        s.io(str);
        s.list(items, [&](Item& i) {
            s.io(i.x);
            s.io(i.y);
        });
    };
    run(w);
    auto data = w.take();

    a = 0;
    b = 0;
    c = 0;
    d = 0;
    e = false;
    f = 0;
    arr = {};
    bytes.clear();
    words.clear();
    str.clear();
    items.clear();
    StateIo r(data);
    run(r);
    EXPECT_TRUE(r.at_end());
    EXPECT_EQ(a, 0xAB);
    EXPECT_EQ(b, -1234);
    EXPECT_EQ(c, 0xDEADBEEFu);
    EXPECT_EQ(d, -0x123456789ALL);
    EXPECT_TRUE(e);
    EXPECT_EQ(f, 1.5f);
    EXPECT_EQ(arr[2], 3);
    EXPECT_EQ(bytes, (std::vector<uint8_t>{9, 8, 7}));
    EXPECT_EQ(words, (std::vector<uint32_t>{10, 20}));
    EXPECT_EQ(str, "hello");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[1].y, 4u);
}

TEST(StateIo, IsLittleEndianWithFixedWidths) {
    StateIo w;
    uint32_t v = 0x01020304;
    w.io(v);
    EXPECT_EQ(w.written(), (std::vector<uint8_t>{4, 3, 2, 1}));
    StateIo w2;
    enum class E { A = -2 };
    E e = E::A;
    w2.io(e);
    EXPECT_EQ(w2.written(), (std::vector<uint8_t>{0xFE, 0xFF, 0xFF, 0xFF}));  // enums are 32-bit
}

TEST(StateIo, SectionHeaderLayout) {
    StateIo w;
    {
        StateIo::Section sec(w, "ABCD", 3);
        uint16_t v = 0x0201;
        w.io(v);
    }
    EXPECT_EQ(w.written(), (std::vector<uint8_t>{'A', 'B', 'C', 'D', 3, 0, 0, 0, 2, 0, 0, 0, 1, 2}));
}

TEST(StateIo, TruncationIsAnError) {
    StateIo w;
    {
        StateIo::Section sec(w, "ABCD");
        uint32_t v = 7;
        w.io(v);
    }
    auto data = w.take();
    for (std::size_t cut = 0; cut < data.size(); ++cut) {
        StateIo r(std::span<const uint8_t>(data.data(), cut));
        EXPECT_THROW(
            {
                StateIo::Section sec(r, "ABCD");
                uint32_t v = 0;
                r.io(v);
            },
            StateError)
            << "cut at " << cut;
    }
}

TEST(StateIo, ReadingTheWrongAmountIsAnError) {
    StateIo w;
    {
        StateIo::Section sec(w, "ABCD");
        uint32_t v = 7;
        w.io(v);
    }
    auto data = w.take();
    StateIo r(data);
    EXPECT_THROW(
        {
            StateIo::Section sec(r, "ABCD");
            uint16_t half = 0;
            r.io(half);
        },
        StateError);
}

TEST(StateIo, UnknownSectionsAreSkipped) {
    StateIo w;
    {
        StateIo::Section sec(w, "AAAA");
        uint32_t v = 1;
        w.io(v);
    }
    {
        StateIo::Section sec(w, "ZZZZ", 9);  // written by some other build
        std::vector<uint8_t> junk(100, 0x5A);
        w.io(junk);
        StateIo::Section inner(w, "YYYY");
        uint8_t x = 3;
        w.io(x);
    }
    {
        StateIo::Section sec(w, "BBBB");
        uint32_t v = 2;
        w.io(v);
    }
    auto data = w.take();
    StateIo r(data);
    uint32_t a = 0, b = 0;
    {
        StateIo::Section sec(r, "AAAA");
        r.io(a);
    }
    {
        StateIo::Section sec(r, "BBBB");  // ZZZZ in between is skipped by its length
        r.io(b);
    }
    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, 2u);
    EXPECT_TRUE(r.at_end());

    StateIo r2(data);
    EXPECT_THROW(r2.begin("NONE"), StateError);  // a missing section is named in the error
}

namespace {

// A block that grew a field in each of two later versions.
struct Block {
    uint32_t a = 0, b = 0, c = 0;
    void io(StateIo& s, uint32_t current, uint32_t min_version = 1) {
        StateIo::Section sec(s, "BLK0", current, min_version);
        s.io(a);
        if (current >= 2) s.io_since(2, b, uint32_t{77});
        if (current >= 3) s.io_since(3, c, uint32_t{88});
    }
};

}  // namespace

TEST(StateVersion, OlderStatesLoadWithDefaults) {
    StateIo w;
    Block old;
    old.a = 5;
    old.io(w, 1);
    auto v1 = w.take();

    Block now;
    StateIo r(v1);
    now.io(r, 3);
    EXPECT_EQ(now.a, 5u);
    EXPECT_EQ(now.b, 77u);
    EXPECT_EQ(now.c, 88u);

    StateIo w2;
    old.b = 6;
    old.io(w2, 2);
    auto v2 = w2.take();
    Block mid;
    StateIo r2(v2);
    mid.io(r2, 3);
    EXPECT_EQ(mid.b, 6u);
    EXPECT_EQ(mid.c, 88u);
}

TEST(StateVersion, NewerOrTooOldSectionsAreRejectedWithTheirName) {
    StateIo w;
    Block b;
    b.io(w, 3);
    auto v3 = w.take();
    Block reader;
    StateIo r(v3);
    try {
        reader.io(r, 2);
        FAIL();
    } catch (const StateError& e) {
        EXPECT_NE(std::string(e.what()).find("BLK0"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("newer"), std::string::npos);
    }

    StateIo w1;
    Block b1;
    b1.io(w1, 1);
    auto v1 = w1.take();
    StateIo r1(v1);
    EXPECT_THROW(reader.io(r1, 3, 2), StateError);  // layout 1 is older than the oldest this build supports
}

TEST(SaveStateFile, HeaderIsChecked) {
    std::vector<uint8_t> payload(5000, 7);
    payload[100] = 1;
    auto file = savestate::wrap(payload, 0x1122334455667788ull);
    EXPECT_LT(file.size(), payload.size());  // compressed
    savestate::Info info;
    EXPECT_EQ(savestate::unwrap(file, 0x1122334455667788ull, info), payload);
    EXPECT_EQ(info.encoding, savestate::kPlain);

    EXPECT_THROW(savestate::unwrap(file, 0x99, info), StateError);  // other game

    auto bad_magic = file;
    bad_magic[0] ^= 0xFF;
    EXPECT_THROW(savestate::unwrap(bad_magic, 0x1122334455667788ull, info), StateError);

    auto newer = file;
    newer[4] = 99;  // container format from a newer build
    auto odd_encoding = file;
    odd_encoding[8] = 77;
    EXPECT_THROW(savestate::unwrap(odd_encoding, 0x1122334455667788ull, info), StateError);
    EXPECT_THROW(savestate::unwrap(newer, 0x1122334455667788ull, info), StateError);

    auto damaged = file;
    damaged.back() ^= 1;
    EXPECT_THROW(savestate::unwrap(damaged, 0x1122334455667788ull, info), StateError);

    auto truncated = file;
    truncated.pop_back();
    EXPECT_THROW(savestate::unwrap(truncated, 0x1122334455667788ull, info), StateError);
    EXPECT_THROW(savestate::unwrap(std::span<const uint8_t>(file.data(), 3), 0, info), StateError);
}

TEST(SaveStateFile, ErrorMessagesSayWhatIsWrong) {
    auto file = savestate::wrap({}, 1);
    savestate::Info info;
    try {
        savestate::unwrap(file, 2, info);
        FAIL();
    } catch (const StateError& e) {
        EXPECT_NE(std::string(e.what()).find("different game"), std::string::npos);
    }
}

// ---- Resuming a guest PC -------------------------------------------------------------------------------------------
//
// The guest program: main() calls work() three times; work() counts to five with an event check on each backward
// jump. r20 folds the order of events into a checksum, so a resumed run must end with the same value as an
// uninterrupted one. The calling convention is the MIPS one: the caller sets r31 to the return label, the callee
// returns to r31, and main keeps its own return address (0 = leave the game) in r18.
//
// Layout A writes each guest function as one host function. Layout B has the same guest semantics and the same
// guest addresses, but splits main after its call and work into a setup and a loop function, so the functions that
// own the resume labels are different from the ones that saved the state.

namespace {

constexpr uint32_t kAfterCall = 0x80010008u, kLoopHead = 0x80010108u;

void fold(RecompContext* c, uint32_t v) { c->r[20] = c->r[20] * 31 + v; }

// -- layout A
void a_work(RecompContext* ctx) {
    if (DW3_UNLIKELY(ctx->resume)) {
        uint32_t rp = ctx->resume;
        ctx->resume = 0;
        if (rp != kLoopHead) { dw3_bad_resume(ctx, 0x80010100u); return; }
        goto L_top;
    }
    ctx->r[17] = 0;
L_top:
    fold(ctx, ctx->r[17]);
    ctx->cycles += 10;
    if (++ctx->r[17] < 5) {
        ctx->next_event = 0;  // make every check call the event handler
        DW3_CHECK_EVENTS(ctx, kLoopHead);
        goto L_top;
    }
}

void a_main(RecompContext* ctx) {
    if (DW3_UNLIKELY(ctx->resume)) {
        uint32_t rp = ctx->resume;
        ctx->resume = 0;
        if (rp != kAfterCall) { dw3_bad_resume(ctx, 0x80010000u); return; }
        goto L_after;
    }
    ctx->r[18] = ctx->r[31];
    ctx->r[16] = 0;
L_loop:
    ctx->r[31] = kAfterCall;
    a_work(ctx);
L_after:
    fold(ctx, 1000 + ctx->r[16]);
    if (++ctx->r[16] < 3) goto L_loop;
    ctx->r[31] = ctx->r[18];
}

const RecompFuncEntry kFuncsA[] = {{0x80010000, a_main}, {0x80010100, a_work}};
const RecompResumeEntry kResumeA[] = {{kAfterCall, a_main}, {kLoopHead, a_work}, {0, nullptr}};

// -- layout B
void b_loop(RecompContext* ctx) {
    if (DW3_UNLIKELY(ctx->resume)) {
        uint32_t rp = ctx->resume;
        ctx->resume = 0;
        if (rp != kLoopHead) { dw3_bad_resume(ctx, 0x80010180u); return; }
        goto L_top;
    }
L_top:
    fold(ctx, ctx->r[17]);
    ctx->cycles += 10;
    if (++ctx->r[17] < 5) {
        ctx->next_event = 0;
        DW3_CHECK_EVENTS(ctx, kLoopHead);
        goto L_top;
    }
}

void b_work(RecompContext* ctx) {
    ctx->r[17] = 0;
    b_loop(ctx);
}

void b_after(RecompContext* ctx) {  // the part of main behind the call, as a function of its own
    if (DW3_UNLIKELY(ctx->resume)) {
        uint32_t rp = ctx->resume;
        ctx->resume = 0;
        if (rp != kAfterCall) { dw3_bad_resume(ctx, 0x80010040u); return; }
    }
    fold(ctx, 1000 + ctx->r[16]);
    if (++ctx->r[16] < 3) {
        ctx->r[31] = kAfterCall;
        b_work(ctx);
        b_after(ctx);
        return;
    }
    ctx->r[31] = ctx->r[18];
}

void b_main(RecompContext* ctx) {
    ctx->r[18] = ctx->r[31];
    ctx->r[16] = 0;
    ctx->r[31] = kAfterCall;
    b_work(ctx);
    b_after(ctx);
}

const RecompFuncEntry kFuncsB[] = {{0x80010000, b_main}, {0x80010040, b_after}, {0x80010100, b_work}, {0x80010180, b_loop}};
const RecompResumeEntry kResumeB[] = {{kAfterCall, b_after}, {kLoopHead, b_loop}, {0, nullptr}};

RecompSegment make_seg(const char* name, uint32_t base, const RecompFuncEntry* f, uint32_t n, const RecompResumeEntry* r) {
    RecompSegment s{};
    s.name = name;
    s.base = base;
    s.size = 0x1000;
    s.funcs = f;
    s.func_count = n;
    s.resume = r;
    return s;
}

const RecompSegment kSegsA[] = {make_seg("MAIN", 0x80010000, kFuncsA, 2, kResumeA)};
const RecompSegment kSegsB[] = {make_seg("MAIN", 0x80010000, kFuncsB, 4, kResumeB)};

}  // namespace

TEST(MachineState, StateSavedByOneCodeLayoutResumesInAnother) {
    Machine m(kSegsA);
    m.ctx().r[20] = 5;
    std::vector<uint8_t> state;
    int checks = 0;
    m.on_service_events = [&](RecompContext&) {
        m.ctx().next_event = INT64_MAX;
        if (++checks == 7) {  // second call of work(), after its third iteration
            StateIo w;
            m.io(w);
            state = w.take();
        }
    };
    a_main(&m.ctx());
    ASSERT_FALSE(state.empty());
    const uint32_t expected = m.ctx().r[20];

    Machine m2(kSegsB);
    StateIo r(state);
    m2.io(r);
    EXPECT_TRUE(r.at_end());
    ASSERT_TRUE(m2.resume_pending());
    m2.on_service_events = [&](RecompContext&) { m2.ctx().next_event = INT64_MAX; };
    m2.resume();
    EXPECT_FALSE(m2.resume_pending());
    EXPECT_EQ(m2.ctx().r[20], expected);
    EXPECT_EQ(m2.ctx().r[16], 3u);

    // The same state also resumes in the layout that wrote it, and a layout-B run saves a state layout A can take.
    Machine m3(kSegsA);
    StateIo r3(state);
    m3.io(r3);
    m3.on_service_events = [&](RecompContext&) { m3.ctx().next_event = INT64_MAX; };
    m3.resume();
    EXPECT_EQ(m3.ctx().r[20], expected);
}

TEST(MachineState, ResumeAtAReturnLabelOnly) {
    // A state whose innermost function sits at the label after a call (cur_pc = kAfterCall): resume enters main there.
    Machine m(kSegsA);
    m.ctx().r[20] = 5;
    std::vector<uint8_t> state;
    // run main once up to its first call returning; simulate by hand
    m.ctx().r[18] = 0;
    m.ctx().r[16] = 0;
    m.ctx().r[31] = kAfterCall;
    a_work(&m.ctx());
    m.ctx().cur_pc = kAfterCall;
    StateIo w;
    m.io(w);
    state = w.take();
    const uint32_t partial = m.ctx().r[20];
    (void)partial;

    Machine m2(kSegsB);
    StateIo r(state);
    m2.io(r);
    m2.on_service_events = [&](RecompContext&) { m2.ctx().next_event = INT64_MAX; };
    m2.resume();  // finishes main: two more work() calls
    EXPECT_EQ(m2.ctx().r[16], 3u);
}

TEST(MachineState, QueuedActionsRunOnlyAtASafePoint) {
    Machine m(kSegsA);
    int ran = 0;
    m.run_at_safe_point([&] { ++ran; });
    m.service_events();  // no event check of recompiled code has run: not a safe point
    EXPECT_EQ(ran, 0);
    m.ctx().cur_pc = kLoopHead;
    m.kernel_idle = [] { return false; };
    m.service_events();
    EXPECT_EQ(ran, 0);
    m.kernel_idle = nullptr;
    m.service_events();
    EXPECT_EQ(ran, 1);
    m.service_events();
    EXPECT_EQ(ran, 1);  // actions run once
}

TEST(MachineState, CannotBeSavedOutsideASafePoint) {
    Machine m(kSegsA);
    StateIo w;
    EXPECT_THROW(m.io(w), StateError);
    m.ctx().cur_pc = kLoopHead;
    m.kernel_idle = [] { return false; };  // an interrupt handler or event callback is running
    StateIo w2;
    EXPECT_THROW(m.io(w2), StateError);
}

TEST(MachineState, BuildWithoutALabelAtThePcRefusesWithTheAddress) {
    Machine m(kSegsA);
    m.ctx().cur_pc = kLoopHead;
    StateIo w;
    m.io(w);
    auto state = w.take();

    static const RecompResumeEntry no_loop[] = {{kAfterCall, a_main}, {0, nullptr}};
    const RecompSegment other[] = {make_seg("MAIN", 0x80010000, kFuncsA, 2, no_loop)};
    Machine m2(other);
    StateIo r(state);
    try {
        m2.io(r);
        FAIL();
    } catch (const StateError& e) {
        EXPECT_NE(std::string(e.what()).find("0x80010108"), std::string::npos);
    }
}

TEST(MachineState, OverlaysAreMatchedByName) {
    static const RecompFuncEntry ov_funcs[] = {{0x80020000, a_work}};
    static const RecompResumeEntry ov_resume[] = {{0x80020010, a_work}, {0, nullptr}};
    const RecompSegment with_overlay[] = {make_seg("MAIN", 0x80010000, kFuncsA, 2, kResumeA),
                                          make_seg("OVL", 0x80020000, ov_funcs, 1, ov_resume)};
    Machine m(with_overlay);
    m.load_segment_image(1, std::vector<uint8_t>(0x1000, 0));
    m.ctx().cur_pc = 0x80020010;
    StateIo w;
    m.io(w);
    auto state = w.take();

    // Same overlay in a different position of the table: found by name.
    const RecompSegment reordered[] = {make_seg("MAIN", 0x80010000, kFuncsA, 2, kResumeA),
                                       make_seg("OTHER", 0x80030000, kFuncsA, 2, kResumeA),
                                       make_seg("OVL", 0x80020000, ov_funcs, 1, ov_resume)};
    Machine m2(reordered);
    StateIo r(state);
    m2.io(r);
    EXPECT_TRUE(m2.resume_pending());

    // A build without that overlay says so.
    Machine m3(kSegsA);
    StateIo r3(state);
    try {
        m3.io(r3);
        FAIL();
    } catch (const StateError& e) {
        EXPECT_NE(std::string(e.what()).find("OVL"), std::string::npos);
    }
}
