// Texture replacement, GPU-independent part: hashing, block registry, sprite splitting, atlas packing, pack and
// dump manifests, component layout, and pack generation with a stand-in upscaler.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <set>
#include <vector>

#include "dw3/texrep/blocks.h"
#include "dw3/texrep/dump.h"
#include "dw3/texrep/generate.h"
#include "dw3/texrep/pack.h"
#include "dw3/texrep/window.h"

using namespace dw3;
using namespace dw3::texrep;

namespace {

std::vector<uint16_t> blank_vram() { return std::vector<uint16_t>(kVramW * kVramH, 0); }

void fill_region(std::vector<uint16_t>& vram, const Region& r, uint16_t seed) {
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) vram[static_cast<std::size_t>(y) * kVramW + x] = static_cast<uint16_t>(seed + x * 31 + y * 17);
}

std::filesystem::path temp_dir(const char* name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / (std::string("dw3_texrep_") + name);
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

Image solid(int w, int h, uint32_t rgba) {
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.assign(static_cast<std::size_t>(w) * h, rgba);
    return img;
}

// A stand-in for the real upscaler: every pixel becomes a factor x factor square.
Image replicate(const Image& src, int factor) {
    Image out;
    out.w = src.w * factor;
    out.h = src.h * factor;
    out.rgba.resize(static_cast<std::size_t>(out.w) * out.h);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) out.rgba[static_cast<std::size_t>(y) * out.w + x] = src.rgba[static_cast<std::size_t>(y / factor) * src.w + x / factor];
    return out;
}

}  // namespace

// ---- hashing ------------------------------------------------------------------------------------------------------

TEST(TexrepHash, SameContentSameHashAnyPlace) {
    auto vram = blank_vram();
    fill_region(vram, {640, 0, 704, 128}, 5);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 64; ++x) vram[static_cast<std::size_t>(y + 128) * kVramW + 768 + x] = vram[static_cast<std::size_t>(y) * kVramW + 640 + x];
    EXPECT_EQ(hash_vram_rect(vram, {640, 0, 704, 128}), hash_vram_rect(vram, {768, 128, 832, 256}));
}

TEST(TexrepHash, OneWordChangesTheHash) {
    auto vram = blank_vram();
    fill_region(vram, {640, 0, 704, 128}, 5);
    const uint64_t before = hash_vram_rect(vram, {640, 0, 704, 128});
    vram[static_cast<std::size_t>(77) * kVramW + 651] ^= 0x0100;
    EXPECT_NE(before, hash_vram_rect(vram, {640, 0, 704, 128}));
}

TEST(TexrepHash, ShapeIsPartOfTheHash) {
    auto vram = blank_vram();  // all zero: equal pixels in different shapes
    EXPECT_NE(hash_vram_rect(vram, {0, 0, 32, 16}), hash_vram_rect(vram, {0, 0, 16, 32}));
}

TEST(TexrepHash, ClutHashFollowsTheEntriesAndWrapsAtTheEdge) {
    auto vram = blank_vram();
    for (int i = 0; i < 256; ++i) vram[static_cast<std::size_t>(240) * kVramW + i] = static_cast<uint16_t>(i * 3);
    const uint64_t a = hash_clut(vram, 0, 240);
    vram[static_cast<std::size_t>(240) * kVramW + 200] ^= 1;
    EXPECT_NE(a, hash_clut(vram, 0, 240));
    // A palette that runs past the right edge wraps to x = 0.
    auto v2 = blank_vram();
    v2[static_cast<std::size_t>(5) * kVramW + 1023] = 7;
    v2[static_cast<std::size_t>(5) * kVramW + 0] = 9;
    auto v3 = v2;
    v3[static_cast<std::size_t>(5) * kVramW + 0] = 10;
    EXPECT_NE(hash_clut(v2, 1000, 5), hash_clut(v3, 1000, 5));
}

TEST(TexrepHash, HexRoundTrip) {
    for (uint64_t v : {uint64_t{0}, uint64_t{1}, uint64_t{0xDEADBEEF12345678}, ~uint64_t{0}}) {
        uint64_t back = 5;
        ASSERT_TRUE(hash_from_hex(hash_to_hex(v), back));
        EXPECT_EQ(back, v);
    }
    uint64_t x;
    EXPECT_FALSE(hash_from_hex("", x));
    EXPECT_FALSE(hash_from_hex("xyz", x));
    EXPECT_FALSE(hash_from_hex("00000000000000000", x));  // 17 digits
}

// ---- registry -----------------------------------------------------------------------------------------------------

TEST(TexrepRegistry, UploadsBecomeBlocksAndAreFound) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});
    ASSERT_NE(reg.find(640, 0), nullptr);
    EXPECT_NE(reg.find(703, 127), nullptr);
    EXPECT_EQ(reg.find(704, 0), nullptr);
    EXPECT_EQ(reg.find(640, 128), nullptr);
}

