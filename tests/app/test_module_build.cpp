// Preparing the game from the disc: build identity, toolchain lookup and the sequence of compiler runs (with a fake
// compiler). The cases that read the disc are skipped without it.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <mutex>

#include "module_build.h"

using namespace dw3setup;
namespace fs = std::filesystem;
using dw3::platform::Arch;
using dw3::platform::Os;

namespace {

fs::path scratch(const char* name) {
    fs::path p = fs::temp_directory_path() / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
}

std::string value_after(const std::vector<std::string>& command, const std::string& flag) {
    for (std::size_t i = 0; i + 1 < command.size(); ++i)
        if (command[i] == flag) return command[i + 1];
    return {};
}

struct FakeCompiler {
    std::mutex mutex;
    std::vector<std::vector<std::string>> commands;
    int fail_on_object = -1;  // fail the nth compile
    int compiles = 0;
    int operator()(const std::vector<std::string>& command, const std::atomic<bool>&) {
        std::lock_guard<std::mutex> lock(mutex);
        commands.push_back(command);
        const std::string output = value_after(command, "-o");
        if (value_after(command, "-c").empty()) {  // the link
            touch(output);
            return 0;
        }
        if (compiles++ == fail_on_object) return 1;
        touch(output);
        return 0;
    }
};

PrepareConfig config_for(const fs::path& user_data, const fs::path& disc, FakeCompiler& fake) {
    PrepareConfig c;
    c.user_data = user_data;
    c.disc = disc;
    c.toolchain = {"/tool/zig", "/tool/include", "/tool/module/module_entry.c"};
    c.os = Os::Linux;
    c.arch = Arch::X86_64;
    c.build_id = "abc123";
    c.jobs = 4;
    c.run = [&fake](const std::vector<std::string>& cmd, const std::atomic<bool>& cancel) { return fake(cmd, cancel); };
    return c;
}

fs::path real_disc() { return fs::path(DW3_GAME_DIR) / "Digimon World 3 (USA).cue"; }

std::string game_config() {
    std::ifstream f(fs::path(DW3_SOURCE_DIR) / "config" / "dw3.toml");
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(ModuleBuild, TargetsAndNames) {
    EXPECT_EQ(compile_target(Os::Linux, Arch::X86_64), "x86_64-linux-gnu");
    EXPECT_EQ(compile_target(Os::Linux, Arch::Arm64), "aarch64-linux-gnu");
    EXPECT_EQ(compile_target(Os::Windows, Arch::X86_64), "x86_64-windows-gnu");
    EXPECT_EQ(compile_target(Os::MacOS, Arch::Arm64), "aarch64-macos");
    EXPECT_EQ(module_extension(Os::Linux), "so");
    EXPECT_EQ(module_extension(Os::Windows), "dll");
    EXPECT_EQ(module_extension(Os::MacOS), "dylib");
    EXPECT_EQ(module_path("/data", "abc", Os::Linux), fs::path("/data/module/dw3game-abc.so"));
}

TEST(ModuleBuild, BuildIdChangesWithEachInput) {
    const std::string base = make_build_id("0.1.0", "cfg", "exe");
    EXPECT_EQ(base.size(), 16u);
    EXPECT_EQ(base, make_build_id("0.1.0", "cfg", "exe"));
    EXPECT_NE(base, make_build_id("0.1.1", "cfg", "exe"));
    EXPECT_NE(base, make_build_id("0.1.0", "cfg2", "exe"));
    EXPECT_NE(base, make_build_id("0.1.0", "cfg", "exe2"));
}

TEST(ModuleBuild, FindsAnIntactToolchainOnly) {
    const fs::path app = scratch("dw3_toolchain_app");
    EXPECT_FALSE(find_toolchain(app));
    touch(app / "toolchain" / "include" / "dw3" / "recomp.h");
    touch(app / "toolchain" / "module" / "module_entry.c");
    EXPECT_FALSE(find_toolchain(app));  // no compiler yet
#if defined(DW3_OS_windows)
    touch(app / "toolchain" / "zig.exe");
#else
    touch(app / "toolchain" / "zig");
#endif
    auto t = find_toolchain(app);
    ASSERT_TRUE(t);
    EXPECT_EQ(t->module_entry, app / "toolchain" / "module" / "module_entry.c");
    fs::remove_all(app);
}

TEST(ModuleBuild, UnreadableDiscFails) {
    FakeCompiler fake;
    const fs::path data = scratch("dw3_prepare_nodisc");
    ModulePreparer preparer(config_for(data, data / "missing.cue", fake));
    const PrepareResult r = preparer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("disc"), std::string::npos);
    EXPECT_TRUE(fake.commands.empty());
    EXPECT_EQ(preparer.snapshot().phase, PreparePhase::Failed);
    fs::remove_all(data);
}

TEST(ModuleBuild, CompilesEverySourceThenLinks) {
    if (!fs::exists(real_disc())) GTEST_SKIP() << "needs the disc";
    FakeCompiler fake;
    const fs::path data = scratch("dw3_prepare_full");
    PrepareConfig config = config_for(data, real_disc(), fake);
    config.game_config = game_config();
    ModulePreparer preparer(config);
    const PrepareResult r = preparer.run();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.module, data / "module" / "dw3game-abc123.so");
    EXPECT_TRUE(fs::exists(r.module));
    EXPECT_FALSE(fs::exists(data / "module" / "build"));  // temporary files removed

    ASSERT_GT(fake.commands.size(), 500u);
    const auto& first = fake.commands.front();
    EXPECT_EQ(first[0], "/tool/zig");
    EXPECT_EQ(first[1], "cc");
    EXPECT_EQ(value_after(first, "-target"), "x86_64-linux-gnu");
    EXPECT_EQ(value_after(first, "-I"), "/tool/include");
    const auto& link = fake.commands.back();
    EXPECT_EQ(value_after(link, "-target"), "x86_64-linux-gnu");
    EXPECT_NE(std::find(link.begin(), link.end(), "-shared"), link.end());
    // The module entry point was compiled.
    bool entry = false;
    for (const auto& c : fake.commands) entry |= value_after(c, "-c") == "/tool/module/module_entry.c";
    EXPECT_TRUE(entry);

    // A second run finds the module and does nothing.
    const std::size_t before = fake.commands.size();
    ModulePreparer again(config);
    EXPECT_TRUE(again.run().ok);
    EXPECT_EQ(fake.commands.size(), before);
    fs::remove_all(data);
}

TEST(ModuleBuild, ACompilerFailureStopsTheBuild) {
    if (!fs::exists(real_disc())) GTEST_SKIP() << "needs the disc";
    FakeCompiler fake;
    fake.fail_on_object = 10;
    const fs::path data = scratch("dw3_prepare_fail");
    PrepareConfig config = config_for(data, real_disc(), fake);
    config.game_config = game_config();
    ModulePreparer preparer(config);
    const PrepareResult r = preparer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("compiler failed"), std::string::npos) << r.error;
    EXPECT_FALSE(fs::exists(data / "module" / "dw3game-abc123.so"));
    fs::remove_all(data);
}

TEST(ModuleBuild, CancelBeforeStartReportsCancelled) {
    if (!fs::exists(real_disc())) GTEST_SKIP() << "needs the disc";
    FakeCompiler fake;
    const fs::path data = scratch("dw3_prepare_cancel");
    PrepareConfig config = config_for(data, real_disc(), fake);
    config.game_config = game_config();
    ModulePreparer preparer(config);
    preparer.cancel();
    const PrepareResult r = preparer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cancelled);
    EXPECT_TRUE(fake.commands.empty());
    fs::remove_all(data);
}
