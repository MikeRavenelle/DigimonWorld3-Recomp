#pragma once

// Texture dumping: records the background images the game draws and writes them to a directory from which a
// texture pack can be generated (see tools/texpack).
//
// While the game runs, each sprite draw that lands inside a tracked block reports the block, the palette it used
// and where the block's top-left corner is on screen. Two blocks that are drawn in the same frame always have the
// same relative position when they are part of one picture, so those relative positions are enough to put blocks
// that touch back together without knowing anything about how the game lays out its maps. A group of blocks that
// are placed consistently and never overlap is written as one "component" image; the generator upscales blocks
// with their neighbours around them so the seams between blocks stay coherent.
//
// Directory layout:
//   dump.toml                        format, components and textures
//   textures/<block>_<clut>.png      one decoded image per block and palette
//   components/c<N>.png              the blocks of a component pasted together (most used palette of each)

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dw3/texrep/pack.h"

namespace dw3::texrep {

constexpr int kDumpFormat = 1;

struct BlockSize {
    uint64_t hash = 0;
    int w = 0, h = 0;
};
// Observed position of block b's origin relative to block a's.
struct PairOffset {
    uint64_t a = 0, b = 0;
    int dx = 0, dy = 0;
    int count = 0;
};
struct PlacedBlock {
    uint64_t block = 0;
    int w = 0, h = 0;
    int x = 0, y = 0;  // top-left corner in the component, with the component's own top-left at (0, 0)
};
struct Component {
    std::vector<PlacedBlock> blocks;
    int width = 0, height = 0;
};

// Groups blocks into components from pairwise offsets, strongest evidence first. A pair that contradicts an
// earlier one, or that would make two blocks overlap, is ignored. Every block ends up in exactly one component.
std::vector<Component> build_components(std::span<const BlockSize> blocks, std::span<const PairOffset> pairs);

struct DumpTexture {
    uint64_t block = 0, clut = 0;
    int width = 0, height = 0;
    std::string file;
    uint32_t uses = 0;
    int component = 0;
    int x = 0, y = 0;     // position in the component
    bool primary = false; // the palette used to draw the component image
};
struct DumpComponent {
    int id = 0, width = 0, height = 0;
    std::string file;
};
struct DumpManifest {
    std::vector<DumpComponent> components;
    std::vector<DumpTexture> textures;
};
bool parse_dump_manifest(std::string_view text, DumpManifest& out, std::string& error);
std::string write_dump_manifest(const DumpManifest& m);

class TextureDump {
public:
    // Blocks that were drawn fewer times than this are left out (transient uploads such as animated sprites).
    explicit TextureDump(uint32_t min_uses = 30) : min_uses_(min_uses) {}

    bool has_variant(uint64_t block, uint64_t clut) const { return variants_.count({block, clut}) != 0; }
    // The decoded image (RGBA8, transparent texels with alpha 0) of a block drawn with a palette.
    void add_variant(uint64_t block, uint64_t clut, int w, int h, std::vector<uint32_t> rgba);
    // A draw of the block with the palette; (ox, oy) is where the block's top-left corner lies in the game's own
    // screen coordinates.
    void observe(uint64_t block, uint64_t clut, int ox, int oy);
    void end_frame();

    std::size_t texture_count() const { return variants_.size(); }
    // Writes everything seen so far to `dir`. Returns false and sets `error` on I/O failure.
    bool write(const std::filesystem::path& dir, std::string& error);

private:
    struct Variant {
        int w = 0, h = 0;
        std::vector<uint32_t> rgba;
        uint32_t uses = 0;
    };
    struct FrameDraw {
        uint64_t block;
        int ox, oy;
        bool conflict;
    };
    uint32_t min_uses_;
    std::map<std::pair<uint64_t, uint64_t>, Variant> variants_;
    std::vector<FrameDraw> frame_;
    // (a, b) with a < b -> (dx, dy) -> frames; a pair seen with two different offsets is dropped (parallax layers).
    std::map<std::pair<uint64_t, uint64_t>, std::map<std::pair<int, int>, int>> pairs_;
};

}  // namespace dw3::texrep
