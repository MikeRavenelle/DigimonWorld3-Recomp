#pragma once

// Seamless field backgrounds, GPU independent part.
//
// The game keeps a window of about twelve 128x128 cells of the map in VRAM and draws, every frame, the pieces of the
// cells in that window as opaque sprites: layer 2 (far scenery), layer 0 (the ground) and, between the characters,
// layer 1 (what stands in front of them). The window lags behind the camera, so a cell that has just come into view
// is not drawn for a few frames and the edge of the picture shows black or stale blocks.
//
// The field map files (stage_library.h) say where every piece lies in the map, and the sprites the game does draw give
// away where the camera is: a sprite whose picture and shape are those of a piece sits at the piece's map position minus
// the scroll, and the keys of its picture say which cell and which map it belongs to. From the sprites of a frame this
// class works out the map and the scroll, and lists the visible pieces the game did not draw, for the renderer to
// draw itself. Pieces the game drew are left alone, including animated ones (a sprite of the right size at the right
// place counts as drawn whatever picture it uses), so the picture never gets worse than the game's own.
//
// Order. Different cells of one layer do not overlap in opaque pixels, so the missing pieces of the background layers
// may be drawn after the game's own; they are listed at the first draw that is not part of the background and the
// foreground pieces after the foreground sprites, which keeps the characters in between where they were.

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "dw3/texrep/stage_library.h"

namespace dw3::texrep {

namespace layers {
constexpr unsigned kMain = 1u << 0;   // piece layer 0
constexpr unsigned kFront = 1u << 1;  // piece layer 1, drawn over the characters
constexpr unsigned kBack = 1u << 2;   // piece layer 2, drawn under the main layer
constexpr unsigned kBackground = kMain | kBack;
}  // namespace layers

struct BackdropRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // half open, screen pixels
    bool intersects(const BackdropRect& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
};

// An opaque sprite that reads 8-bit texels one to one, as the game drew it. Positions are screen pixels (the draw
// offset removed).
struct BackdropSprite {
    int x = 0, y = 0, w = 0, h = 0;
    bool keyed = false;                 // the sprite lies inside one picture the renderer tracks
    uint64_t block = 0, clut = 0;       // that picture's keys
    int bu = 0, bv = 0;                 // the sprite's texel position inside the picture
    uint32_t color = 0x808080;          // the sprite's colour modulation, 0xBBGGRR
};

// A piece to draw.
struct BackdropFill {
    uint32_t map = 0, tile = 0;
    const StagePiece* piece = nullptr;
    int x = 0, y = 0;                   // screen position of the piece's top-left corner
    uint32_t color = 0x808080;
};

// Widescreen fields. The wide copy shows the view widened by `margin` columns on each side; the pieces listed for it are
// split between the frame buffer (what the game's own 4:3 picture is completed with) and the copy.
struct WideBackdropPlan {
    std::vector<BackdropFill> frame;  // pieces to draw into the frame buffer: those that touch the 4:3 view, only with `seamless`
    std::vector<BackdropFill> wide;   // pieces to draw into the copy: all with `seamless`, otherwise those that reach a margin
};
BackdropRect widened_view(const BackdropRect& view, int margin);
WideBackdropPlan plan_wide_backdrop(const std::vector<BackdropFill>& listed, const BackdropRect& view, bool seamless);

// Floor division, for scroll positions that can be negative.
constexpr int floor_div(int a, int b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

// The cells of a map whose picture is likely to be wanted soon: those within one cell of `view` (screen pixels, with the
// map scrolled by `scroll_x`, `scroll_y`), nearest to the middle of the view first.
std::vector<uint32_t> tiles_to_prefetch(const StageLibrary::Map& map, int scroll_x, int scroll_y, const BackdropRect& view);

class FieldBackdrop {
public:
    explicit FieldBackdrop(std::shared_ptr<const StageLibrary> library) : library_(std::move(library)) {}

    // Starts a frame: forgets what was drawn but remembers the map and scroll of the last one.
    void begin_frame();
    // Records a sprite the game drew. Returns true when it belongs to the background (a known picture at a position that
    // fits the map), false for anything else. A false return is a draw that is not background.
    bool add_sprite(const BackdropSprite& sprite);
    // The layer groups with pieces waiting to be listed: sprites of the resolved map were seen since `take_missing` last
    // ran for their group. (Background: layers kBackground, foreground: kFront.)
    unsigned due();

    // Works out the map and scroll from the sprites seen. False when they do not say.
    bool resolve();
    int map() const { return map_; }
    int scroll_x() const { return sx_; }
    int scroll_y() const { return sy_; }

    // Whether a 64x64 sprite at this screen position is one of the squares the game draws over cells it has not loaded:
    // they lie on the 64 pixel grid of the map. Needs the map and scroll (see resolve); false when they are not known.
    bool is_placeholder(int x, int y, int w, int h);

    // The visible pieces of the given layers that the game did not draw this frame, in drawing order, and with `layers`
    // containing both background layers also the pieces of the main layer that a listed back piece may lie over.
    // Marks those layers as handled for this frame.
    std::vector<BackdropFill> take_missing(unsigned layers, const BackdropRect& view);

private:
    struct Hypothesis {
        int map = 0, sx = 0, sy = 0, votes = 0;
    };
    struct Match {
        int hypothesis = 0;  // index into votes_
        int layer = 0;
        uint32_t color = 0;
    };
    static uint64_t geometry_key(int x, int y, int w, int h);
    bool fits_known_piece(const BackdropSprite& s) const;

    std::shared_ptr<const StageLibrary> library_;
    std::vector<Hypothesis> votes_;
    std::vector<Match> matches_;
    // What the game drew at each piece shape and place; the keys of its picture when it is one the library knows.
    struct Drawn {
        bool keyed = false;
        uint64_t block = 0, clut = 0;
    };
    std::unordered_map<uint64_t, Drawn> drawn_;
    // The game drew this piece's place with the picture of another cell of the map (its window has not caught up yet).
    bool is_stale(const Drawn& d, uint32_t tile) const;
    std::size_t consumed_[2] = {0, 0};  // matches_ entries already listed for the background and front groups
    int picked_ = -1;
    int idle_frames_ = 0;
    bool resolved_ = false;
    int map_ = -1, sx_ = 0, sy_ = 0;
    int prev_map_ = -1, prev_sx_ = 0, prev_sy_ = 0;
    uint32_t last_color_[3] = {0x808080, 0x808080, 0x808080};
};

}  // namespace dw3::texrep
