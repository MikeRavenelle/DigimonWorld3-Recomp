#pragma once

// Sub-pixel vertex positions for the GPU renderer ("precise geometry").
//
// The GTE projects vertices to integers (SX, SY) and the game later copies those words into GP0 packets, so the
// GPU only ever sees integer vertices. When enabled, the GTE also records the position it had before truncation,
// plus the depth, keyed by the packed SXY word it produced. The GPU front end looks each polygon vertex word up
// here and, on a hit, passes the precise position on to the renderer.
//
// Nothing in the generated code is involved: the words travel through RAM and DMA untouched, so a vertex that
// still carries the word the GTE produced is found by value. A word the game changed (offsets, clipping, software
// projection) simply misses and the vertex stays integer. A hit is only used when the integer word equals the
// truncated precise position, and entries older than a few frames are ignored, so a stale or colliding
// entry can shift a vertex by less than a pixel but never replaces it with an unrelated position.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace dw3 {

struct PreciseVertex {
    float x = 0, y = 0;  // projected position in pixels, before truncation to the integer SX/SY
    float w = 0;         // depth (SZ3) for perspective-correct interpolation
};

class PreciseVertexCache {
public:
    // Entries older than this many frames are not returned. The game draws a frame's polygons one to three video
    // frames after the GTE projected them (it runs at 30 frames per second or less), so an entry has to outlive that.
    static constexpr uint64_t kMaxAge = 4;

    // Relative change of depth between two projections to one pixel above which they are taken for different vertices.
    static constexpr double kMaxDepthStep = 0.25;

    PreciseVertexCache();

    // Frame number source (the video frame/field counter). Without one every entry stays current.
    void set_clock(std::function<uint64_t()> clock) { clock_ = std::move(clock); }

    // The key of a vertex word: the 11-bit X and Y fields the GPU uses.
    static constexpr uint32_t key_of(uint32_t word) { return word & 0x07FF07FFu; }

    // Records the precise position behind the integer vertex (sx, sy). Ignored unless the integer is the floor of
    // the precise position (the GTE truncates) and the depth is positive.
    void record(int32_t sx, int32_t sy, double x, double y, double w);

    // Looks up a GP0 vertex word. Returns false on a miss, a stale entry, a mismatch with the integer word, or a pixel
    // that unrelated vertices share (their depths differ too much for one entry to be right for both).
    bool find(uint32_t word, PreciseVertex& out) const;

    // Drops every entry. The cache is transient (entries expire after kMaxAge frames) and is not part of a save
    // state; a restored machine starts with it cleared.
    void clear();

    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    struct Entry {
        uint32_t key = 0;
        uint64_t frame = 0;
        bool valid = false;
        uint64_t contested = 0;  // frame + 1 when another vertex with a very different depth last projected to this pixel
        PreciseVertex v;
    };
    static constexpr std::size_t kSets = 16384;  // two ways each

    uint64_t now() const { return clock_ ? clock_() : 0; }
    static std::size_t set_of(uint32_t key) { return static_cast<std::size_t>((key * 0x9E3779B1u) >> 18); }  // top 14 bits

    std::vector<Entry> entries_;
    std::function<uint64_t()> clock_;
    mutable uint64_t hits_ = 0, misses_ = 0;
};

}  // namespace dw3