TEST(TexrepRegistry, OverlappingUploadInvalidatesTheOldBlock) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});
    reg.add_upload({768, 0, 832, 128});
    reg.add_upload({700, 20, 760, 60});  // overlaps the first one only
    EXPECT_EQ(reg.find(640, 0), nullptr);
    EXPECT_NE(reg.find(768, 0), nullptr);
    ASSERT_NE(reg.find(700, 20), nullptr);
    EXPECT_EQ(reg.find(700, 20)->rect.width(), 60);
}

TEST(TexrepRegistry, WideUploadsAreOneBlockUpToTheLimit) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 640 + 128, 128});  // 256 texels wide: the widest accepted
    EXPECT_NE(reg.find(700, 5), nullptr);
    reg.add_upload({640, 256, 640 + 129, 384});
    EXPECT_EQ(reg.find(700, 260), nullptr) << "wider than any background image";
}

TEST(TexrepRegistry, PalettesAndStripsAreNotBlocks) {
    BlockRegistry reg;
    reg.add_upload({0, 240, 256, 241});    // a palette row
    reg.add_upload({640, 0, 704, 4});      // too few rows
    reg.add_upload({640, 8, 644, 128});    // too few columns
    reg.add_upload({0, 0, 1024, 512});     // a state load
    EXPECT_TRUE(reg.blocks().empty());
}

TEST(TexrepRegistry, DrawsFillsAndCopiesInvalidate) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});
    reg.invalidate({0, 0, 320, 240});
    EXPECT_NE(reg.find(640, 0), nullptr);
    reg.invalidate({700, 100, 710, 110});
    EXPECT_EQ(reg.find(640, 0), nullptr);
}

TEST(TexrepRegistry, StateLoadDropsEverything) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});
    reg.add_upload({768, 0, 832, 128});
    reg.add_upload({0, 0, 1024, 512});
    EXPECT_TRUE(reg.blocks().empty());
}

TEST(TexrepRegistry, ImplicitBlocksMayBeNarrowStrips) {
    BlockRegistry reg;
    EXPECT_NE(reg.add_implicit({700, 0, 704, 56}), nullptr);  // 8 texels wide
    EXPECT_EQ(reg.add_implicit({710, 0, 711, 56}), nullptr);  // one word: too thin
    EXPECT_EQ(reg.add_implicit({720, 0, 760, 1}), nullptr);   // one row
    reg.add_upload({800, 0, 804, 56});
    EXPECT_EQ(reg.find(801, 3), nullptr) << "an upload must still be at least 8 words wide";
}

TEST(TexrepRegistry, ImplicitBlocksNeverDropExistingOnes) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});
    EXPECT_EQ(reg.add_implicit({700, 10, 720, 40}), nullptr);
    EXPECT_NE(reg.find(640, 0), nullptr);
    Block* b = reg.add_implicit({768, 0, 800, 64});
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(reg.find(770, 3), b);
}

TEST(TexrepRegistry, HashIsComputedOnceFromTheContentAtThatTime) {
    auto vram = blank_vram();
    fill_region(vram, {640, 0, 704, 128}, 9);
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});
    Block* b = reg.find(640, 0);
    const uint64_t h = reg.hash_of(*b, vram);
    EXPECT_EQ(h, hash_vram_rect(vram, {640, 0, 704, 128}));
    vram[640] ^= 1;  // later changes do not matter: any real change goes through an upload, which makes a new block
    EXPECT_EQ(reg.hash_of(*b, vram), h);
}

// ---- splitting sprites along blocks --------------------------------------------------------------------------------

namespace {
int covered_area(const std::vector<Piece>& pieces) {
    int n = 0;
    for (const Piece& p : pieces) n += p.w * p.h;
    return n;
}
}  // namespace

TEST(TexrepSplit, SpriteInsideOneBlockIsOnePiece) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});  // texels 0..127 of a page at x = 640
    const auto pieces = split_by_blocks(reg, 640, 0, 10, 20, 100, 50);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_NE(pieces[0].block, nullptr);
    EXPECT_EQ(pieces[0].u, 10);
    EXPECT_EQ(pieces[0].v, 20);
    EXPECT_EQ(pieces[0].w, 100);
    EXPECT_EQ(pieces[0].h, 50);
    EXPECT_EQ(pieces[0].block_u0, 0);
    EXPECT_EQ(pieces[0].block_v0, 0);
}

TEST(TexrepSplit, SpriteAcrossTwoBlocksIsSplitAtTheEdgeAndCoversEverything) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});
    reg.add_upload({704, 0, 768, 128});
    const auto pieces = split_by_blocks(reg, 640, 0, 100, 0, 56, 128);  // texels 100..155 cross the edge at 128
    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_EQ(covered_area(pieces), 56 * 128);
    EXPECT_EQ(pieces[0].w, 28);
    EXPECT_EQ(pieces[1].u, 128);
    EXPECT_EQ(pieces[1].w, 28);
    EXPECT_EQ(pieces[1].block_u0, 128);
    EXPECT_NE(pieces[0].block, pieces[1].block);
}

