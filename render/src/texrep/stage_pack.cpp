#include "dw3/texrep/stage_pack.h"

#include <algorithm>
#include <cstring>

#include "dw3/texrep/blocks.h"
#include "dw3/texrep/window.h"

namespace dw3::texrep {

namespace {

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

constexpr std::size_t kSector = 2048;

}  // namespace

bool is_rlen(std::span<const uint8_t> data) { return data.size() >= 8 && std::memcmp(data.data(), "RLEN", 4) == 0; }

bool rlen_decode(std::span<const uint8_t> data, std::vector<uint8_t>& out) {
    out.clear();
    if (!is_rlen(data)) return false;
    const uint32_t size = rd32(data.data() + 4);
    if (size > (1u << 26)) return false;
    out.reserve(size);
    std::size_t i = 8;
    while (i < data.size() && out.size() < size) {
        const uint8_t control = data[i++];
        if (control == 0) break;
        if (control & 0x80) {
            if (i >= data.size()) return false;
            out.insert(out.end(), control & 0x7F, data[i++]);
        } else {
            if (i + control > data.size()) return false;
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(i), data.begin() + static_cast<std::ptrdiff_t>(i + control));
            i += control;
        }
    }
    if (out.size() < size) return false;
    out.resize(size);
    return true;
}

bool StageTile::tracked() const {
    return width_words >= BlockRegistry::kMinWords && width_words <= BlockRegistry::kMaxWords &&
           rows >= BlockRegistry::kMinRows && rows <= BlockRegistry::kMaxRows;
}

namespace {

bool parse_cell(std::span<const uint8_t> cell, StageTile& tile, std::string& error) {
    auto fail = [&](const char* what) {
        error = what;
        return false;
    };
    if (cell.size() < 16) return fail("cell data too short");
    const std::size_t header = rd32(cell.data());
    if (header < 16 || (header - 16) % 12 != 0 || header + 8 > cell.size()) return fail("bad cell header size");
    std::size_t p = 4;
    for (int layer = 0; layer < 3; ++layer) {
        if (p + 4 > header) return fail("cell header too short");
        const uint32_t n = rd32(cell.data() + p);
        p += 4;
        if (n > (header - p) / 12) return fail("cell header too short");
        for (uint32_t k = 0; k < n; ++k, p += 12) {
            StagePiece piece;
            piece.layer = layer;
            piece.x = rd16(cell.data() + p);
            piece.y = rd16(cell.data() + p + 2);
            piece.ax = rd16(cell.data() + p + 4);
            piece.ay = rd16(cell.data() + p + 6);
            piece.w = rd16(cell.data() + p + 8);
            piece.h = rd16(cell.data() + p + 10);
            tile.pieces.push_back(piece);
        }
    }
    if (p != header) return fail("cell header size does not match its pieces");

    // The TIM: id 0x10, flags 9 (8 bits per texel, with a palette), palette block, image block.
    const uint8_t* tim = cell.data() + header;
    const std::size_t avail = cell.size() - header;
    if (avail < 8 + 12 || rd32(tim) != 0x10 || rd32(tim + 4) != 9) return fail("cell picture is not an 8-bit TIM");
    const std::size_t clut_bytes = rd32(tim + 8);
    if (clut_bytes < 12 + 512 || 8 + clut_bytes + 12 > avail) return fail("bad palette block");
    if (rd16(tim + 8 + 8) != 256) return fail("palette is not 256 entries wide");
    for (int i = 0; i < 256; ++i) tile.clut[static_cast<std::size_t>(i)] = rd16(tim + 8 + 12 + 2 * i);
    const uint8_t* image = tim + 8 + clut_bytes;
    const int w = rd16(image + 8), h = rd16(image + 10);
    const std::size_t pixel_bytes = static_cast<std::size_t>(w) * h * 2;
    if (w <= 0 || h <= 0 || 8 + clut_bytes + 12 + pixel_bytes > avail) return fail("bad image block");
    tile.width_words = w;
    tile.rows = h;
    tile.words.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < tile.words.size(); ++i) tile.words[i] = rd16(image + 12 + 2 * i);
    for (const StagePiece& piece : tile.pieces)
        if (piece.ax + piece.w > w * 2 || piece.ay + piece.h > h) return fail("piece outside the picture");
    tile.block_hash = hash_image_words(tile.words.data(), static_cast<std::size_t>(w), w, h);
    tile.clut_hash = hash_palette(tile.clut.data(), 256);
    return true;
}

}  // namespace

