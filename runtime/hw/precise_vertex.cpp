#include "dw3/precise_vertex.h"

#include <cmath>

namespace dw3 {

namespace {
constexpr int32_t sext11(uint32_t v) { return static_cast<int32_t>(v << 21) >> 21; }
}  // namespace

PreciseVertexCache::PreciseVertexCache() : entries_(kSets * 2) {}

void PreciseVertexCache::record(int32_t sx, int32_t sy, double x, double y, double w) {
    if (!(w > 0.0) || std::floor(x) != sx || std::floor(y) != sy) return;
    const uint32_t key = (static_cast<uint32_t>(sx) & 0x7FFu) | ((static_cast<uint32_t>(sy) & 0x7FFu) << 16);
    Entry* set = &entries_[set_of(key) * 2];
    const uint64_t frame = now();
    // Reuse the entry of the same key, else an empty way, else the older one.
    Entry* slot = nullptr;
    for (int i = 0; i < 2 && !slot; ++i)
        if (set[i].valid && set[i].key == key) slot = &set[i];
    for (int i = 0; i < 2 && !slot; ++i)
        if (!set[i].valid) slot = &set[i];
    if (!slot) slot = set[0].frame <= set[1].frame ? &set[0] : &set[1];
    slot->key = key;
    slot->frame = frame;
    slot->valid = true;
    slot->v = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w)};
}

bool PreciseVertexCache::find(uint32_t word, PreciseVertex& out) const {
    const uint32_t key = key_of(word);
    const Entry* set = &entries_[set_of(key) * 2];
    const uint64_t frame = now();
    for (int i = 0; i < 2; ++i) {
        const Entry& e = set[i];
        if (!e.valid || e.key != key) continue;
        if (frame - e.frame >= kMaxAge) break;
        if (std::floor(e.v.x) != sext11(key & 0x7FFu) || std::floor(e.v.y) != sext11(key >> 16)) break;
        out = e.v;
        ++hits_;
        return true;
    }
    ++misses_;
    return false;
}

void PreciseVertexCache::clear() {
    for (Entry& e : entries_) e.valid = false;
}

}  // namespace dw3