TEST(TexrepSplit, CornerCrossingMakesUpToThreePieces) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 704, 128});
    reg.add_upload({704, 0, 768, 128});
    reg.add_upload({640, 128, 704, 256});
    reg.add_upload({704, 128, 768, 256});
    const auto pieces = split_by_blocks(reg, 640, 0, 100, 100, 56, 56);
    EXPECT_EQ(pieces.size(), 4u);
    EXPECT_EQ(covered_area(pieces), 56 * 56);
    std::set<Block*> blocks;
    for (const Piece& p : pieces) blocks.insert(p.block);
    EXPECT_EQ(blocks.size(), 4u);
}

TEST(TexrepSplit, PartsOutsideAnyBlockAreNotReplaced) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 700, 128});  // 120 texels wide
    const auto pieces = split_by_blocks(reg, 640, 0, 100, 0, 56, 10);
    ASSERT_EQ(pieces.size(), 2u);
    EXPECT_NE(pieces[0].block, nullptr);
    EXPECT_EQ(pieces[0].w, 20);
    EXPECT_EQ(pieces[1].block, nullptr);
    EXPECT_EQ(covered_area(pieces), 560);
}

TEST(TexrepSplit, ImplicitBlocksCoverTheRest) {
    BlockRegistry reg;
    const auto pieces = split_by_blocks(reg, 640, 0, 4, 8, 48, 99, true);
    ASSERT_EQ(pieces.size(), 1u);
    ASSERT_NE(pieces[0].block, nullptr);
    EXPECT_EQ(pieces[0].block->rect.x0, 642);
    EXPECT_EQ(pieces[0].block->rect.width(), 24);
    EXPECT_EQ(pieces[0].block_u0, 4);
    // The block is found again by the next draw of the same sprite.
    const auto again = split_by_blocks(reg, 640, 0, 4, 8, 48, 99, true);
    EXPECT_EQ(again[0].block, pieces[0].block);
}

TEST(TexrepSplit, BlockLeftOfThePageStartHasANegativeOrigin) {
    BlockRegistry reg;
    reg.add_upload({636, 0, 700, 128});  // starts 4 words (8 texels) before the page at x = 640
    const auto pieces = split_by_blocks(reg, 640, 0, 0, 0, 64, 16);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_EQ(pieces[0].block_u0, -8);
}

TEST(TexrepSplit, WrappingSpritesAreLeftAlone) {
    BlockRegistry reg;
    reg.add_upload({640, 0, 768, 128});
    const auto pieces = split_by_blocks(reg, 640, 0, 200, 0, 100, 10);  // u + w > 256
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_EQ(pieces[0].block, nullptr);
}

TEST(TexrepSplit, TexelAndPageRowsFollowThePageBase) {
    BlockRegistry reg;
    reg.add_upload({640, 256, 704, 384});
    const auto pieces = split_by_blocks(reg, 640, 256, 0, 0, 128, 128);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_EQ(pieces[0].block_v0, 0);
    EXPECT_TRUE(split_by_blocks(reg, 640, 0, 0, 0, 128, 128)[0].block == nullptr);
}

// ---- uv scaling -----------------------------------------------------------------------------------------------------

TEST(TexrepScaling, OffsetMapsTexelsOfTheBlockOntoItsAtlasSlot) {
    const int scale = 4, u0 = 128, v0 = 0, ax = 512, ay = 1024;
    const ReplacementOffset o = replacement_offset(u0, v0, ax, ay, scale);
    auto atlas_x = [&](double u) { return u * scale + o.x; };
    auto atlas_y = [&](double v) { return v * scale + o.y; };
    EXPECT_DOUBLE_EQ(atlas_x(128), ax);          // the block's first texel edge is the slot's left edge
    EXPECT_DOUBLE_EQ(atlas_y(0), ay);
    EXPECT_DOUBLE_EQ(atlas_x(128 + 10.5), ax + 42);  // texel 10, middle
    EXPECT_DOUBLE_EQ(atlas_y(127.5), ay + 510);
}

TEST(TexrepScaling, NegativeOffsetsSurviveSixteenBitPacking) {
    const ReplacementOffset o = replacement_offset(250, 130, 16, 16, 4);  // 16 - 1000, 16 - 520
    ASSERT_LT(o.x, 0);
    const uint32_t packed = (static_cast<uint32_t>(o.y) << 16) | (static_cast<uint32_t>(o.x) & 0xFFFFu);
    const int x = static_cast<int16_t>(packed & 0xFFFF), y = static_cast<int16_t>(packed >> 16);
    EXPECT_EQ(x, o.x);
    EXPECT_EQ(y, o.y);
}

// ---- atlas ----------------------------------------------------------------------------------------------------------

