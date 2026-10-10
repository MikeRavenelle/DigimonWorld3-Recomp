#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "psxdisc/disc.h"

namespace psxdisc {

struct DirRecord {
    std::string name;  // version suffix (";1") stripped
    uint32_t lba = 0;
    uint32_t size = 0;
    bool is_dir = false;
};

struct FileEntry {
    std::string path;  // '/'-separated, relative to the walk root
    uint32_t lba = 0;
    uint32_t size = 0;
};

// Parses the directory records contained in `data` (one or more 2048-byte sectors).
// Skips "." and ".." entries.
std::vector<DirRecord> parse_directory(std::span<const uint8_t> data);

// Returns the root directory record from the Primary Volume Descriptor (LBA 16).
std::optional<DirRecord> read_root(Disc& disc);

// Recursively lists files below a directory extent. `prefix` is prepended to every path.
// Works for directories not reachable from the root (DW3 hides its "AAA" tree at LBA 473).
std::vector<FileEntry> walk(Disc& disc, uint32_t dir_lba, uint32_t dir_size, const std::string& prefix = "");

}  // namespace psxdisc
