#pragma once

// Memory card image format and a small filesystem layer over a raw 128 KiB image (as stored in .mcd files).
//
// Layout follows psx-spx "Memory Card Data Format": 1024 frames of 128 bytes; frame 0 is the "MC" header,
// frames 1-15 are the directory (one entry per 8 KiB block), 16-35 the broken-sector list, 36-55 the
// replacement data, 63 the write-test frame (a copy of frame 0). Blocks 1-15 (frames 64..1023) hold file data.
// Every system frame ends with a checksum byte: the XOR of the 127 bytes before it.
//
// Directory/chain handling follows psx-spx specification; cross-checked against PCSX-Redux and
// Beetle PSX / Mednafen (GPL-2+).

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dw3::memcard {

inline constexpr std::size_t kCardSize = 128 * 1024;
inline constexpr std::size_t kFrameSize = 128;
inline constexpr std::size_t kFrameCount = 1024;
inline constexpr std::size_t kBlockSize = 8192;
inline constexpr std::size_t kFramesPerBlock = 64;
inline constexpr int kDataBlocks = 15;  // blocks 1..15
inline constexpr std::size_t kMaxNameLen = 20;
inline constexpr uint16_t kNoNext = 0xFFFF;

// Directory entry states (first byte of the entry).
enum DirState : uint8_t {
    kStateFirst = 0x51, kStateMiddle = 0x52, kStateLast = 0x53,
    kStateFree = 0xA0, kStateDeletedFirst = 0xA1, kStateDeletedMiddle = 0xA2, kStateDeletedLast = 0xA3,
};

// Result codes; the numeric values are the BIOS errno values (_get_errno).
enum class Status : int {
    Ok = 0, NotFound = 0x02, Io = 0x10, Exists = 0x11, BadDevice = 0x13, BadArg = 0x16, NoSpace = 0x1C,
};

uint8_t frame_checksum(std::span<const uint8_t> frame_first_127);
// Recomputes byte 127 of a 128-byte frame.
void seal_frame(std::span<uint8_t> frame);
bool frame_checksum_ok(std::span<const uint8_t> frame);

// Writes a blank, correctly formatted card (all 15 blocks free).
void format_memory_card(std::span<uint8_t, kCardSize> image);
// True when frame 0 has the "MC" signature and checksum and every directory frame checksum is valid.
bool is_formatted(std::span<const uint8_t> image);

struct FileInfo {
    std::string name;
    uint32_t size = 0;           // bytes (blocks * 8192 for well-formed files)
    std::vector<uint8_t> blocks;  // chain of block numbers 1..15, in file order
};

// Non-owning view of a card image; every mutation keeps directory checksums valid.
class CardFs {
public:
    explicit CardFs(std::span<uint8_t> image) : img_(image) {}

    bool valid() const { return img_.size() == kCardSize && is_formatted(img_); }
    // Live files (first-block entries), in directory order.
    std::vector<FileInfo> list() const;
    std::optional<FileInfo> find(std::string_view name) const;
    int free_blocks() const;  // free or deleted blocks

    Status create(std::string_view name, int blocks);
    // Byte-offset access within a file, clamped to the file size. Returns the number of bytes transferred.
    std::size_t read(const FileInfo& f, std::size_t offset, std::span<uint8_t> out) const;
    std::size_t write(const FileInfo& f, std::size_t offset, std::span<const uint8_t> in);
    Status remove(std::string_view name);
    Status rename(std::string_view from, std::string_view to);

private:
    uint8_t* dir_frame(int block) const { return img_.data() + static_cast<std::size_t>(block) * kFrameSize; }
    std::span<uint8_t> frame_span(int block) const { return {dir_frame(block), kFrameSize}; }
    std::span<uint8_t> img_;
};

// Reads a .mcd (raw) or .gme (DexDrive 3904-byte header) / .vgs (64-byte header) file into a 128 KiB image.
std::optional<std::vector<uint8_t>> load_card_file(const std::filesystem::path& path);
// Writes a raw 128 KiB image atomically (temp file + rename).
bool save_card_file(const std::filesystem::path& path, std::span<const uint8_t> image);

}  // namespace dw3::memcard