TEST(TexrepAtlas, AllocationsStayInsideAndDoNotOverlap) {
    ShelfAtlas atlas(1024, 1024);
    struct R {
        int x, y, w, h;
    };
    std::vector<R> placed;
    const int sizes[][2] = {{512, 512}, {256, 512}, {320, 256}, {128, 128}, {512, 256}, {640, 640}, {200, 100}, {512, 512}};
    for (const auto& s : sizes) {
        auto p = atlas.alloc(s[0], s[1]);
        if (!p) continue;
        placed.push_back({p->first, p->second, s[0], s[1]});
    }
    ASSERT_GE(placed.size(), 4u);
    for (std::size_t i = 0; i < placed.size(); ++i) {
        EXPECT_GE(placed[i].x, 0);
        EXPECT_GE(placed[i].y, 0);
        EXPECT_LE(placed[i].x + placed[i].w, 1024);
        EXPECT_LE(placed[i].y + placed[i].h, 1024);
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
            const R &a = placed[i], &b = placed[j];
            const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
            EXPECT_FALSE(overlap) << i << " and " << j;
        }
    }
}

TEST(TexrepAtlas, FullAtlasRefusesAndResetStartsOver) {
    ShelfAtlas atlas(512, 512);
    EXPECT_TRUE(atlas.alloc(512, 512));
    EXPECT_FALSE(atlas.alloc(16, 16));
    atlas.reset();
    auto p = atlas.alloc(512, 512);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->first, 0);
    EXPECT_EQ(p->second, 0);
    EXPECT_FALSE(atlas.alloc(513, 8));
}

TEST(TexrepAtlas, SixtyFourFullSizeImagesFit) {
    ShelfAtlas atlas(4096, 4096);
    int n = 0;
    while (atlas.alloc(512, 512)) ++n;
    EXPECT_EQ(n, 64);
}

// ---- manifests -----------------------------------------------------------------------------------------------------

TEST(TexrepManifest, RoundTrip) {
    PackManifest m;
    m.name = "sample";
    m.scale = 4;
    m.entries.push_back({0x0123456789abcdefull, 0xfedcba9876543210ull, 128, 128, "tex/a.png"});
    m.entries.push_back({0x1ull, 0x2ull, 80, 110, "tex/b.png"});
    PackManifest back;
    std::string error;
    ASSERT_TRUE(parse_pack_manifest(write_pack_manifest(m), back, error)) << error;
    EXPECT_EQ(back.name, "sample");
    EXPECT_EQ(back.scale, 4);
    ASSERT_EQ(back.entries.size(), 2u);
    EXPECT_EQ(back.entries[0].block, 0x0123456789abcdefull);
    EXPECT_EQ(back.entries[0].clut, 0xfedcba9876543210ull);
    EXPECT_EQ(back.entries[1].height, 110);
    EXPECT_EQ(back.entries[1].file, "tex/b.png");
}

TEST(TexrepManifest, RejectsBadInput) {
    PackManifest m;
    std::string error;
    EXPECT_FALSE(parse_pack_manifest("this is = = not toml", m, error));
    EXPECT_FALSE(parse_pack_manifest("format = 2\nscale = 4\n", m, error));
    EXPECT_FALSE(parse_pack_manifest("format = 1\nscale = 5\n", m, error));
    EXPECT_FALSE(parse_pack_manifest("format = 1\nscale = 0\n", m, error));
    EXPECT_FALSE(parse_pack_manifest("format = 1\nscale = 2\n[[texture]]\nblock = \"zz\"\nclut = \"1\"\nwidth = 8\nheight = 8\nfile = \"a.png\"\n", m, error));
    EXPECT_FALSE(parse_pack_manifest("format = 1\nscale = 2\n[[texture]]\nblock = \"1\"\nclut = \"1\"\nwidth = 0\nheight = 8\nfile = \"a.png\"\n", m, error));
    EXPECT_FALSE(parse_pack_manifest("format = 1\nscale = 2\n[[texture]]\nblock = \"1\"\nclut = \"1\"\nwidth = 8\nheight = 8\nfile = \"../a.png\"\n", m, error));
    EXPECT_FALSE(parse_pack_manifest("format = 1\nscale = 2\n[[texture]]\nblock = \"1\"\nclut = \"1\"\nwidth = 8\nheight = 8\nfile = \"/etc/a.png\"\n", m, error));
    EXPECT_TRUE(parse_pack_manifest("format = 1\nscale = 2\n", m, error)) << "a pack without textures is valid";
}

TEST(TexrepManifest, PackNames) {
    EXPECT_TRUE(valid_pack_name("general-x4"));
    EXPECT_FALSE(valid_pack_name(""));
    EXPECT_FALSE(valid_pack_name(".."));
    EXPECT_FALSE(valid_pack_name("a/b"));
    EXPECT_FALSE(valid_pack_name("a\\b"));
    EXPECT_FALSE(valid_pack_name("c:x"));
}

