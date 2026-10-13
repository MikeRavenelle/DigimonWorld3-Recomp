// Recognises the supported disc: Digimon World 3, USA release (serial SLUS-01436), by the boot file named in
// SYSTEM.CNF and a SHA-256 of that executable.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace dw3setup {

struct ExpectedDisc {
    std::string serial;      // "SLUS-01436"
    std::string exe_sha256;  // of the boot executable file
};

const ExpectedDisc& retail_usa();

enum class DiscStatus {
    Ok,
    NotFound,
    UnsupportedFormat,  // not a raw 2352-byte-per-sector image (or a cue sheet that points at one)
    NotPlayStation,     // no ISO 9660 volume, SYSTEM.CNF or boot executable
    WrongRegion,        // a PlayStation game of another region
    WrongGame,          // another USA game
    ModifiedExecutable  // right serial, but the executable differs from the retail one
};

struct DiscCheck {
    DiscStatus status = DiscStatus::NotFound;
    std::string serial;   // as read from the disc, when it could be
    std::string message;  // for the player; empty on success
    bool ok() const { return status == DiscStatus::Ok; }
};

DiscCheck check_disc(const std::filesystem::path& image, const ExpectedDisc& expected = retail_usa());

// "cdrom:\SLUS_014.36;1" from a SYSTEM.CNF text -> "SLUS-01436"; empty if there is no BOOT line.
std::string serial_from_system_cnf(std::string_view text);
// The boot file name of the same line: "SLUS_014.36".
std::string boot_file_from_system_cnf(std::string_view text);
// "USA", "Europe", "Japan" from the serial's prefix; empty when unknown.
std::string_view region_of_serial(std::string_view serial);

}  // namespace dw3setup
