#pragma once

// Widescreen geometry. The game draws 320x240 frames into two buffers in VRAM and the columns to their right hold
// textures, so a wider picture cannot live there. The GPU renderer instead keeps a second, wider copy of the frame
// buffers in extra columns to the right of VRAM (x >= kOriginX): the centre of that copy is the original picture
// and `margin` columns on each side show more of the world. Everything drawn into a frame buffer is drawn into the
// copy too, shifted by `shift()`, so 2D elements stay where the game put them (centred) while 3D geometry the game
// already submits beyond the 4:3 edges fills the margins.

#include <algorithm>
#include <cstdint>

namespace dw3 {

enum class WideAspect : uint8_t { Off, Ratio16x9, Ratio16x10, Ratio21x9 };

// What the wide frame copy shows (see the renderer). Battle: every draw is mirrored and the copy is always shown.
// Field: the margins are filled with the pieces of the map, and a frame whose map could not be resolved is shown at 4:3.
enum class WideMode : uint8_t { Off, Battle, Field };

// The game mode word (0x800554D0) that selects the wide mode: 2 is the field, 6 a battle; everything else (menus,
// transitions, results, cutscene modes) stays at 4:3.
constexpr WideMode wide_mode_for_game_mode(uint32_t game_mode) {
    return game_mode == 6 ? WideMode::Battle : game_mode == 2 ? WideMode::Field : WideMode::Off;
}

struct AspectRatio {
    int num, den;
};

constexpr AspectRatio aspect_ratio(WideAspect a) {
    switch (a) {
        case WideAspect::Ratio16x9: return {16, 9};
        case WideAspect::Ratio16x10: return {16, 10};
        case WideAspect::Ratio21x9: return {21, 9};
        default: return {4, 3};
    }
}

// Columns added on each side of a frame_w x frame_h picture to reach the aspect, rounded down to a multiple of 4
// (keeps the dither pattern aligned); 0 when off or when the aspect is not wider than the frame.
constexpr int wide_margin(WideAspect a, int frame_w, int frame_h) {
    if (a == WideAspect::Off) return 0;
    const AspectRatio r = aspect_ratio(a);
    const int wide_w = frame_h * r.num / r.den;
    if (wide_w <= frame_w) return 0;
    return (wide_w - frame_w) / 2 / 4 * 4;
}

struct WideFrame {
    static constexpr int kOriginX = 1024;  // first column of the wide copy
    static constexpr int kMaxColumns = 576;
    int frame_w = 320, frame_h = 240;
    int margin = 0;

    bool enabled() const { return margin > 0; }
    int width() const { return frame_w + 2 * margin; }
    // Translation from a frame-buffer x to the wide copy.
    int shift() const { return kOriginX + margin; }
    int map_x(int x) const { return x + shift(); }
    // Like map_x, but a coordinate on or beyond an edge of the frame goes to the matching edge of the wide copy,
    // so shapes that fill the frame (fades, backdrops) fill the margins as well.
    int snap_x(int x) const {
        if (x <= 0) return kOriginX;
        if (x >= frame_w) return kOriginX + width();
        return map_x(x);
    }
    // Whether a drawing area lies inside one of the two frame buffers (stacked at y = 0 and y = 256).
    bool in_frame(int x1, int y1, int x2, int y2) const {
        if (x1 < 0 || x2 >= frame_w) return false;
        for (int top : {0, 256})
            if (y1 >= top && y2 < top + frame_h) return true;
        return false;
    }
    bool covers_width(int x1, int x2) const { return x1 <= 0 && x2 >= frame_w - 1; }
    // Whether a polygon with these wide-copy x coordinates covers columns in one of the margins.
    bool reaches_margin(int xa, int xb, int xc) const {
        const int lo = std::min({xa, xb, xc}), hi = std::max({xa, xb, xc});
        const int left_end = kOriginX + margin, right_start = kOriginX + margin + frame_w, right_end = kOriginX + width();
        return (lo < left_end && hi >= kOriginX) || (hi >= right_start && lo < right_end);
    }
};

constexpr WideFrame make_wide_frame(WideAspect a, int frame_w = 320, int frame_h = 240) {
    return WideFrame{frame_w, frame_h, wide_margin(a, frame_w, frame_h)};
}

}  // namespace dw3