TEST(TexrepPack, LookupByBlockAndPaletteAndImageSizeCheck) {
    const auto dir = temp_dir("pack");
    std::string error;
    ASSERT_TRUE(write_png(dir / "tex/a.png", solid(16, 8, 0xFF102030), error)) << error;
    ASSERT_TRUE(write_png(dir / "tex/wrong.png", solid(7, 8, 0xFF102030), error)) << error;
    PackManifest m;
    m.scale = 2;
    m.entries.push_back({1, 10, 8, 4, "tex/a.png"});
    m.entries.push_back({1, 11, 8, 4, "tex/wrong.png"});
    auto pack = TexturePack::from_manifest(m, dir);
    EXPECT_TRUE(pack->has_block(1));
    EXPECT_FALSE(pack->has_block(2));
    ASSERT_NE(pack->find(1, 10), nullptr);
    EXPECT_EQ(pack->find(1, 12), nullptr);
    Image img;
    EXPECT_TRUE(pack->load_image(*pack->find(1, 10), img, error)) << error;
    EXPECT_EQ(img.w, 16);
    EXPECT_FALSE(pack->load_image(*pack->find(1, 11), img, error));
    EXPECT_NE(error.find("expected"), std::string::npos);
}

TEST(TexrepPack, PngRoundTripKeepsPixels) {
    const auto dir = temp_dir("png");
    Image img = solid(5, 3, 0x80010203);
    img.rgba[7] = 0xFF0A0B0C;
    std::string error;
    ASSERT_TRUE(write_png(dir / "x.png", img, error)) << error;
    Image back;
    ASSERT_TRUE(read_png(dir / "x.png", back, error)) << error;
    EXPECT_EQ(back.w, 5);
    EXPECT_EQ(back.rgba, img.rgba);
    ASSERT_TRUE(write_png(dir / "rgb.png", img, error, false));
    ASSERT_TRUE(read_png(dir / "rgb.png", back, error));
    EXPECT_EQ(back.rgba[7], 0xFF0A0B0Cu) << "RGB files are read back opaque";
}

TEST(TexrepPack, UnreadableFilesGiveAnError) {
    Image img;
    std::string error;
    EXPECT_FALSE(read_png(temp_dir("missing") / "nope.png", img, error));
    EXPECT_FALSE(decode_png(reinterpret_cast<const uint8_t*>("not a png"), 9, img, error));
}

// ---- decoding a block ------------------------------------------------------------------------------------------------

TEST(TexrepDecode, IndexedTexelsGoThroughThePalette) {
    auto vram = blank_vram();
    // Two words = four texels: indices 0, 1, 2, 1 (low byte first).
    vram[static_cast<std::size_t>(10) * kVramW + 640] = 0x0100;
    vram[static_cast<std::size_t>(10) * kVramW + 641] = 0x0102;
    vram[static_cast<std::size_t>(240) * kVramW + 0] = 0;                 // transparent
    vram[static_cast<std::size_t>(240) * kVramW + 1] = 31;                // red
    vram[static_cast<std::size_t>(240) * kVramW + 2] = (31 << 10) | 1;    // blue
    std::vector<uint32_t> out;
    decode_block_8bit(vram, {640, 10, 642, 11}, 0, 240, out);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], 0u);
    EXPECT_EQ(out[1], 0xFF0000FFu);
    EXPECT_EQ(out[2] >> 24, 0xFFu);
    EXPECT_EQ((out[2] >> 16) & 255, 255u);
    EXPECT_EQ(out[3], out[1]);
}

// ---- component layout -----------------------------------------------------------------------------------------------

TEST(TexrepComponents, BlocksThatTouchAreJoinedWithTheirRelativePositions) {
    const BlockSize blocks[] = {{1, 128, 128}, {2, 128, 128}, {3, 80, 110}};
    const PairOffset pairs[] = {{1, 2, 128, 0, 5}, {1, 3, 0, 128, 5}};
    const auto comps = build_components(blocks, pairs);
    ASSERT_EQ(comps.size(), 1u);
    EXPECT_EQ(comps[0].width, 256);
    EXPECT_EQ(comps[0].height, 238);
    for (const PlacedBlock& b : comps[0].blocks) {
        if (b.block == 1) EXPECT_EQ(std::make_pair(b.x, b.y), std::make_pair(0, 0));
        if (b.block == 2) EXPECT_EQ(std::make_pair(b.x, b.y), std::make_pair(128, 0));
        if (b.block == 3) EXPECT_EQ(std::make_pair(b.x, b.y), std::make_pair(0, 128));
    }
}

