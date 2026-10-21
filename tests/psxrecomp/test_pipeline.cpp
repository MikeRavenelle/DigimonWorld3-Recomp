// The recompiler pipeline: reading from the disc must give the same segments as reading the extracted files, and the
// glob expansion must pick the right names. The disc cases are skipped without the game.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "psxdisc/disc.h"
#include "psxrecomp/pipeline.h"

using namespace psxrecomp;
namespace fs = std::filesystem;

namespace {

TEST(Pipeline, DirectorySourceExpandsOneStar) {
    const fs::path dir = fs::temp_directory_path() / "dw3_pipeline_glob";
    fs::remove_all(dir);
    fs::create_directories(dir / "AAA" / "PRO");
    for (const char* n : {"WSTAG01.PRO", "WSTAG02.PRO", "OTHER.PRO", "WSTAG03.BIN"}) std::ofstream(dir / "AAA" / "PRO" / n) << "x";
    auto source = directory_source(dir);
    const auto names = source.expand("AAA/PRO/WSTAG*.PRO");
    EXPECT_EQ(names, (std::vector<std::string>{"AAA/PRO/WSTAG01.PRO", "AAA/PRO/WSTAG02.PRO"}));
    EXPECT_EQ(source.expand("AAA/PRO/OTHER.PRO"), (std::vector<std::string>{"AAA/PRO/OTHER.PRO"}));
    EXPECT_TRUE(source.read("AAA/PRO/MISSING.PRO").empty());
    fs::remove_all(dir);
}

TEST(Pipeline, DiscSourceMatchesExtractedFiles) {
    const fs::path game = fs::path(DW3_GAME_DIR);
    auto disc = psxdisc::Disc::open(game / "Digimon World 3 (USA).cue");
    if (!disc || !fs::exists(game / "extracted" / "SLUS_014.36")) GTEST_SKIP() << "needs the disc and game/extracted";
    toml::table config;
    try {
        config = toml::parse_file((fs::path(DW3_SOURCE_DIR) / "config" / "dw3.toml").string());
    } catch (const toml::parse_error& e) {
        FAIL() << e.what();
    }
    Game from_disc, from_files;
    std::string error;
    ASSERT_TRUE(from_disc.load(config, disc_source(*disc), error)) << error;
    ASSERT_TRUE(from_files.load(config, directory_source(game / "extracted"), error)) << error;
    ASSERT_EQ(from_disc.jobs().size(), from_files.jobs().size());
    for (std::size_t i = 0; i < from_disc.jobs().size(); ++i) {
        EXPECT_EQ(from_disc.jobs()[i].seg.name, from_files.jobs()[i].seg.name);
        EXPECT_EQ(from_disc.jobs()[i].seg.base, from_files.jobs()[i].seg.base);
        EXPECT_EQ(from_disc.jobs()[i].seg.bytes, from_files.jobs()[i].seg.bytes) << from_disc.jobs()[i].seg.name;
    }
}

TEST(Pipeline, ReportsAMissingOverlay) {
    toml::table config = toml::parse(R"(
[exe]
path = "EXE"
[[overlay_slot]]
base = 0x80082448
files = ["AAA/PRO/NOPE.PRO"]
)");
    FileSource source;
    source.read = [](const std::string&) { return std::vector<uint8_t>{}; };
    source.expand = [](const std::string& p) { return std::vector<std::string>{p}; };
    Game game;
    std::string error;
    EXPECT_FALSE(game.load(config, source, error));
    EXPECT_FALSE(error.empty());
}

}  // namespace
