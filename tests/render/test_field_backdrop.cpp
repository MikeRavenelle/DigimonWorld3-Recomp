// Seamless field backgrounds: working out the map and scroll from the sprites the game drew, and listing the pieces it
// did not. Runs on synthetic maps; nothing here needs the game's files.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "dw3/texrep/field_backdrop.h"
#include "dw3/texrep/stage_library.h"

using namespace dw3::texrep;

namespace {

StageTile make_tile(int col, int row, uint64_t block, std::vector<StagePiece> pieces) {
    StageTile t;
    t.col = col;
    t.row = row;
    t.width_words = 64;  // 128 texels
    t.rows = 128;
    t.block_hash = block;
    t.clut_hash = 7;
    t.pieces = std::move(pieces);
    return t;
}

StagePiece piece(int layer, int x, int y, int ax, int ay, int w, int h) {
    StagePiece p;
    p.layer = layer;
    p.x = x;
    p.y = y;
    p.ax = ax;
    p.ay = ay;
    p.w = w;
    p.h = h;
    return p;
}

// A 4x3 cell map. Every cell has a full main layer piece with the picture of its own (hash 100 + index); the cell (2,1)
// also has a back piece and a front piece.
StagePack make_map() {
    StagePack pack;
    pack.cols = 4;
    pack.rows = 3;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) {
            std::vector<StagePiece> pieces = {piece(0, 0, 0, 0, 0, 128, 128)};
            if (col == 2 && row == 1) {
                pieces.push_back(piece(2, 10, 20, 0, 0, 30, 30));
                pieces.push_back(piece(1, 60, 60, 0, 0, 40, 40));
            }
            pack.tiles.push_back(make_tile(col, row, 100 + static_cast<uint64_t>(row * 4 + col), pieces));
        }
    return pack;
}

std::shared_ptr<StageLibrary> make_library() {
    auto lib = std::make_shared<StageLibrary>();
    lib->add_map("S001", make_map());
    lib->finish();
    return lib;
}

BackdropSprite keyed(const StageLibrary&, int col, int row, int x, int y, int w = 128, int h = 128, int bu = 0, int bv = 0) {
    BackdropSprite s;
    s.x = x;
    s.y = y;
    s.w = w;
    s.h = h;
    s.keyed = true;
    s.block = 100 + static_cast<uint64_t>(row * 4 + col);
    s.clut = 7;
    s.bu = bu;
    s.bv = bv;
    return s;
}

const BackdropRect kScreen{0, 0, 320, 240};

}  // namespace

TEST(FieldBackdrop, ScrollFromSprites) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    // Scroll (200, 100): cell (1,1) lies at (128 - 200, 128 - 100) = (-72, 28).
    EXPECT_TRUE(b.add_sprite(keyed(*lib, 1, 1, -72, 28)));
    EXPECT_TRUE(b.add_sprite(keyed(*lib, 2, 1, 56, 28)));
    ASSERT_TRUE(b.resolve());
    EXPECT_EQ(b.map(), 0);
    EXPECT_EQ(b.scroll_x(), 200);
    EXPECT_EQ(b.scroll_y(), 100);
}

TEST(FieldBackdrop, ListsOnlyMissingMainPieces) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    // Scroll (200, 100) shows cells columns 1..3 (x -72..312) and rows 0..2 (y -100..284); the game drew two of them.
    b.add_sprite(keyed(*lib, 1, 1, -72, 28));
    b.add_sprite(keyed(*lib, 2, 1, 56, 28));
    const auto fills = b.take_missing(layers::kMain, kScreen);
    // Cells (1..3, 0..2) are visible: nine in all, minus the two drawn.
    EXPECT_EQ(fills.size(), 7u);
    for (const BackdropFill& f : fills) {
        const auto& tile = lib->map(f.map).tiles[f.tile];
        EXPECT_FALSE(tile.col == 1 && tile.row == 1);
        EXPECT_FALSE(tile.col == 2 && tile.row == 1);
        EXPECT_EQ(f.x, tile.col * 128 - 200);
        EXPECT_EQ(f.y, tile.row * 128 - 100);
    }
    // Asking again in the same frame lists nothing new.
    EXPECT_TRUE(b.take_missing(layers::kMain, kScreen).empty());
}

