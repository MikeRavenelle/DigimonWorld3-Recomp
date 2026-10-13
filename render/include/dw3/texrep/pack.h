#pragma once

// Texture packs and image files.
//
// A pack is a directory holding manifest.toml and the PNG files it lists:
//
//   format = 1
//   name = "Real-ESRGAN general x4"
//   scale = 4                      # replacement pixels per original texel, 1..4
//   [[texture]]
//   block = "0123456789abcdef"     # content hash of the uploaded image (see blocks.h)
//   clut  = "fedcba9876543210"     # hash of the palette it was drawn with
//   width = 128                    # size of the original image in texels
//   height = 128
//   file  = "tex/0123456789abcdef_fedcba9876543210.png"   # (width * scale) x (height * scale), RGBA or RGB

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dw3::texrep {

constexpr int kPackFormat = 1;
constexpr int kMaxPackScale = 4;

struct PackEntry {
    uint64_t block = 0, clut = 0;
    int width = 0, height = 0;  // original texels
    std::string file;           // relative to the pack directory
};

struct PackManifest {
    std::string name;
    int scale = 1;
    std::vector<PackEntry> entries;
};

// Parses manifest text. Returns false and describes the first problem in `error`.
bool parse_pack_manifest(std::string_view text, PackManifest& out, std::string& error);
std::string write_pack_manifest(const PackManifest& m);

// A decoded image, RGBA8 (red in the lowest byte).
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> rgba;
};

bool read_png(const std::filesystem::path& path, Image& out, std::string& error);
bool decode_png(const uint8_t* data, std::size_t size, Image& out, std::string& error);
// Writes RGBA, or RGB (alpha dropped) with `with_alpha` false.
bool write_png(const std::filesystem::path& path, const Image& img, std::string& error, bool with_alpha = true);

class TexturePack {
public:
    // Loads manifest.toml from `dir`. Images are read on demand.
    static std::shared_ptr<TexturePack> load(const std::filesystem::path& dir, std::string& error);
    static std::shared_ptr<TexturePack> from_manifest(PackManifest m, std::filesystem::path dir);

    const std::string& name() const { return manifest_.name; }
    int scale() const { return manifest_.scale; }
    std::size_t size() const { return manifest_.entries.size(); }
    const std::filesystem::path& directory() const { return dir_; }
    // True if any entry is for this block, whatever its palette.
    bool has_block(uint64_t block) const { return blocks_.count(block) != 0; }
    const PackEntry* find(uint64_t block, uint64_t clut) const;
    // Reads and checks the entry's image (it must be exactly width*scale by height*scale).
    bool load_image(const PackEntry& e, Image& out, std::string& error) const;

private:
    PackManifest manifest_;
    std::filesystem::path dir_;
    std::unordered_map<uint64_t, std::vector<std::size_t>> blocks_;  // block hash -> entries
};

// Where replacement packs live: <user data dir>/texture-packs/<name>. Names may not contain path separators.
bool valid_pack_name(std::string_view name);

}  // namespace dw3::texrep
