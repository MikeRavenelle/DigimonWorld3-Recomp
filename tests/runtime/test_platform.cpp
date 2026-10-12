#include <gtest/gtest.h>

#include "dw3/platform.h"

using namespace dw3::platform;

TEST(Platform, NamesAreKnown) {
    EXPECT_NE(arch_name(host_arch()), "?");
    EXPECT_NE(os_name(host_os()), "?");
}

TEST(Platform, UserDataDirIsAbsolute) { EXPECT_TRUE(user_data_dir().is_absolute()); }

TEST(Platform, Utf8PathRoundTrip) {
    const std::string name = "Digimon World 3 \xC3\xA9\xE3\x83\x87\xE3\x82\xB8\xE3\x83\xA2\xE3\x83\xB3 (USA).cue";
    EXPECT_EQ(path_to_utf8(path_from_utf8(name)), name);
    EXPECT_EQ(path_to_utf8(path_from_utf8("a b/c d").filename()), "c d");
}

TEST(Platform, CommandLineKeepsArguments) {
    char a0[] = "prog";
    char* argv[] = {a0};
    EXPECT_FALSE(command_line_utf8(1, argv).empty());
}
