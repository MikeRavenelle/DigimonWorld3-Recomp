#pragma once

// Field map backgrounds as the game stores them, and their conversion into texture dumps.
//
// Every field map has a "stage pack" file (Z_STAGE/S<nnn>PACK.BIN). It describes the map as a grid of 128x128 pixel
// cells. Each cell has one 8-bit indexed picture with its own 256-entry palette, in the PlayStation TIM layout, and a
// list of pieces: rectangles of that picture and the place in the cell where each is drawn. The game uploads the
// picture to VRAM unchanged and draws the pieces as textured rectangles, so the uploaded words and the palette are
// exactly what the renderer hashes (see blocks.h), and the same keys can be computed from the file without running
// the game.
//
// File layout (all values little endian):
//   u32 count         number of cells that hold a picture
//   u32 columns, rows size of the cell grid
//   u32 slot_size     bytes reserved for each cell (a multiple of 2048)
//   u16 used[columns * rows]   non-zero when the cell holds a picture
//   padding up to 2048
//   slots, `slot_size` bytes each, in row-major order starting at offset 2048
// A slot is either an "RLEN" stream or the cell data itself (see rlen_decode). The cell data is
//   u32 header_size   byte offset of the picture; the header is 16 + 12 * pieces bytes
//   three groups, each: u32 n, then n pieces of 12 bytes:
//       u16 x, y      where the piece is drawn, relative to the cell's top-left corner
//       u16 ax, ay    its top-left corner in the picture
//       u16 w, h      its size
//   then a TIM with an 8-bit picture and a 256-entry palette (flags 9).
// The groups are drawing layers; later groups lie over earlier ones.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "dw3/texrep/pack.h"

namespace dw3::texrep {

constexpr int kStageCell = 128;

// Decodes a run-length stream: the text "RLEN", the u32 decoded size, then control bytes: 0 ends the stream, 1..127
// copies that many literal bytes, 128 + n repeats the next byte n times. Returns false when the data is malformed.
bool rlen_decode(std::span<const uint8_t> data, std::vector<uint8_t>& out);
bool is_rlen(std::span<const uint8_t> data);

struct StagePiece {
    int layer = 0;
    int x = 0, y = 0;    // in the cell
    int ax = 0, ay = 0;  // in the picture, texels
    int w = 0, h = 0;
};

struct StageTile {
    int col = 0, row = 0;
    int width_words = 0, rows = 0;  // picture size: width in 16-bit words (two texels each)
    std::vector<uint16_t> words;    // width_words * rows
    std::array<uint16_t, 256> clut{};
    std::vector<StagePiece> pieces;
    uint64_t block_hash = 0, clut_hash = 0;  // the keys the renderer computes for this picture
    int width_texels() const { return width_words * 2; }
    // Whether the renderer tracks an upload of this size as an image (see BlockRegistry::add_upload). Pictures
    // outside those limits are not replaceable by key.
    bool tracked() const;
};

struct StagePack {
    int cols = 0, rows = 0;
    std::vector<StageTile> tiles;
};

bool parse_stage_pack(std::span<const uint8_t> file, StagePack& out, std::string& error);

// The picture of a tile, RGBA8; palette entry zero is transparent.
Image decode_stage_tile(const StageTile& tile);
// The whole map, cols * 128 by rows * 128 pixels: the pieces of every cell drawn in layer order.
Image compose_stage_map(const StagePack& pack);

}  // namespace dw3::texrep
