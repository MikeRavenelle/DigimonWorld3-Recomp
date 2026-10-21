#pragma once

// The recompiler as a library: load the executable and overlays named by a game config, analyse every segment and
// write the C sources. The command-line tool and the player-facing setup both use it.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "psxrecomp/analysis.h"
#include "psxrecomp/emitter.h"

namespace psxdisc {
class Disc;
}

namespace psxrecomp {

// Where the executable and overlays come from.
struct FileSource {
    std::function<std::vector<uint8_t>(const std::string& path)> read;  // empty when the file is missing
    std::function<std::vector<std::string>(const std::string& pattern)> expand;  // "dir/PRE*POST" (one '*' in the name)
};

FileSource directory_source(const std::filesystem::path& root);
// Reads straight from the disc image, including the hidden AAA tree. `disc` must outlive the source.
FileSource disc_source(psxdisc::Disc& disc);

struct Progress {
    std::function<void(const std::string& stage, std::size_t done, std::size_t total)> report;
    const std::atomic<bool>* cancel = nullptr;
    bool cancelled() const { return cancel && cancel->load(); }
};

struct SegmentJob {
    CodeSegment seg;
    AnalysisConfig cfg;
};

struct EmitStats {
    std::size_t functions = 0, jump_tables = 0, pointer_jumps = 0, unresolved_jumps = 0, functions_with_errors = 0;
    std::size_t files = 0, files_changed = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> sources;  // file names written, in sources.txt order
    std::vector<std::string> ambiguous_overlays;
    std::size_t stale_overlays = 0, stale_links = 0;
};

class Game {
public:
    // Reads the config's executable and overlays. False with `error` set on failure.
    bool load(const toml::table& config, const FileSource& source, std::string& error);
    // Analyses every segment. False if cancelled.
    bool analyze(const Progress& progress);
    // Writes the C sources and sources.txt into `out_dir`. Files whose text is unchanged are left alone. False if
    // cancelled or the directory cannot be written.
    bool emit(const std::filesystem::path& out_dir, const Progress& progress, EmitStats& stats, std::string& error);

    const std::vector<SegmentJob>& jobs() const { return jobs_; }
    const std::vector<AnalysisResult>& results() const { return results_; }
    EmitStats statistics() const;  // everything but the file counts

private:
    std::vector<SegmentJob> jobs_;
    std::vector<AnalysisResult> results_;
    std::vector<TableSegment> tables_;
};

bool write_if_changed(const std::filesystem::path& path, const std::string& text);

}  // namespace psxrecomp