TEST(TexrepComponents, NegativeOffsetsAreNormalisedToTheTopLeft) {
    const BlockSize blocks[] = {{1, 64, 64}, {2, 64, 64}};
    const PairOffset pairs[] = {{1, 2, -64, -64, 1}};
    const auto comps = build_components(blocks, pairs);
    ASSERT_EQ(comps.size(), 1u);
    for (const PlacedBlock& b : comps[0].blocks) {
        if (b.block == 2) EXPECT_EQ(std::make_pair(b.x, b.y), std::make_pair(0, 0));
        if (b.block == 1) EXPECT_EQ(std::make_pair(b.x, b.y), std::make_pair(64, 64));
    }
}

TEST(TexrepComponents, OverlappingPlacementsAreRefused) {
    const BlockSize blocks[] = {{1, 100, 100}, {2, 100, 100}};
    const PairOffset pairs[] = {{1, 2, 50, 50, 9}};  // would overlap: two layers, not one picture
    const auto comps = build_components(blocks, pairs);
    EXPECT_EQ(comps.size(), 2u);
}

TEST(TexrepComponents, StrongerEvidenceWinsWhenPairsDisagree) {
    const BlockSize blocks[] = {{1, 64, 64}, {2, 64, 64}, {3, 64, 64}};
    // 1-2 side by side, 2-3 side by side, but 1-3 claimed to be only 64 apart (contradiction with 128).
    const PairOffset pairs[] = {{1, 2, 64, 0, 10}, {2, 3, 64, 0, 10}, {1, 3, 64, 0, 2}};
    const auto comps = build_components(blocks, pairs);
    ASSERT_EQ(comps.size(), 1u);
    int x3 = -1;
    for (const PlacedBlock& b : comps[0].blocks)
        if (b.block == 3) x3 = b.x;
    EXPECT_EQ(x3, 128);
}

TEST(TexrepComponents, UnrelatedBlocksStaySeparateAndEveryBlockIsKept) {
    const BlockSize blocks[] = {{1, 64, 64}, {2, 64, 64}, {3, 64, 64}};
    const PairOffset pairs[] = {{1, 2, 64, 0, 3}};
    const auto comps = build_components(blocks, pairs);
    std::size_t n = 0;
    for (const Component& c : comps) n += c.blocks.size();
    EXPECT_EQ(comps.size(), 2u);
    EXPECT_EQ(n, 3u);
}

// ---- dump -----------------------------------------------------------------------------------------------------------

