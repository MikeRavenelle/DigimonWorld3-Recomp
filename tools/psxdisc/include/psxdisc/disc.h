#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psxdisc {

constexpr std::size_t kRawSectorSize = 2352;
constexpr std::size_t kForm1DataSize = 2048;
constexpr std::size_t kForm2DataSize = 2324;

// CD-XA subheader submode bits.
enum Submode : uint8_t {
    kSubmodeEOR      = 0x01,
    kSubmodeVideo    = 0x02,
    kSubmodeAudio    = 0x04,
    kSubmodeData     = 0x08,
    kSubmodeTrigger  = 0x10,
    kSubmodeForm2    = 0x20,
    kSubmodeRealtime = 0x40,
    kSubmodeEOF      = 0x80,
};

struct SectorHeader {
    uint8_t minute, second, frame, mode;     // BCD MSF + mode
    uint8_t file, channel, submode, coding;  // XA subheader (first copy)
};

struct CueTrack {
    int number = 0;
    std::string mode;  // e.g. "MODE2/2352"
    uint32_t index1_lba = 0;
};

struct CueSheet {
    std::string bin_file;
    std::vector<CueTrack> tracks;
};

// Parse a cue sheet's text. Only single-FILE cues are supported (all PS1 single-track games).
std::optional<CueSheet> parse_cue(const std::string& text);

// Converts between BCD MSF and LBA (LBA 0 == 00:02:00).
uint32_t msf_to_lba(uint8_t m, uint8_t s, uint8_t f);
void lba_to_msf(uint32_t lba, uint8_t& m, uint8_t& s, uint8_t& f);
inline uint8_t to_bcd(uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
inline uint8_t from_bcd(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 0xF)); }

// Random-access reader for a raw 2352-byte/sector MODE2 image.
class Disc {
public:
    // Opens a .cue (resolving its FILE) or a raw .bin directly.
    static std::optional<Disc> open(const std::filesystem::path& path);

    uint32_t sector_count() const { return sector_count_; }

    // Full 2352-byte raw sector. Returns false if out of range.
    bool read_raw(uint32_t lba, std::span<uint8_t, kRawSectorSize> out);
    SectorHeader read_header(uint32_t lba);
    // 2048-byte Form 1 user data.
    bool read_form1(uint32_t lba, std::span<uint8_t, kForm1DataSize> out);
    // Reads `size` bytes of Form 1 user data starting at `lba`.
    std::vector<uint8_t> read_file(uint32_t lba, uint32_t size);

private:
    explicit Disc(std::ifstream&& f, uint32_t sectors) : file_(std::move(f)), sector_count_(sectors) {}
    std::ifstream file_;
    uint32_t sector_count_ = 0;
};

}  // namespace psxdisc
