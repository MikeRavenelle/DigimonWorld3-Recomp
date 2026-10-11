#include "dw3/vram_regions.h"

#include <algorithm>

namespace dw3 {

Region Region::intersection(const Region& o) const {
    return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
}

Region Region::united(const Region& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
}

Region region_from(const VramRect& r) {
    auto axis = [](int start, int len, int size, int& a, int& b) {
        if (len >= size) {
            a = 0;
            b = size;
            return;
        }
        a = ((start % size) + size) % size;
        b = a + len;
        if (b > size) {  // wraps: take the whole axis
            a = 0;
            b = size;
        }
    };
    Region out;
    axis(r.x, r.w, kVramWidth, out.x0, out.x1);
    axis(r.y, r.h, kVramHeight, out.y0, out.y1);
    return out;
}

void RegionSet::add(const Region& in) {
    if (in.empty()) return;
    Region r = in;
    // Absorb every member that overlaps the new rectangle (growing it may overlap more of them).
    for (bool merged = true; merged;) {
        merged = false;
        for (std::size_t i = 0; i < regions_.size(); ++i) {
            if (regions_[i].intersects(r)) {
                r = r.united(regions_[i]);
                regions_.erase(regions_.begin() + static_cast<std::ptrdiff_t>(i));
                merged = true;
                break;
            }
        }
    }
    regions_.push_back(r);
    if (regions_.size() > kMaxRegions) {
        Region all;
        for (const Region& m : regions_) all = all.united(m);
        regions_.assign(1, all);
    }
}

bool RegionSet::intersects(const Region& r) const {
    return std::any_of(regions_.begin(), regions_.end(), [&](const Region& m) { return m.intersects(r); });
}

std::vector<Region> RegionSet::take_intersecting(const Region& r) {
    std::vector<Region> out;
    for (std::size_t i = 0; i < regions_.size();) {
        if (regions_[i].intersects(r)) {
            out.push_back(regions_[i]);
            regions_.erase(regions_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    return out;
}

std::vector<Region> RegionSet::take_all() {
    std::vector<Region> out;
    out.swap(regions_);
    return out;
}

}  // namespace dw3
