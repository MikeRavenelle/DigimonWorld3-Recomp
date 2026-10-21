// Loading a game module: its segment table, the runtime services it calls back, and the failure cases.

#include <gtest/gtest.h>

#include "dw3/game_module.h"
#include "dw3/machine.h"

namespace {
const std::filesystem::path kFixture = DW3_FIXTURE_MODULE;
}

TEST(GameModule, LoadsSegmentsAndBuildId) {
    std::string error;
    auto module = dw3::GameModule::load(kFixture, "", error);
    ASSERT_TRUE(module) << error;
    ASSERT_EQ(module->segments().size(), 1u);
    EXPECT_STREQ(module->segments()[0].name, "FIX");
    EXPECT_EQ(module->build_id(), "fixture");
}

TEST(GameModule, ModuleCodeCallsBackIntoTheRuntime) {
    std::string error;
    auto module = dw3::GameModule::load(kFixture, "fixture", error);
    ASSERT_TRUE(module) << error;
    dw3::Machine machine(module->segments());
    RecompFunc func = machine.lookup(0x80010000u);
    ASSERT_NE(func, nullptr);
    func(&machine.ctx());
    EXPECT_EQ(machine.ctx().r[2], 5u);  // written and read back through the module's runtime services
}

TEST(GameModule, RejectsOtherBuild) {
    std::string error;
    EXPECT_FALSE(dw3::GameModule::load(kFixture, "another-build", error));
    EXPECT_NE(error.find("other recompiler output"), std::string::npos) << error;
}

TEST(GameModule, RejectsMissingFile) {
    std::string error;
    EXPECT_FALSE(dw3::GameModule::load(kFixture.parent_path() / "no_such_module", "", error));
    EXPECT_FALSE(error.empty());
}
