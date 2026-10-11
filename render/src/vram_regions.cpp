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

int split_wrapped(const VramRect& r, Region out[4]) {
    if (r.w <= 0 || r.h <= 0) return 0;
    auto axis = [](int start, int len, int size, int a[2], int b[2]) {
        len = std::min(len, size);
        const int s = ((start % size) + size) % size;
        if (s + len <= size) {
            a[0] = s;
            b[0] = s + len;
            return 1;
        }
        a[0] = s;
        b[0] = size;
        a[1] = 0;
        b[1] = s + len - size;
        return 2;
    };
    int xa[2], xb[2], ya[2], yb[2];
    const int nx = axis(r.x, r.w, kVramWidth, xa, xb);
    const int ny = axis(r.y, r.h, kVramHeight, ya, yb);
    int n = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) out[n++] = {xa[i], ya[j], xb[i], yb[j]};
    return n;
}

bool copy_has_row_hazard(const VramRect& src, int dx, int dy) {
    if (src.w <= 0 || src.h <= 0) return false;
    const int h = std::min(src.h, kVramHeight);
    // Destination row j is source row k when k - j == dy - sy (mod height), which a later row k > j can hit.
    const int d = (((dy - src.y) % kVramHeight) + kVramHeight) % kVramHeight;
    if (d < 1 || d > h - 1) return false;
    const int w = std::min(src.w, kVramWidth);
    const int fwd = (((dx - src.x) % kVramWidth) + kVramWidth) % kVramWidth;
    const int back = (kVramWidth - fwd) % kVramWidth;
    return fwd < w || back < w;
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
