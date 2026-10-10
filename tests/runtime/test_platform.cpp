#include <gtest/gtest.h>

#include "dw3/platform.h"

using namespace dw3::platform;

TEST(Platform, NamesAreKnown) {
    EXPECT_NE(arch_name(host_arch()), "?");
    EXPECT_NE(os_name(host_os()), "?");
}

TEST(Platform, UserDataDirIsAbsolute) { EXPECT_TRUE(user_data_dir().is_absolute()); }