TEST(TexrepDump, BlocksDrawnTogetherAreStitchedAndWritten) {
    TextureDump dump(2);
    dump.add_variant(0xA, 0x1, 8, 4, std::vector<uint32_t>(32, 0xFF0000FF));
    dump.add_variant(0xB, 0x2, 8, 4, std::vector<uint32_t>(32, 0xFF00FF00));
    dump.add_variant(0xC, 0x3, 8, 4, std::vector<uint32_t>(32, 0xFFFF0000));  // drawn once only: dropped
    for (int frame = 0; frame < 3; ++frame) {
        const int camera = frame * 5;  // the whole picture scrolls: relative positions do not change
        dump.observe(0xA, 0x1, 10 - camera, 10);
        dump.observe(0xB, 0x2, 18 - camera, 10);
        if (frame == 0) dump.observe(0xC, 0x3, 0, 0);
        dump.end_frame();
    }
    const auto dir = temp_dir("dump");
    std::string error;
    ASSERT_TRUE(dump.write(dir, error)) << error;
    std::string text;
    {
        std::ifstream in(dir / "dump.toml");
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    DumpManifest m;
    ASSERT_TRUE(parse_dump_manifest(text, m, error)) << error;
    ASSERT_EQ(m.components.size(), 1u);
    EXPECT_EQ(m.components[0].width, 16);
    EXPECT_EQ(m.components[0].height, 4);
    ASSERT_EQ(m.textures.size(), 2u);
    for (const DumpTexture& t : m.textures) {
        EXPECT_TRUE(t.primary);
        EXPECT_EQ(t.x, t.block == 0xA ? 0 : 8);
        EXPECT_TRUE(std::filesystem::exists(dir / t.file));
    }
    Image comp;
    ASSERT_TRUE(read_png(dir / m.components[0].file, comp, error)) << error;
    EXPECT_EQ(comp.rgba[0], 0xFF0000FFu);
    EXPECT_EQ(comp.rgba[8], 0xFF00FF00u);
}

TEST(TexrepDump, ParallaxLayersAreNotJoined) {
    TextureDump dump(1);
    dump.add_variant(0xA, 1, 8, 8, std::vector<uint32_t>(64, 0xFF000001));
    dump.add_variant(0xB, 2, 8, 8, std::vector<uint32_t>(64, 0xFF000002));
    for (int frame = 0; frame < 4; ++frame) {
        dump.observe(0xA, 1, 20 - frame * 4, 0);
        dump.observe(0xB, 2, 40 - frame * 2, 0);  // moves at half the speed
        dump.end_frame();
    }
    const auto dir = temp_dir("parallax");
    std::string error;
    ASSERT_TRUE(dump.write(dir, error)) << error;
    std::ifstream in(dir / "dump.toml");
    DumpManifest m;
    ASSERT_TRUE(parse_dump_manifest(std::string(std::istreambuf_iterator<char>(in), {}), m, error)) << error;
    EXPECT_EQ(m.components.size(), 2u);
}

TEST(TexrepDump, ABlockDrawnAtTwoPlacesInOneFrameGivesNoEvidence) {
    TextureDump dump(1);
    dump.add_variant(0xA, 1, 8, 8, std::vector<uint32_t>(64, 1));
    dump.add_variant(0xB, 2, 8, 8, std::vector<uint32_t>(64, 2));
    dump.observe(0xA, 1, 0, 0);
    dump.observe(0xA, 1, 8, 0);
    dump.observe(0xB, 2, 16, 0);
    dump.end_frame();
    const auto dir = temp_dir("twice");
    std::string error;
    ASSERT_TRUE(dump.write(dir, error)) << error;
    std::ifstream in(dir / "dump.toml");
    DumpManifest m;
    ASSERT_TRUE(parse_dump_manifest(std::string(std::istreambuf_iterator<char>(in), {}), m, error)) << error;
    EXPECT_EQ(m.components.size(), 2u);
}

// ---- window helpers -------------------------------------------------------------------------------------------------

TEST(TexrepWindow, CropClampedRepeatsEdges) {
    Image src = solid(4, 4, 0);
    for (int i = 0; i < 16; ++i) src.rgba[static_cast<std::size_t>(i)] = static_cast<uint32_t>(i);
    const Image c = crop_clamped(src, -2, -2, 8, 8);
    EXPECT_EQ(c.rgba[0], 0u);                // above-left corner repeats pixel (0, 0)
    EXPECT_EQ(c.rgba[2 * 8 + 2], 0u);        // the real (0, 0)
    EXPECT_EQ(c.rgba[2 * 8 + 3], 1u);
    EXPECT_EQ(c.rgba[7 * 8 + 7], 15u);       // below-right corner repeats (3, 3)
}

TEST(TexrepWindow, ContextWindowShowsTheVariantOverItsPlace) {
    Image comp = solid(32, 16, 0xFF111111);
    paste(comp, solid(8, 8, 0xFF222222), 8, 4);
    const Image variant = solid(8, 8, 0xFF333333);
    const Image win = context_window(comp, 8, 4, variant, 3);
    EXPECT_EQ(win.w, 14);
    EXPECT_EQ(win.h, 14);
    EXPECT_EQ(win.rgba[3 * 14 + 3], 0xFF333333u);
    EXPECT_EQ(win.rgba[0], 0xFF111111u) << "the surroundings come from the component";
}

TEST(TexrepWindow, UpscalingAWindowAndCroppingGivesTheBlockUpscaled) {
    Image block;
    block.w = 6;
    block.h = 5;
    for (int i = 0; i < 30; ++i) block.rgba.push_back(0xFF000000u | static_cast<uint32_t>(i * 8));
    Image comp = solid(20, 20, 0xFF808080);
    paste(comp, block, 7, 9);
    const int pad = 4, scale = 3;
    const Image up = replicate(context_window(comp, 7, 9, block, pad), scale);
    const Image cropped = crop_block(up, pad, scale, block.w, block.h);
    EXPECT_EQ(cropped.rgba, replicate(block, scale).rgba);
}

TEST(TexrepWindow, BleedFillsTransparentPixelsFromTheirOpaqueNeighbours) {
    Image img = solid(8, 1, 0);
    img.rgba[3] = 0xFF0000FF;  // one opaque red pixel
    bleed_transparent(img, 2);
    EXPECT_EQ(img.rgba[3], 0xFF0000FFu) << "opaque pixels do not change";
    EXPECT_EQ(img.rgba[2], 0xFF0000FFu);
    EXPECT_EQ(img.rgba[4], 0xFF0000FFu);
    EXPECT_EQ(img.rgba[1], 0xFF0000FFu);
    EXPECT_EQ(img.rgba[5], 0xFF0000FFu);
    // Beyond the reach: the average of the opaque pixels, and everything ends up opaque.
    EXPECT_EQ(img.rgba[0], 0xFF0000FFu);
    EXPECT_EQ(img.rgba[7], 0xFF0000FFu);
    for (uint32_t p : img.rgba) EXPECT_EQ(p >> 24, 0xFFu);
}

TEST(TexrepWindow, BleedWithoutAnyOpaquePixelGivesGrey) {
    Image img = solid(3, 3, 0);
    bleed_transparent(img, 4);
    for (uint32_t p : img.rgba) EXPECT_EQ(p, 0xFF808080u);
}

TEST(TexrepWindow, BleedAveragesSeveralNeighbours) {
    Image img = solid(3, 1, 0);
    img.rgba[0] = 0xFF000000u | 100;
    img.rgba[2] = 0xFF000000u | 200;
    bleed_transparent(img, 1);
    EXPECT_EQ(img.rgba[1] & 255, 150u);
}

TEST(TexrepWindow, BoxDownscaleAverages) {
    Image src = solid(4, 2, 0);
    src.rgba = {0xFF000000, 0xFF000000, 0xFF646464, 0xFF646464, 0xFF000000, 0xFF000000, 0xFF646464, 0xFF646464};
    const Image d = box_downscale(src, 2);
    ASSERT_EQ(d.w, 2);
    ASSERT_EQ(d.h, 1);
    EXPECT_EQ(d.rgba[0], 0xFF000000u);
    EXPECT_EQ(d.rgba[1], 0xFF646464u);
    EXPECT_EQ(box_downscale(src, 1).rgba, src.rgba);
}

// ---- pack generation ------------------------------------------------------------------------------------------------

TEST(TexrepGenerate, BuildsAPackFromADumpAndExtendsIt) {
    TextureDump dump(1);
    Image a = solid(16, 8, 0xFF0000FF), b = solid(16, 8, 0xFF00FF00);
    a.rgba[3] = 0xFFFFFFFF;
    dump.add_variant(0xA, 1, 16, 8, a.rgba);
    dump.add_variant(0xB, 2, 16, 8, b.rgba);
    dump.add_variant(0xB, 3, 16, 8, std::vector<uint32_t>(128, 0xFF123456));  // a second palette of block B
    dump.observe(0xA, 1, 0, 0);
    dump.observe(0xB, 2, 16, 0);
    dump.end_frame();
    const auto dump_dir = temp_dir("gen_dump");
    std::string error;
    ASSERT_TRUE(dump.write(dump_dir, error)) << error;

    const auto out = temp_dir("gen_pack");
    GenerateOptions options;
    options.name = "mock";
    options.scale = 2;
    options.model_scale = 4;
    options.pad = 4;
    options.batch = 2;
    std::size_t images_seen = 0;
    BatchUpscaler up = [&](const std::vector<Image>& in, std::vector<Image>& result, std::string&) {
        result.clear();
        for (const Image& i : in) result.push_back(replicate(i, 4));
        images_seen += in.size();
        return true;
    };
    GenerateReport report;
    ASSERT_TRUE(generate_pack({dump_dir}, out, options, up, report, error)) << error;
    EXPECT_EQ(report.generated, 3u);
    EXPECT_EQ(images_seen, 3u);

    auto pack = TexturePack::load(out, error);
    ASSERT_TRUE(pack) << error;
    EXPECT_EQ(pack->scale(), 2);
    EXPECT_EQ(pack->size(), 3u);
    const PackEntry* ea = pack->find(0xA, 1);
    ASSERT_NE(ea, nullptr);
    Image img;
    ASSERT_TRUE(pack->load_image(*ea, img, error)) << error;
    EXPECT_EQ(img.w, 32);
    EXPECT_EQ(img.h, 16);
    EXPECT_EQ(img.rgba[0] & 0xFFFFFF, 0x0000FFu) << "block A's own texels, red in the low byte";
    EXPECT_EQ(img.rgba[6] & 0xFFFFFF, 0xFFFFFFu);
    EXPECT_NE(pack->find(0xB, 3), nullptr) << "palette variants get their own image";

    // A second run with the same dump generates nothing new.
    GenerateReport again;
    ASSERT_TRUE(generate_pack({dump_dir}, out, options, up, again, error)) << error;
    EXPECT_EQ(again.generated, 0u);
    EXPECT_EQ(again.skipped_existing, 3u);
    EXPECT_EQ(images_seen, 3u);
}

TEST(TexrepGenerate, RefusesAScaleThatDoesNotDivideTheModelScale) {
    GenerateOptions options;
    options.scale = 3;
    options.model_scale = 4;
    GenerateReport report;
    std::string error;
    EXPECT_FALSE(generate_pack({}, temp_dir("gen_bad"), options, nullptr, report, error));
    EXPECT_FALSE(error.empty());
}

TEST(TexrepGenerate, UpscalerFailureIsReported) {
    TextureDump dump(1);
    dump.add_variant(0xA, 1, 8, 8, std::vector<uint32_t>(64, 0xFF000000));
    dump.observe(0xA, 1, 0, 0);
    dump.end_frame();
    const auto dump_dir = temp_dir("gen_fail_dump");
    std::string error;
    ASSERT_TRUE(dump.write(dump_dir, error)) << error;
    GenerateOptions options;
    GenerateReport report;
    BatchUpscaler fail = [](const std::vector<Image>&, std::vector<Image>&, std::string& e) {
        e = "device lost";
        return false;
    };
    EXPECT_FALSE(generate_pack({dump_dir}, temp_dir("gen_fail_pack"), options, fail, report, error));
    EXPECT_EQ(error, "device lost");
}
