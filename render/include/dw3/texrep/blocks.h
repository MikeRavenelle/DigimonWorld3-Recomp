#pragma once

// Texture replacement, GPU-independent part: content hashes of the images the game uploads to VRAM, the registry
// that remembers which VRAM rectangles hold such an image, and the arithmetic that maps a sprite draw onto them.
//
// Field backgrounds are cut into blocks (up to 128x128 texels, 8-bit indexed) that the game uploads one by one with
// a CPU-to-VRAM transfer, each with its own 256-entry palette, and draws as textured rectangles that map texels to
// pixels one to one. A block is identified by what it contains, not where it sits, so a replacement keeps matching
// when the game places the same block somewhere else in VRAM on the next visit.

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dw3/vram_regions.h"

namespace dw3::texrep {

constexpr int kVramW = 1024, kVramH = 512;

// 64-bit hash of `n` 16-bit words.
uint64_t hash_words(const uint16_t* p, std::size_t n, uint64_t seed = 0);
// Hash of the words of a VRAM rectangle (dimensions included, so equal pixels in a different shape differ).
uint64_t hash_vram_rect(std::span<const uint16_t> vram, const Region& r);
// The same hash for an image kept outside VRAM: `w` words by `h` rows, `stride` words from one row to the next.
uint64_t hash_image_words(const uint16_t* rows, std::size_t stride, int w, int h);
// Hash of `count` palette entries kept outside VRAM; hash_clut of the same entries gives the same value.
uint64_t hash_palette(const uint16_t* entries, int count = 256);
// Hash of `count` palette entries starting at (x, y).
uint64_t hash_clut(std::span<const uint16_t> vram, int x, int y, int count = 256);

// Decodes a block of 8-bit indexed texels (two per VRAM word, low byte first) through the 256-entry palette at
// (clut_x, clut_y) to RGBA8 (red in the lowest byte). Colour channels are expanded from 5 to 8 bits the way the
// renderer does; a texel whose palette word is zero is transparent (alpha 0), everything else opaque.
void decode_block_8bit(std::span<const uint16_t> vram, const Region& block, int clut_x, int clut_y, std::vector<uint32_t>& out);

// The same decoding for an image kept outside VRAM (`width_words` words by `h` rows) and a 256-entry palette.
void decode_8bit_words(const uint16_t* rows, std::size_t stride, int width_words, int h, const uint16_t* clut, std::vector<uint32_t>& out);

std::string hash_to_hex(uint64_t h);
bool hash_from_hex(std::string_view text, uint64_t& out);

// A rectangle of VRAM (in 16-bit words) that holds one uploaded image.
struct Block {
    Region rect;          // VRAM words
    uint64_t hash = 0;    // valid when `hashed`
    bool hashed = false;
    int id = 0;           // unique over the registry's lifetime
    int width_texels() const { return rect.width() * 2; }  // 8-bit texels
    int height_texels() const { return rect.height(); }
};

class BlockRegistry {
public:
    static constexpr std::size_t kMaxBlocks = 512;
    // Uploads outside these bounds are not images worth tracking (palettes, full-screen copies, state loads).
    static constexpr int kMinWords = 8, kMaxWords = 128, kMinRows = 8, kMaxRows = 256;
    // Blocks made from a sprite rectangle (add_implicit) may be much smaller: the game cuts pictures into narrow strips.
    static constexpr int kMinImplicitWords = 2, kMinImplicitRows = 2;

    // A CPU-to-VRAM transfer completed. Blocks it overlaps are dropped; the region becomes a block if its size
    // qualifies.
    void add_upload(const Region& r);
    // Registers `r` as a block if it qualifies by size and overlaps no existing block (it is never allowed to drop
    // one). Used for images that were not seen being uploaded, such as those restored by a save state. Returns the
    // new block or null.
    Block* add_implicit(const Region& r);
    // Something other than an upload changed this region (a draw, fill or copy): blocks over it are dropped.
    void invalidate(const Region& r);
    void clear() { blocks_.clear(); }

    // The block that contains the VRAM word at (x, y), or null.
    Block* find(int x, int y);
    // The block's content hash, computed on first use from `vram`.
    uint64_t hash_of(Block& b, std::span<const uint16_t> vram) const;

    const std::list<Block>& blocks() const { return blocks_; }

private:
    std::list<Block> blocks_;  // a list: pieces keep pointers to blocks while new ones are added
    int next_id_ = 1;
};

// One piece of a sprite rectangle after splitting along block edges.
struct Piece {
    int u = 0, v = 0, w = 0, h = 0;  // texel coordinates in the texture page, size in pixels
    Block* block = nullptr;          // null: not inside any block
    int block_u0 = 0, block_v0 = 0;  // texel coordinates of the block's top-left corner in the texture page
};

// Splits a w x h sprite that samples texture page (page_x, page_y) (VRAM words / rows) from texel (u, v), 8-bit
// depth, into pieces that each lie inside a single block or in none. The pieces tile the rectangle exactly. The
// sprite must not wrap the page (u + w <= 256, v + h <= 256); otherwise one piece without a block is returned.
// With `implicit_blocks`, a part of the sprite that lies in no block becomes a block of its own (see add_implicit).
std::vector<Piece> split_by_blocks(BlockRegistry& registry, int page_x, int page_y, int u, int v, int w, int h,
                                   bool implicit_blocks = false);

// Where a replacement texture of a block sits relative to the sprite's own coordinates. A texel position t (in the
// texture page's texel units, continuous) of a piece maps to the replacement texel position
// (t - block_origin) * scale, i.e. t * scale + offset.
struct ReplacementOffset {
    int x = 0, y = 0;
};
ReplacementOffset replacement_offset(int block_u0, int block_v0, int atlas_x, int atlas_y, int scale);

// Shelf packer for the replacement atlas.
class ShelfAtlas {
public:
    ShelfAtlas(int width, int height) : w_(width), h_(height) {}
    // Position of a w x h rectangle, or nothing if it does not fit.
    std::optional<std::pair<int, int>> alloc(int w, int h);
    void reset();
    int width() const { return w_; }
    int height() const { return h_; }

private:
    struct Shelf {
        int y = 0, h = 0, used = 0;
    };
    int w_, h_;
    std::vector<Shelf> shelves_;
    int next_y_ = 0;
};

}  // namespace dw3::texrep
