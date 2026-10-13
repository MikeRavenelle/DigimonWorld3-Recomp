// Field map backgrounds from the game files: run-length decoding, the stage pack parser, the keys it computes
// (which must equal what the renderer computes from VRAM), map layout, dump writing and pack generation.
// Everything except the last tests runs on synthetic data.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

#include "../game_disc.h"
#include "dw3/texrep/blocks.h"
#include "dw3/texrep/dump.h"
#include "dw3/texrep/generate.h"
#include "dw3/texrep/stage_dump.h"
#include "dw3/texrep/stage_pack.h"

using namespace dw3;
using namespace dw3::texrep;

namespace {

using Bytes = std::vector<uint8_t>;

void put16(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(Bytes& b, uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}

// Encoder for the run-length format: runs of 3 or more equal bytes become repeats, the rest literals.
Bytes rlen_encode(const Bytes& in) {
    Bytes out = {'R', 'L', 'E', 'N'};
    put32(out, static_cast<uint32_t>(in.size()));
    std::size_t i = 0;
    Bytes literal;
    auto flush = [&] {
        for (std::size_t k = 0; k < literal.size(); k += 127) {
            const std::size_t n = std::min<std::size_t>(127, literal.size() - k);
            out.push_back(static_cast<uint8_t>(n));
            out.insert(out.end(), literal.begin() + static_cast<std::ptrdiff_t>(k), literal.begin() + static_cast<std::ptrdiff_t>(k + n));
        }
        literal.clear();
    };
    while (i < in.size()) {
        std::size_t run = 1;
        while (i + run < in.size() && in[i + run] == in[i] && run < 127) ++run;
        if (run >= 3) {
            flush();
            out.push_back(static_cast<uint8_t>(0x80 | run));
            out.push_back(in[i]);
            i += run;
        } else {
            literal.push_back(in[i++]);
        }
    }
    flush();
    out.push_back(0);
    return out;
}

struct PieceSpec {
    int layer, x, y, ax, ay, w, h;
};

// One cell: header with the pieces, then an 8-bit TIM with a 256-entry palette.
Bytes make_cell(const std::vector<PieceSpec>& pieces, int width_words, int rows, const std::vector<uint16_t>& pixels,
                const std::vector<uint16_t>& clut) {
    Bytes b;
    put32(b, static_cast<uint32_t>(16 + 12 * pieces.size()));
    for (int layer = 0; layer < 3; ++layer) {
        put32(b, static_cast<uint32_t>(std::count_if(pieces.begin(), pieces.end(), [&](const PieceSpec& p) { return p.layer == layer; })));
        for (const PieceSpec& p : pieces) {
            if (p.layer != layer) continue;
            put16(b, static_cast<uint32_t>(p.x));
            put16(b, static_cast<uint32_t>(p.y));
            put16(b, static_cast<uint32_t>(p.ax));
            put16(b, static_cast<uint32_t>(p.ay));
            put16(b, static_cast<uint32_t>(p.w));
            put16(b, static_cast<uint32_t>(p.h));
        }
    }
    put32(b, 0x10);
    put32(b, 9);
    put32(b, 12 + 512);
    put16(b, 0), put16(b, 0), put16(b, 256), put16(b, 1);
    for (uint16_t c : clut) put16(b, c);
    put32(b, static_cast<uint32_t>(12 + width_words * rows * 2));
    put16(b, 0), put16(b, 0), put16(b, static_cast<uint32_t>(width_words)), put16(b, static_cast<uint32_t>(rows));
    for (uint16_t px : pixels) put16(b, px);
    return b;
}

std::vector<uint16_t> test_palette(uint16_t base) {
    std::vector<uint16_t> clut(256);
    for (int i = 0; i < 256; ++i) clut[static_cast<std::size_t>(i)] = i == 0 ? 0 : static_cast<uint16_t>(0x8000 | ((base + i * 37) & 0x7FFF));
    return clut;
}

std::vector<uint16_t> test_pixels(int words, int rows, int seed) {
    std::vector<uint16_t> px(static_cast<std::size_t>(words) * rows);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint16_t>((i * 2654435761u + static_cast<unsigned>(seed) * 40503u) >> 7);
    return px;
}

struct CellSpec {
    int index;  // row-major
    Bytes data;
    bool compress;
};

Bytes make_pack(int cols, int rows, uint32_t slot, const std::vector<CellSpec>& cells) {
    Bytes b;
    put32(b, static_cast<uint32_t>(cells.size()));
    put32(b, static_cast<uint32_t>(cols));
    put32(b, static_cast<uint32_t>(rows));
    put32(b, slot);
    for (int i = 0; i < cols * rows; ++i) {
        const bool used = std::any_of(cells.begin(), cells.end(), [&](const CellSpec& c) { return c.index == i; });
        put16(b, used ? 0x4000 : 0);
    }
    b.resize(2048, 0);
    b.resize(2048 + static_cast<std::size_t>(cols) * rows * slot, 0);
    for (const CellSpec& c : cells) {
        const Bytes slot_data = c.compress ? rlen_encode(c.data) : c.data;
        if (slot_data.size() > slot) {
            ADD_FAILURE() << "test cell does not fit its slot";
            continue;
        }
        std::copy(slot_data.begin(), slot_data.end(), b.begin() + static_cast<std::ptrdiff_t>(2048 + static_cast<std::size_t>(c.index) * slot));
    }
    return b;
}

std::filesystem::path temp_dir(const char* name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / (std::string("dw3_stage_") + name);
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

Image replicate_image(const Image& src, int factor) {
    Image out;
    out.w = src.w * factor;
    out.h = src.h * factor;
    out.rgba.resize(static_cast<std::size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) out.rgba[static_cast<std::size_t>(y) * out.w + x] = src.rgba[static_cast<std::size_t>(y / factor) * src.w + x / factor];
    return out;
}

}  // namespace

// ---- run-length streams --------------------------------------------------------------------------------------------

TEST(RlenDecode, LiteralsAndRuns) {
    const Bytes stream = {'R', 'L', 'E', 'N', 9, 0, 0, 0, 3, 7, 8, 9, 0x84, 5, 0x82, 1, 0};
    Bytes out;
    ASSERT_TRUE(rlen_decode(stream, out));
    EXPECT_EQ(out, (Bytes{7, 8, 9, 5, 5, 5, 5, 1, 1}));
}

TEST(RlenDecode, StopsAtTheDeclaredSize) {
    const Bytes stream = {'R', 'L', 'E', 'N', 2, 0, 0, 0, 0x85, 4, 0};
    Bytes out;
    ASSERT_TRUE(rlen_decode(stream, out));
    EXPECT_EQ(out, (Bytes{4, 4}));
}

TEST(RlenDecode, RejectsMalformedStreams) {
    Bytes out;
    EXPECT_FALSE(rlen_decode(Bytes{'R', 'L', 'E', 'X', 0, 0, 0, 0, 0}, out));
    EXPECT_FALSE(rlen_decode(Bytes{'R', 'L', 'E', 'N', 5, 0, 0, 0, 3, 1, 2}, out));       // literal runs past the end
    EXPECT_FALSE(rlen_decode(Bytes{'R', 'L', 'E', 'N', 5, 0, 0, 0, 0x83}, out));          // repeat without its byte
    EXPECT_FALSE(rlen_decode(Bytes{'R', 'L', 'E', 'N', 6, 0, 0, 0, 2, 1, 2, 0}, out));    // ends early
    EXPECT_FALSE(rlen_decode(Bytes{'R', 'L', 'E', 'N', 0xFF, 0xFF, 0xFF, 0x7F, 0}, out));  // absurd size
}

TEST(RlenDecode, RoundTripsTheTestEncoder) {
    Bytes data;
    for (int i = 0; i < 5000; ++i) data.push_back(static_cast<uint8_t>(i < 2000 ? i / 300 : (i * 31) >> 3));
    Bytes out;
    ASSERT_TRUE(rlen_decode(rlen_encode(data), out));
    EXPECT_EQ(out, data);
}

// ---- parsing ---------------------------------------------------------------------------------------------------------

TEST(StagePack, ParsesCellsPiecesAndPictures) {
    const std::vector<uint16_t> px_a = test_pixels(64, 128, 1), px_b = test_pixels(40, 100, 2);
    const Bytes cell_a = make_cell({{0, 0, 0, 0, 0, 128, 128}}, 64, 128, px_a, test_palette(10));
    const Bytes cell_b = make_cell({{0, 5, 7, 0, 0, 50, 60}, {2, 90, 3, 50, 0, 30, 20}}, 40, 100, px_b, test_palette(99));
    const Bytes file = make_pack(3, 2, 0x4800, {{1, cell_a, true}, {4, cell_b, false}});

    StagePack pack;
    std::string error;
    ASSERT_TRUE(parse_stage_pack(file, pack, error)) << error;
    EXPECT_EQ(pack.cols, 3);
    EXPECT_EQ(pack.rows, 2);
    ASSERT_EQ(pack.tiles.size(), 2u);
    EXPECT_EQ(pack.tiles[0].col, 1);
    EXPECT_EQ(pack.tiles[0].row, 0);
    EXPECT_EQ(pack.tiles[1].col, 1);
    EXPECT_EQ(pack.tiles[1].row, 1);
    EXPECT_EQ(pack.tiles[0].words, px_a);
    EXPECT_EQ(pack.tiles[0].width_texels(), 128);
    EXPECT_EQ(pack.tiles[1].rows, 100);
    ASSERT_EQ(pack.tiles[1].pieces.size(), 2u);
    EXPECT_EQ(pack.tiles[1].pieces[0].layer, 0);
    EXPECT_EQ(pack.tiles[1].pieces[0].x, 5);
    EXPECT_EQ(pack.tiles[1].pieces[0].y, 7);
    EXPECT_EQ(pack.tiles[1].pieces[1].layer, 2);
    EXPECT_EQ(pack.tiles[1].pieces[1].ax, 50);
    EXPECT_EQ(pack.tiles[1].pieces[1].w, 30);
    EXPECT_EQ(pack.tiles[1].clut[3], test_palette(99)[3]);
}

TEST(StagePack, KeysMatchWhatTheRendererComputesFromVram) {
    const std::vector<uint16_t> pixels = test_pixels(100, 77, 5), clut = test_palette(1234);
    const Bytes file = make_pack(1, 1, 0x4800, {{0, make_cell({{0, 0, 0, 0, 0, 200, 77}}, 100, 77, pixels, clut), true}});
    StagePack pack;
    std::string error;
    ASSERT_TRUE(parse_stage_pack(file, pack, error)) << error;

    // The game uploads the picture and its palette; the renderer hashes what ended up in VRAM.
    std::vector<uint16_t> vram(kVramW * kVramH, 0);
    for (int y = 0; y < 77; ++y)
        std::copy_n(pixels.begin() + y * 100, 100, vram.begin() + (y + 300) * kVramW + 640);
    std::copy(clut.begin(), clut.end(), vram.begin() + 245 * kVramW + 0);
    BlockRegistry registry;
    registry.add_upload({640, 300, 740, 377});
    Block* block = registry.find(650, 310);
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(registry.hash_of(*block, vram), pack.tiles[0].block_hash);
    EXPECT_EQ(hash_clut(vram, 0, 245), pack.tiles[0].clut_hash);
    // A different position of the same picture gives the same key.
    std::vector<uint16_t> moved(kVramW * kVramH, 0);
    for (int y = 0; y < 77; ++y) std::copy_n(pixels.begin() + y * 100, 100, moved.begin() + (y + 10) * kVramW + 900);
    EXPECT_EQ(hash_vram_rect(moved, {900, 10, 1000, 87}), pack.tiles[0].block_hash);
}

TEST(StagePack, TheShapeIsPartOfTheKey) {
    const std::vector<uint16_t> pixels = test_pixels(64, 64, 9), clut = test_palette(5);
    StagePack wide, narrow;
    std::string error;
    ASSERT_TRUE(parse_stage_pack(make_pack(1, 1, 0x4800, {{0, make_cell({}, 64, 64, pixels, clut), false}}), wide, error)) << error;
    ASSERT_TRUE(parse_stage_pack(make_pack(1, 1, 0x4800, {{0, make_cell({}, 32, 128, pixels, clut), false}}), narrow, error)) << error;
    EXPECT_NE(wide.tiles[0].block_hash, narrow.tiles[0].block_hash);
    EXPECT_EQ(wide.tiles[0].clut_hash, narrow.tiles[0].clut_hash);
}

TEST(StagePack, OnlyPicturesTheRendererTracksAreTracked) {
    const std::vector<uint16_t> clut = test_palette(5);
    StagePack pack;
    std::string error;
    auto tile_of = [&](int words, int rows) {
        EXPECT_TRUE(parse_stage_pack(make_pack(1, 1, 0x9000, {{0, make_cell({}, words, rows, test_pixels(words, rows, 1), clut), false}}), pack, error)) << error;
        return pack.tiles[0].tracked();
    };
    EXPECT_TRUE(tile_of(128, 128));
    EXPECT_TRUE(tile_of(8, 8));
    EXPECT_FALSE(tile_of(7, 64));
    EXPECT_FALSE(tile_of(64, 7));
}

TEST(StagePack, RejectsDamagedFiles) {
    const Bytes cell = make_cell({{0, 0, 0, 0, 0, 16, 16}}, 8, 16, test_pixels(8, 16, 3), test_palette(1));
    const Bytes good = make_pack(2, 1, 0x1000, {{0, cell, true}});
    StagePack pack;
    std::string error;
    ASSERT_TRUE(parse_stage_pack(good, pack, error)) << error;

    EXPECT_FALSE(parse_stage_pack(Bytes(good.begin(), good.begin() + 100), pack, error));  // cut short
    Bytes bad = good;
    bad[12] = 0x10;  // slot size not a multiple of a sector
    bad[13] = 0x10 + 1;
    EXPECT_FALSE(parse_stage_pack(bad, pack, error));
    bad = good;
    bad[4] = 0;  // no columns
    EXPECT_FALSE(parse_stage_pack(bad, pack, error));

    // A piece that reaches outside its picture.
    const Bytes outside = make_cell({{0, 0, 0, 10, 0, 16, 16}}, 8, 16, test_pixels(8, 16, 3), test_palette(1));
    EXPECT_FALSE(parse_stage_pack(make_pack(1, 1, 0x1000, {{0, outside, false}}), pack, error));
    // Not an 8-bit TIM.
    Bytes not_tim = cell;
    not_tim[16 + 12 + 4] = 8;
    EXPECT_FALSE(parse_stage_pack(make_pack(1, 1, 0x1000, {{0, not_tim, false}}), pack, error));
    // Header size that does not match the piece lists.
    Bytes bad_header = cell;
    bad_header[0] += 12;
    EXPECT_FALSE(parse_stage_pack(make_pack(1, 1, 0x1000, {{0, bad_header, false}}), pack, error));
}

// ---- layout -----------------------------------------------------------------------------------------------------------

TEST(StageLayout, PiecesAreDrawnInLayerOrderAtTheirCellOffsets) {
    // A 16x8 picture: left half index 1, right half index 2; palette 1 red, 2 green, the rest transparent.
    std::vector<uint16_t> pixels(8 * 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) pixels[static_cast<std::size_t>(y * 8 + x)] = x < 4 ? 0x0101 : 0x0202;
    std::vector<uint16_t> clut(256, 0);
    clut[1] = 0x001F;  // red
    clut[2] = 0x03E0;  // green
    // Cell (1, 0): the left half drawn at (4, 2), the right half drawn over it, half a block to the left, in layer 1.
    const Bytes cell = make_cell({{0, 4, 2, 0, 0, 8, 8}, {1, 8, 2, 8, 0, 8, 8}}, 8, 8, pixels, clut);
    StagePack pack;
    std::string error;
    ASSERT_TRUE(parse_stage_pack(make_pack(2, 2, 0x800, {{1, cell, false}}), pack, error)) << error;

    const Image map = compose_stage_map(pack);
    ASSERT_EQ(map.w, 256);
    ASSERT_EQ(map.h, 256);
    auto at = [&](int x, int y) { return map.rgba[static_cast<std::size_t>(y) * map.w + x]; };
    const uint32_t red = 0xFF0000FFu, green = 0xFF00FF00u;
    EXPECT_EQ(at(128 + 4, 2), red);
    EXPECT_EQ(at(128 + 7, 9), red);
    EXPECT_EQ(at(128 + 8, 2), green);   // layer 1 starts at x = 8 in the cell
    EXPECT_EQ(at(128 + 15, 9), green);
    EXPECT_EQ(at(128 + 3, 2), 0u);      // outside every piece
    EXPECT_EQ(at(128 + 4, 1), 0u);
    EXPECT_EQ(at(0, 0), 0u);            // the other cells are empty
    EXPECT_EQ(at(128 + 4, 10), 0u);
}

TEST(StageLayout, TransparentTexelsDoNotCoverEarlierLayers) {
    // 8 texels wide: the left four are index 1 (blue), the right four index 0 (transparent).
    std::vector<uint16_t> pixels(4 * 4, 0x0101);
    for (int y = 0; y < 4; ++y) pixels[static_cast<std::size_t>(y * 4 + 2)] = pixels[static_cast<std::size_t>(y * 4 + 3)] = 0;
    std::vector<uint16_t> clut(256, 0);
    clut[1] = 0x7C00;
    // Layer 0 draws the blue half at x = 4; layer 1 draws the transparent half over the same place.
    const Bytes cell = make_cell({{0, 4, 0, 0, 0, 4, 4}, {1, 4, 0, 4, 0, 4, 4}}, 4, 4, pixels, clut);
    StagePack pack;
    std::string error;
    ASSERT_TRUE(parse_stage_pack(make_pack(1, 1, 0x800, {{0, cell, false}}), pack, error)) << error;
    const Image map = compose_stage_map(pack);
    const uint32_t blue = 0xFFFF0000u;
    EXPECT_EQ(map.rgba[4], blue);
    EXPECT_EQ(map.rgba[7], blue);
    EXPECT_EQ(map.rgba[3], 0u);
    EXPECT_EQ(map.rgba[8], 0u);
}

// ---- dump and pack --------------------------------------------------------------------------------------------------

TEST(StageDump, WritesDistinctImagesOnceAndSkipsWhatCannotBeTracked) {
    const std::filesystem::path dir = temp_dir("dump");
    const std::vector<uint16_t> clut = test_palette(3);
    const Bytes tile_a = make_cell({{0, 0, 0, 0, 0, 100, 40}, {1, 20, 60, 100, 0, 28, 30}}, 64, 64, test_pixels(64, 64, 1), clut);
    const Bytes tile_b = make_cell({{0, 0, 0, 0, 0, 128, 128}}, 64, 128, test_pixels(64, 128, 2), clut);
    const Bytes tiny = make_cell({{0, 0, 0, 0, 0, 8, 8}}, 4, 8, test_pixels(4, 8, 3), clut);
    StagePack one, two;
    std::string error;
    ASSERT_TRUE(parse_stage_pack(make_pack(2, 1, 0x4800, {{0, tile_a, true}, {1, tiny, false}}), one, error)) << error;
    ASSERT_TRUE(parse_stage_pack(make_pack(2, 1, 0x4800, {{0, tile_a, false}, {1, tile_b, true}}), two, error)) << error;

    StageDumpWriter writer(dir);
    ASSERT_TRUE(writer.add_map("S001", one, error)) << error;
    ASSERT_TRUE(writer.add_map("S002", two, error)) << error;
    ASSERT_TRUE(writer.finish(error)) << error;
    EXPECT_EQ(writer.stats().maps, 2u);
    EXPECT_EQ(writer.stats().cells, 4u);
    EXPECT_EQ(writer.stats().untracked, 1u);
    EXPECT_EQ(writer.stats().images, 2u);  // tile_a is shared by both maps

    std::ifstream in(dir / "dump.toml", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    DumpManifest manifest;
    ASSERT_TRUE(parse_dump_manifest(text, manifest, error)) << error;
    ASSERT_EQ(manifest.components.size(), 2u);
    EXPECT_EQ(manifest.components[0].width, 256);
    ASSERT_EQ(manifest.textures.size(), 2u);
    const DumpTexture& first = manifest.textures[0];
    EXPECT_EQ(first.block, one.tiles[0].block_hash);
    EXPECT_EQ(first.clut, one.tiles[0].clut_hash);
    EXPECT_EQ(first.width, 128);
    EXPECT_EQ(first.height, 64);
    ASSERT_EQ(first.pieces.size(), 2u);
    EXPECT_EQ(first.pieces[1].ax, 100);
    EXPECT_EQ(first.pieces[1].cx, 20);  // cell column 0
    EXPECT_EQ(first.pieces[1].cy, 60);
    EXPECT_TRUE(std::filesystem::exists(dir / first.file));
    EXPECT_TRUE(std::filesystem::exists(dir / manifest.components[1].file));
    EXPECT_EQ(manifest.textures[1].pieces[0].cx, 128);  // tile_b sits in column 1
}

TEST(StageDump, GeneratedPackKeepsEachPieceAtItsPlaceInTheImage) {
    const std::filesystem::path dir = temp_dir("gen_dump"), out = temp_dir("gen_pack");
    // A picture of 16 x 8 texels, the smallest the renderer tracks: left half index 1 (red), right half index 2
    // (green). Two pieces draw the halves swapped on the map.
    std::vector<uint16_t> clut(256, 0);
    clut[1] = 0x001F;
    clut[2] = 0x03E0;
    std::vector<uint16_t> tall(8 * 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) tall[static_cast<std::size_t>(y * 8 + x)] = x < 4 ? 0x0101 : 0x0202;
    const Bytes cell = make_cell({{0, 0, 0, 8, 0, 8, 8}, {0, 8, 0, 0, 0, 8, 8}}, 8, 8, tall, clut);
    StagePack pack;
    std::string error;
    ASSERT_TRUE(parse_stage_pack(make_pack(1, 1, 0x800, {{0, cell, false}}), pack, error)) << error;
    StageDumpWriter writer(dir);
    ASSERT_TRUE(writer.add_map("S001", pack, error)) << error;
    ASSERT_TRUE(writer.finish(error)) << error;

    GenerateOptions options;
    options.scale = 2;
    options.model_scale = 2;
    options.pad = 4;
    GenerateReport report;
    ASSERT_TRUE(generate_pack({dir}, out, options,
                              [](const std::vector<Image>& in, std::vector<Image>& result, std::string&) {
                                  result.clear();
                                  for (const Image& img : in) result.push_back(replicate_image(img, 2));
                                  return true;
                              },
                              report, error))
        << error;
    EXPECT_EQ(report.generated, 1u);
    Image image;
    ASSERT_TRUE(read_png(out / "tex" / (hash_to_hex(pack.tiles[0].block_hash) + "_" + hash_to_hex(pack.tiles[0].clut_hash) + ".png"), image, error)) << error;
    ASSERT_EQ(image.w, 32);
    ASSERT_EQ(image.h, 16);
    // Left half of the picture (red) is the second piece, right half (green) the first: each stays where it is stored.
    const uint32_t red = 0xFF0000FFu, green = 0xFF00FF00u;
    EXPECT_EQ(image.rgba[3] & 0x00FFFFFFu, red & 0x00FFFFFFu);
    EXPECT_EQ(image.rgba[16 + 3] & 0x00FFFFFFu, green & 0x00FFFFFFu);
    EXPECT_EQ(image.rgba[15 * 32 + 3] & 0x00FFFFFFu, red & 0x00FFFFFFu);
    EXPECT_EQ(image.rgba[15 * 32 + 28] & 0x00FFFFFFu, green & 0x00FFFFFFu);
}

TEST(DumpManifest, PiecesRoundTrip) {
    DumpManifest m;
    DumpTexture t;
    t.block = 1;
    t.clut = 2;
    t.width = 64;
    t.height = 32;
    t.file = "textures/x.png";
    t.pieces = {{0, 0, 10, 20, 30, 40}, {10, 0, 54, 32, 100, 200}};
    m.textures.push_back(t);
    DumpTexture plain = t;
    plain.block = 3;
    plain.pieces.clear();
    m.textures.push_back(plain);
    DumpManifest back;
    std::string error;
    ASSERT_TRUE(parse_dump_manifest(write_dump_manifest(m), back, error)) << error;
    ASSERT_EQ(back.textures.size(), 2u);
    ASSERT_EQ(back.textures[0].pieces.size(), 2u);
    EXPECT_EQ(back.textures[0].pieces[1].cy, 200);
    EXPECT_EQ(back.textures[0].pieces[0].w, 10);
    EXPECT_TRUE(back.textures[1].pieces.empty());
}

// ---- finding the files ------------------------------------------------------------------------------------------------

TEST(StageSource, ListsMapsInAFolder) {
    const std::filesystem::path dir = temp_dir("source");
    std::filesystem::create_directories(dir / "AAA" / "DAT" / "Z_STAGE");
    const Bytes file = make_pack(1, 1, 0x800, {{0, make_cell({}, 8, 8, test_pixels(8, 8, 1), test_palette(1)), false}});
    for (const char* name : {"S226PACK.BIN", "S200PACK.BIN", "S001MASK.BIN", "README.TXT"}) {
        std::ofstream f(dir / "AAA" / "DAT" / "Z_STAGE" / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    std::string error;
    auto source = StageSource::open(dir, error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_EQ(source->maps(), (std::vector<std::string>{"S200", "S226"}));
    Bytes data;
    ASSERT_TRUE(source->read("S226", data, error)) << error;
    EXPECT_EQ(data, file);
    EXPECT_FALSE(source->read("S999", data, error));
    // The folder that holds the files directly works as well.
    EXPECT_NE(StageSource::open(dir / "AAA" / "DAT" / "Z_STAGE", error), nullptr);
    EXPECT_EQ(StageSource::open(temp_dir("empty"), error), nullptr);
}

// ---- the user's disc ----------------------------------------------------------------------------------------------------

TEST(StageSourceDisc, EveryFieldMapParses) {
    DW3_REQUIRE_DISC();
    std::string error;
    auto source = StageSource::open(dw3test::game_cue(), error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_GE(source->maps().size(), 200u);
    std::size_t tiles = 0, tracked = 0;
    std::set<std::pair<uint64_t, uint64_t>> keys;
    std::vector<uint8_t> data;
    for (const std::string& map : source->maps()) {
        ASSERT_TRUE(source->read(map, data, error)) << map << ": " << error;
        StagePack pack;
        ASSERT_TRUE(parse_stage_pack(data, pack, error)) << map << ": " << error;
        ASSERT_FALSE(pack.tiles.empty()) << map;
        tiles += pack.tiles.size();
        for (const StageTile& t : pack.tiles)
            if (t.tracked()) {
                ++tracked;
                keys.insert({t.block_hash, t.clut_hash});
            }
    }
    EXPECT_GT(tiles, 10000u);
    EXPECT_GT(tracked, tiles * 99 / 100);
    EXPECT_GT(keys.size(), 5000u);
}
