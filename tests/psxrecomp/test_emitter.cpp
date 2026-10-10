// Differential tests: C emitted by psxrecomp (compiled at build time by emit_testgen) must behave
// exactly like the MIPS code, as executed by the reference interpreter.

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>

#include "dw3/machine.h"
#include "ref_interp.h"
#include "test_programs.h"

#include "emitted_tests.inc"

namespace {

constexpr int kInitsPerProgram = 4;

const std::vector<tp::TestProgram>& programs() {
    static const auto progs = tp::build_test_programs();
    return progs;
}

// Complete architectural state a program starts from.
struct InitState {
    uint32_t r[32] = {};
    uint32_t hi = 0, lo = 0;
    std::vector<uint8_t> data;     // RAM [kDataFillLo, +kDataFillSize)
    std::vector<uint8_t> scratch;  // 1 KiB scratchpad
};

InitState make_init(uint64_t seed) {
    tp::Rng rng(seed * 0x2545F4914F6CDD1Dull + 17);
    InitState st;
    auto value = [&]() -> uint32_t {
        uint32_t v = rng.next();
        switch (rng.below(4)) {
            case 0: return tp::kEdge32[rng.below(16)];
            case 1: return v & 0xFFu;
            default: return v ^ (rng.next() << 16);
        }
    };
    for (int i = 1; i < 32; ++i) st.r[i] = value();
    st.hi = value();
    st.lo = value();
    st.r[29] = tp::kStackTop;
    st.r[31] = tp::kSentinel;
    st.r[23] = tp::kDataBase;
    st.r[28] = tp::kDataKuseg;
    st.r[30] = tp::kDataKseg1;
    st.r[27] = tp::kDataMirror;
    st.r[26] = tp::kScratchBase;
    st.data.resize(tp::kDataFillSize);
    for (auto& b : st.data) b = static_cast<uint8_t>(rng.next());
    st.scratch.resize(DW3_SCRATCHPAD_SIZE);
    for (auto& b : st.scratch) b = static_cast<uint8_t>(rng.next());
    return st;
}

void apply_init(dw3::Machine& m, const InitState& st) {
    RecompContext& c = m.ctx();
    std::memcpy(c.r, st.r, sizeof(c.r));
    c.hi = st.hi;
    c.lo = st.lo;
    std::memcpy(m.bus().ram() + (tp::kDataFillLo & 0x1FFFFF), st.data.data(), st.data.size());
    std::memcpy(m.bus().scratchpad(), st.scratch.data(), st.scratch.size());
}

std::vector<uint8_t> image_of(const EmittedTest& t) {
    std::vector<uint8_t> img(t.word_count * 4);
    std::memcpy(img.data(), t.words, img.size());
    return img;
}

class EmittedDifferential : public testing::TestWithParam<std::size_t> {
protected:
    void SetUp() override {
        dw3::set_fatal_handler([](const std::string& msg) { throw std::runtime_error(msg); });
    }
    void TearDown() override { dw3::set_fatal_handler(nullptr); }
};

// Reports (at most a few) differences between the oracle state and the recompiled state.
bool compare(const char* what, dw3::Machine& ref, dw3::Machine& rec) {
    bool ok = true;
    int shown = 0;
    auto note = [&](const std::string& s) {
        ok = false;
        if (shown++ < 12) ADD_FAILURE() << what << ": " << s;
    };
    auto hex = [](uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof(b), "%08X", v);
        return std::string(b);
    };
    for (int i = 0; i < 32; ++i)
        if (ref.ctx().r[i] != rec.ctx().r[i])
            note("r" + std::to_string(i) + " oracle=" + hex(ref.ctx().r[i]) + " recompiled=" + hex(rec.ctx().r[i]));
    if (ref.ctx().hi != rec.ctx().hi) note("hi oracle=" + hex(ref.ctx().hi) + " recompiled=" + hex(rec.ctx().hi));
    if (ref.ctx().lo != rec.ctx().lo) note("lo oracle=" + hex(ref.ctx().lo) + " recompiled=" + hex(rec.ctx().lo));
    const uint8_t* a = ref.bus().ram();
    const uint8_t* b = rec.bus().ram();
    for (std::size_t k = 0; k < DW3_RAM_SIZE && shown < 12; ++k)
        if (a[k] != b[k]) note("RAM[" + hex(static_cast<uint32_t>(k)) + "] oracle=" + hex(a[k]) + " recompiled=" + hex(b[k]));
    if (std::memcmp(ref.bus().scratchpad(), rec.bus().scratchpad(), DW3_SCRATCHPAD_SIZE) != 0)
        note("scratchpad differs");
    return ok;
}

}  // namespace