TEST(FieldBackdrop, PiecesOutsideTheViewAreNotListed) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    b.add_sprite(keyed(*lib, 1, 1, -72, 28));
    const BackdropRect small{0, 0, 10, 10};  // only cell (1,0) reaches into this corner: (-72..56, -100..28)
    const auto fills = b.take_missing(layers::kMain, small);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(lib->map(fills[0].map).tiles[fills[0].tile].col, 1);
    EXPECT_EQ(lib->map(fills[0].map).tiles[fills[0].tile].row, 0);
}

TEST(FieldBackdrop, AnimatedPieceCountsAsDrawn) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    b.add_sprite(keyed(*lib, 1, 1, -72, 28));
    // A sprite of another picture with the size and place of cell (2,1)'s main piece: an animated tile.
    BackdropSprite animated;
    animated.x = 56;
    animated.y = 28;
    animated.w = 128;
    animated.h = 128;
    animated.keyed = true;
    animated.block = 9999;
    animated.clut = 1;
    EXPECT_TRUE(b.add_sprite(animated));  // fits a piece of the map at the scroll the others give
    const auto fills = b.take_missing(layers::kMain, kScreen);
    for (const BackdropFill& f : fills) {
        const auto& tile = lib->map(f.map).tiles[f.tile];
        EXPECT_FALSE(tile.col == 2 && tile.row == 1);
    }
}

TEST(FieldBackdrop, UnknownSpriteIsNotBackground) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    b.add_sprite(keyed(*lib, 1, 1, -72, 28));
    BackdropSprite character;
    character.x = 100;
    character.y = 100;
    character.w = 48;
    character.h = 67;
    EXPECT_FALSE(b.add_sprite(character));
}

TEST(FieldBackdrop, DueGroupsAndOrder) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    EXPECT_EQ(b.due(), 0u);
    b.add_sprite(keyed(*lib, 1, 1, -72, 28));
    EXPECT_EQ(b.due(), layers::kBackground);
    // Cell (2,1) holds back and front pieces; its main piece is not drawn.
    const auto bg = b.take_missing(b.due(), kScreen);
    EXPECT_EQ(b.due(), 0u);
    // Back pieces come before main pieces.
    int last_back = -1, first_main = static_cast<int>(bg.size());
    for (std::size_t i = 0; i < bg.size(); ++i) {
        if (bg[i].piece->layer == 2) last_back = static_cast<int>(i);
        if (bg[i].piece->layer == 0) first_main = std::min(first_main, static_cast<int>(i));
        EXPECT_NE(bg[i].piece->layer, 1);
    }
    EXPECT_GE(last_back, 0);
    EXPECT_LT(last_back, first_main);
    // A foreground sprite makes the front group due.
    BackdropSprite front = keyed(*lib, 2, 1, 56, 28, 40, 40);
    // (the picture of cell (2,1) is shown by its front piece at the picture's top-left corner)
    front.x = 128 * 2 + 60 - 200;
    front.y = 128 + 60 - 100;
    EXPECT_TRUE(b.add_sprite(front));
    EXPECT_EQ(b.due(), layers::kFront);
}

TEST(FieldBackdrop, BackPieceRedrawsMainPiecesOverIt) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    // Cell (2,1) is missing; its back piece lies at map (266, 148) and covers main pieces of cells that the game drew
    // earlier only where they overlap it: here none do, so nothing is redrawn. Make cell (2,0)'s main piece reach it
    // by drawing that cell as the game did and moving the scroll so that rows overlap: the pieces are 128 high, the
    // back piece lies inside cell (2,1) only, so there is no overlap and the result has just the missing pieces.
    b.add_sprite(keyed(*lib, 2, 0, 56, -100));
    b.add_sprite(keyed(*lib, 1, 1, -72, 28));
    const auto fills = b.take_missing(layers::kBackground, kScreen);
    for (const BackdropFill& f : fills) {
        const auto& tile = lib->map(f.map).tiles[f.tile];
        EXPECT_FALSE(tile.col == 2 && tile.row == 0);
    }
}

