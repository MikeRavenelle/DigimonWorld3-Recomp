#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "psxdisc/psxexe.h"

using namespace psxdisc;

namespace {
std::vector<uint8_t> make_exe(uint32_t text_size) {
    std::vector<uint8_t> f(kPsxExeHeaderSize + text_size, 0);
    std::memcpy(f.data(), "PS-X EXE", 8);
    auto put = [&](std::size_t off, uint32_t v) { std::memcpy(f.data() + off, &v, 4); };
    put(0x10, 0x80010EC4);
    put(0x18, 0x80010000);
    put(0x1C, text_size);
    put(0x30, 0x801FFFF0);
    for (uint32_t i = 0; i < text_size; ++i) f[kPsxExeHeaderSize + i] = static_cast<uint8_t>(i);
    return f;
}
}  // namespace

TEST(PsxExe, ParsesHeader) {
    auto exe = parse_psxexe(make_exe(0x100));
    ASSERT_TRUE(exe);
    EXPECT_EQ(exe->pc, 0x80010EC4u);
    EXPECT_EQ(exe->text_addr, 0x80010000u);
    EXPECT_EQ(exe->text_size, 0x100u);
    EXPECT_EQ(exe->sp_base, 0x801FFFF0u);
    ASSERT_EQ(exe->text.size(), 0x100u);
    EXPECT_EQ(exe->text[0x42], 0x42);
}

TEST(PsxExe, RejectsBadMagic) {
    auto f = make_exe(16);
    f[0] = 'X';
    EXPECT_FALSE(parse_psxexe(f));
}

TEST(PsxExe, RejectsTruncatedText) {
    auto f = make_exe(0x100);
    f.resize(kPsxExeHeaderSize + 0x80);
    EXPECT_FALSE(parse_psxexe(f));
}

TEST(PsxExe, RejectsShortHeader) { EXPECT_FALSE(parse_psxexe(std::vector<uint8_t>(100, 0))); }
