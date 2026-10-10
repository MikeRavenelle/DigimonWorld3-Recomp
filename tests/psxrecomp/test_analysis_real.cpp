// Function discovery on the real SLUS_014.36 (needs game/extracted from dw3extract; skipped otherwise).

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "psxdisc/psxexe.h"
#include "psxrecomp/analysis.h"

using namespace psxrecomp;

namespace {

constexpr uint32_t kCodeEnd = 0x8005C480;  // BSS start

std::optional<CodeSegment> load_main() {
    auto path = std::filesystem::path(DW3_GAME_DIR) / "extracted" / "SLUS_014.36";
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    auto exe = psxdisc::parse_psxexe(bytes);
    if (!exe) return std::nullopt;
    return CodeSegment{"MAIN", exe->text_addr, exe->text};
}

class RealMain : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        seg_ = load_main();
        if (!seg_) return;
        AnalysisConfig c;
        c.entries = {0x80010EC4};
        c.code_end = kCodeEnd;
        result_ = analyze(*seg_, c);
    }
    void SetUp() override {
        if (!seg_) GTEST_SKIP() << "game/extracted/SLUS_014.36 not present (run dw3extract)";
    }
    static inline std::optional<CodeSegment> seg_;
    static inline AnalysisResult result_;
};

}  // namespace

TEST_F(RealMain, FindsEntryAndMain) {
    EXPECT_TRUE(result_.functions.count(0x80010EC4));
    EXPECT_TRUE(result_.functions.count(0x800144FC));  // main(), called from crt0
}

TEST_F(RealMain, NoErrorsOrOverlaps) {
    for (const auto& [s, f] : result_.functions) EXPECT_TRUE(f.errors.empty()) << std::hex << s;
    EXPECT_TRUE(result_.warnings.empty());
}

TEST_F(RealMain, ThirtySixBiosStubs) {
    int n = 0;
    for (const auto& [s, f] : result_.functions) n += f.kind == FuncKind::BiosStub;
    EXPECT_EQ(n, 36);
}

TEST_F(RealMain, EveryReturnIsCovered) {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const auto& [s, f] : result_.functions) ranges.emplace_back(f.start, f.end);
    int uncovered = 0;
    for (uint32_t a = seg_->base; a < kCodeEnd; a += 4) {
        if (seg_->word(a) != 0x03E00008u) continue;
        auto it = std::upper_bound(ranges.begin(), ranges.end(), std::make_pair(a, UINT32_MAX));
        if (it == ranges.begin() || a >= std::prev(it)->second) ++uncovered;
    }
    EXPECT_EQ(uncovered, 0);
}

TEST_F(RealMain, FunctionCountIsStable) {
    // Regression guard; update deliberately when discovery improves.
    EXPECT_EQ(result_.functions.size(), 915u);
}

TEST_F(RealMain, NoFunctionStartsInOverlaySlots) {
    for (const auto& [s, f] : result_.functions) EXPECT_LT(s, kCodeEnd);
}
