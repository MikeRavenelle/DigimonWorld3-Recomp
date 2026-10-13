#pragma once

// Builds a texture dump (see dump.h) from the stage packs of the user's own game files instead of from a play
// session, and finds those files on a disc image or in an extracted copy of the disc.

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dw3/texrep/dump.h"
#include "dw3/texrep/stage_pack.h"
#include "psxdisc/disc.h"

namespace dw3::texrep {

// The stage pack files of the game. `path` is a .cue or .bin disc image, or a directory that holds the files
// (either the disc's root, with an AAA folder, or the folder that contains the Z_STAGE folder, or Z_STAGE itself).
class StageSource {
public:
    static std::unique_ptr<StageSource> open(const std::filesystem::path& path, std::string& error);

    // Map names such as "S226", in ascending order.
    const std::vector<std::string>& maps() const { return names_; }
    bool read(const std::string& map, std::vector<uint8_t>& out, std::string& error);

private:
    struct DiscFile {
        uint32_t lba = 0, size = 0;
    };
    std::optional<psxdisc::Disc> disc_;
    std::map<std::string, DiscFile> disc_files_;
    std::map<std::string, std::filesystem::path> dir_files_;
    std::vector<std::string> names_;
};

struct StageDumpStats {
    std::size_t maps = 0;
    std::size_t cells = 0;             // pictures seen, including repeats
    std::size_t images = 0;            // distinct (image, palette) pairs written
    std::size_t untracked = 0;         // pictures too small or too large for the renderer to track; not written
    std::size_t transparent = 0;       // pictures with nothing to draw; not written
};

// Accumulates maps and writes the dump directory: one component image per map (the cells put back together), one
// texture entry per distinct image and palette with the rectangles the game draws from it.
class StageDumpWriter {
public:
    explicit StageDumpWriter(std::filesystem::path dir) : dir_(std::move(dir)) {}
    bool add_map(const std::string& name, const StagePack& pack, std::string& error);
    bool finish(std::string& error);  // writes dump.toml
    const StageDumpStats& stats() const { return stats_; }

private:
    std::filesystem::path dir_;
    DumpManifest manifest_;
    std::map<std::pair<uint64_t, uint64_t>, bool> seen_;
    StageDumpStats stats_;
};

}  // namespace dw3::texrep