bool parse_stage_pack(std::span<const uint8_t> file, StagePack& out, std::string& error) {
    out = {};
    if (file.size() < 16) {
        error = "stage pack too short";
        return false;
    }
    const uint32_t cols = rd32(file.data() + 4), rows = rd32(file.data() + 8), slot = rd32(file.data() + 12);
    if (cols == 0 || rows == 0 || cols > 64 || rows > 64 || slot == 0 || slot % kSector != 0) {
        error = "bad stage pack header";
        return false;
    }
    const std::size_t cells = static_cast<std::size_t>(cols) * rows;
    if (16 + cells * 2 > kSector || kSector + cells * slot > file.size()) {
        error = "stage pack is smaller than its header says";
        return false;
    }
    out.cols = static_cast<int>(cols);
    out.rows = static_cast<int>(rows);
    std::vector<uint8_t> decoded;
    for (std::size_t i = 0; i < cells; ++i) {
        if (rd16(file.data() + 16 + 2 * i) == 0) continue;
        std::span<const uint8_t> slot_data = file.subspan(kSector + i * slot, slot);
        std::span<const uint8_t> cell = slot_data;
        if (is_rlen(slot_data)) {
            if (!rlen_decode(slot_data, decoded)) {
                error = "cell " + std::to_string(i) + ": bad run-length data";
                return false;
            }
            cell = decoded;
        }
        StageTile tile;
        tile.col = static_cast<int>(i % cols);
        tile.row = static_cast<int>(i / cols);
        std::string cell_error;
        if (!parse_cell(cell, tile, cell_error)) {
            error = "cell " + std::to_string(i) + ": " + cell_error;
            return false;
        }
        out.tiles.push_back(std::move(tile));
    }
    return true;
}

Image decode_stage_tile(const StageTile& tile) {
    Image img;
    img.w = tile.width_texels();
    img.h = tile.rows;
    decode_8bit_words(tile.words.data(), static_cast<std::size_t>(tile.width_words), tile.width_words, tile.rows, tile.clut.data(), img.rgba);
    return img;
}

Image compose_stage_map(const StagePack& pack) {
    Image map;
    map.w = pack.cols * kStageCell;
    map.h = pack.rows * kStageCell;
    map.rgba.assign(static_cast<std::size_t>(map.w) * map.h, 0);
    std::vector<Image> pictures;
    pictures.reserve(pack.tiles.size());
    for (const StageTile& tile : pack.tiles) pictures.push_back(decode_stage_tile(tile));
    for (int layer = 0; layer < 3; ++layer)
        for (std::size_t t = 0; t < pack.tiles.size(); ++t) {
            const StageTile& tile = pack.tiles[t];
            const Image& picture = pictures[t];
            for (const StagePiece& piece : tile.pieces) {
                if (piece.layer != layer) continue;
                for (int y = 0; y < piece.h; ++y) {
                    const int my = tile.row * kStageCell + piece.y + y;
                    if (my < 0 || my >= map.h) continue;
                    for (int x = 0; x < piece.w; ++x) {
                        const int mx = tile.col * kStageCell + piece.x + x;
                        if (mx < 0 || mx >= map.w) continue;
                        const uint32_t c = picture.rgba[static_cast<std::size_t>(piece.ay + y) * picture.w + piece.ax + x];
                        if (c >> 24) map.rgba[static_cast<std::size_t>(my) * map.w + mx] = c;
                    }
                }
            }
        }
    return map;
}

}  // namespace dw3::texrep