TEST(FieldBackdrop, TieBrokenByPreviousFrame) {
    // Cells 0..2 share one picture; cell 3 has its own.
    StagePack pack;
    pack.cols = 4;
    pack.rows = 1;
    for (int col = 0; col < 4; ++col) pack.tiles.push_back(make_tile(col, 0, col < 3 ? 500 : 501, {piece(0, 0, 0, 0, 0, 128, 128)}));
    auto lib = std::make_shared<StageLibrary>();
    lib->add_map("S002", pack);
    lib->finish();
    auto sprite = [](uint64_t block, int x) {
        BackdropSprite s;
        s.keyed = true;
        s.block = block;
        s.clut = 7;
        s.w = s.h = 128;
        s.x = x;
        return s;
    };
    FieldBackdrop b(lib);
    b.add_sprite(sprite(500, 24));
    EXPECT_FALSE(b.resolve());  // three scrolls fit equally and there is nothing to choose by
    b.begin_frame();
    b.add_sprite(sprite(501, 3 * 128 - 100));  // scroll 100
    ASSERT_TRUE(b.resolve());
    EXPECT_EQ(b.scroll_x(), 100);
    b.begin_frame();
    b.add_sprite(sprite(500, 128 - 104));  // fits scrolls -24, 104 and 232: the one nearest to 100 wins
    ASSERT_TRUE(b.resolve());
    EXPECT_EQ(b.scroll_x(), 104);
}

TEST(FieldBackdrop, ForgetsMapAfterIdleFrames) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    b.add_sprite(keyed(*lib, 1, 1, -72, 28));
    ASSERT_TRUE(b.resolve());
    b.begin_frame();
    // A sprite of the right shape and place is background while the previous scroll is remembered...
    BackdropSprite fits;
    fits.x = -72;
    fits.y = 28;
    fits.w = fits.h = 128;
    EXPECT_TRUE(b.add_sprite(fits));
    // ...and no longer once the field has not been seen for a while.
    for (int i = 0; i < 40; ++i) b.begin_frame();
    EXPECT_FALSE(b.add_sprite(fits));
}

TEST(FieldBackdrop, PlaceholderSquaresLieOnTheCellGrid) {
    auto lib = make_library();
    FieldBackdrop b(lib);
    b.add_sprite(keyed(*lib, 1, 1, -72, 28));  // scroll (200, 100)
    EXPECT_TRUE(b.is_placeholder(-72, 28, 64, 64));
    EXPECT_TRUE(b.is_placeholder(-8, 92, 64, 64));
    EXPECT_FALSE(b.is_placeholder(-71, 28, 64, 64));
    EXPECT_FALSE(b.is_placeholder(-72, 28, 32, 64));
    // Beyond the map's left edge (map x < 0) there is no cell.
    EXPECT_FALSE(b.is_placeholder(-264, 28, 64, 64));
}

TEST(FieldBackdrop, NothingWithoutLibrary) {
    FieldBackdrop b(nullptr);
    BackdropSprite s;
    s.keyed = true;
    s.w = s.h = 128;
    EXPECT_FALSE(b.add_sprite(s));
    EXPECT_FALSE(b.resolve());
    EXPECT_TRUE(b.take_missing(layers::kMain, kScreen).empty());
}

TEST(StageLibrary, IndexesKeysAndLoadsImages) {
    StagePack pack = make_map();
    for (StageTile& t : pack.tiles) {
        t.words.assign(static_cast<std::size_t>(t.width_words) * t.rows, 0x0101);
        t.clut[1] = 0x7FFF;
    }
    auto lib = std::make_shared<StageLibrary>([&](const std::string& name, StagePack& out, std::string&) {
        if (name != "S009") return false;
        out = pack;
        return true;
    });
    lib->add_map("S009", pack);
    lib->finish();
    EXPECT_TRUE(lib->ready());
    const auto refs = lib->refs(105, 7);
    ASSERT_EQ(refs.size(), 1u);
    EXPECT_EQ(lib->map(refs[0].map).tiles[refs[0].tile].col, 1);
    EXPECT_EQ(lib->map(refs[0].map).tiles[refs[0].tile].row, 1);
    EXPECT_TRUE(lib->refs(105, 8).empty());
    Image img;
    ASSERT_TRUE(lib->tile_image(0, refs[0].tile, img));
    EXPECT_EQ(img.w, 128);
    EXPECT_EQ(img.h, 128);
    EXPECT_EQ(img.rgba[0] >> 24, 255u);  // palette entry 1 is opaque
}