TEST(EmittedManifest, MatchesSharedProgramList) {
    const auto& progs = programs();
    ASSERT_EQ(progs.size(), sizeof(kEmitted) / sizeof(kEmitted[0]));
    ASSERT_EQ(progs.size(), dw3_segment_count);
    EXPECT_GE(progs.size(), 300u);
    for (std::size_t i = 0; i < progs.size(); ++i) {
        const auto& p = progs[i];
        const auto& e = kEmitted[i];
        ASSERT_EQ(p.name, e.name) << i;
        ASSERT_EQ(p.base, e.base) << p.name;
        ASSERT_EQ(p.entry, e.entry) << p.name;
        ASSERT_EQ(p.words.size(), e.word_count) << p.name;
        ASSERT_EQ(0, std::memcmp(p.words.data(), e.words, e.word_count * 4)) << p.name << " words differ";
        ASSERT_STREQ(dw3_segments[i].name, e.name);
        ASSERT_EQ(dw3_segments[i].base, e.base);
    }
}

TEST_P(EmittedDifferential, MatchesReferenceInterpreter) {
    const std::size_t idx = GetParam();
    const tp::TestProgram& prog = programs()[idx];
    const EmittedTest& em = kEmitted[idx];
    const auto image = image_of(em);
    const std::span<const RecompSegment> table(dw3_segments, dw3_segment_count);
    SCOPED_TRACE(prog.name + ": " + prog.desc);

    for (int variant = 0; variant < kInitsPerProgram; ++variant) {
        SCOPED_TRACE("init variant " + std::to_string(variant));
        InitState st = make_init(idx * 8 + static_cast<uint64_t>(variant));

        dw3::Machine ref(table), rec(table);
        ref.load_segment_image(idx, image);
        rec.load_segment_image(idx, image);
        apply_init(ref, st);
        apply_init(rec, st);

        refint::Result r = refint::run(ref.ctx(), ref.bus(), prog.entry);
        ASSERT_FALSE(r.faulted) << r.error;
        ASSERT_TRUE(r.returned) << "oracle did not return (stopped at " << std::hex << r.pc << ")";

        try {
            em.func(&rec.ctx());
        } catch (const std::exception& e) {
            FAIL() << "recompiled code aborted: " << e.what();
        }
        EXPECT_EQ(rec.ctx().r[0], 0u);
        EXPECT_TRUE(compare(prog.name.c_str(), ref, rec));
    }
}

INSTANTIATE_TEST_SUITE_P(Programs, EmittedDifferential, testing::Range<std::size_t>(0, sizeof(kEmitted) / sizeof(kEmitted[0])),
                         [](const testing::TestParamInfo<std::size_t>& info) {
                             std::string n = kEmitted[info.param].name;
                             std::string d = programs()[info.param].desc;
                             for (char& c : d)
                                 if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
                             return n + "_" + d;
                         });

// ---- runtime behaviour around emitted code ----

TEST(EmittedRuntime, BackwardBranchServicesEvents) {
    // Find the countdown loop program and make sure DW3_CHECK_EVENTS fires on its back edge.
    const auto& progs = programs();
    std::size_t idx = progs.size();
    for (std::size_t i = 0; i < progs.size(); ++i)
        if (progs[i].desc == "count-down loop (bgtz)") idx = i;
    ASSERT_LT(idx, progs.size());
    const std::span<const RecompSegment> table(dw3_segments, dw3_segment_count);
    dw3::Machine m(table);
    m.load_segment_image(idx, image_of(kEmitted[idx]));
    InitState st = make_init(1);
    apply_init(m, st);
    int serviced = 0;
    m.on_service_events = [&](RecompContext& c) {
        ++serviced;
        c.next_event = INT64_MAX;
    };
    m.ctx().cycles = 0;
    m.ctx().next_event = 5;
    kEmitted[idx].func(&m.ctx());
    EXPECT_GE(serviced, 1);
    EXPECT_GT(m.ctx().cycles, 40);  // ~10 iterations of 4 instructions
}

TEST(EmittedRuntime, JumpTableDefaultIsNotReached) {
    // A table switch must never hit dw3_bad_jump for in-range selectors (covered by the differential
    // tests); here we only check that the fatal handler plumbing used by the harness works.
    dw3::set_fatal_handler([](const std::string& msg) { throw std::runtime_error(msg); });
    RecompContext ctx{};
    EXPECT_THROW(dw3_bad_jump(&ctx, 0x80010000u, 0x1234u), std::runtime_error);
    dw3::set_fatal_handler(nullptr);
}
