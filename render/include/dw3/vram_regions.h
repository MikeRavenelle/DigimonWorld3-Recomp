#pragma once

// Rectangle bookkeeping for the GPU renderer's VRAM copies. A RegionSet holds a small list of disjoint
// rectangles (half-open, VRAM pixel units); when it grows past its capacity it collapses to their bounding box,
// which is conservative: it only ever reports more overlap, never less.

#include <vector>

#include "dw3/gpu.h"

namespace dw3 {

struct Region {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1)

    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool intersects(const Region& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
    Region intersection(const Region& o) const;
    Region united(const Region& o) const;
    bool operator==(const Region&) const = default;
};

constexpr Region kWholeVram{0, 0, kVramWidth, kVramHeight};

// Converts a VRAM rectangle that may wrap around the edges into one non-wrapping Region. A rectangle that does
// wrap is widened to cover the whole axis it wraps on.
Region region_from(const VramRect& r);

class RegionSet {
public:
    static constexpr std::size_t kMaxRegions = 48;

    void add(const Region& r);
    bool intersects(const Region& r) const;
    // Removes and returns every member that overlaps r.
    std::vector<Region> take_intersecting(const Region& r);
    std::vector<Region> take_all();
    void clear() { regions_.clear(); }
    bool empty() const { return regions_.empty(); }
    const std::vector<Region>& regions() const { return regions_; }

private:
    std::vector<Region> regions_;
};

}  // namespace dw3
