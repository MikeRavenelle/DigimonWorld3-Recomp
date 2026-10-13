#include "dw3/texrep/blocks.h"

#include <algorithm>
#include <cstring>

namespace dw3::texrep {

namespace {

constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;

inline uint64_t mix(uint64_t h, uint64_t w) {
    h ^= w;
    h *= kMul;
    h ^= h >> 29;
    return h;
}

}  // namespace

uint64_t hash_words(const uint16_t* p, std::size_t n, uint64_t seed) {
    uint64_t h = mix(seed ^ 0xA5A5A5A5DEADBEEFull, n);
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);  // the hash depends on the host byte order only through the word grouping
        h = mix(h, w);
    }
    uint64_t tail = 0;
    for (std::size_t k = 0; i < n; ++i, ++k) tail |= static_cast<uint64_t>(p[i]) << (16 * k);
    return mix(h, tail);
}

uint64_t hash_vram_rect(std::span<const uint16_t> vram, const Region& r) {
    uint64_t h = mix(0x1234, (static_cast<uint64_t>(r.width()) << 16) | static_cast<uint64_t>(r.height()));
    for (int y = r.y0; y < r.y1; ++y)
        h = hash_words(vram.data() + static_cast<std::size_t>(y & (kVramH - 1)) * kVramW + r.x0,
                       static_cast<std::size_t>(r.width()), h);
    return h;
}

uint64_t hash_clut(std::span<const uint16_t> vram, int x, int y, int count) {
    const uint16_t* row = vram.data() + static_cast<std::size_t>(y & (kVramH - 1)) * kVramW;
    if (x + count <= kVramW) return hash_words(row + x, static_cast<std::size_t>(count), 0x77);
    uint16_t tmp[512];
    count = std::min(count, 512);
    for (int i = 0; i < count; ++i) tmp[i] = row[(x + i) & (kVramW - 1)];
    return hash_words(tmp, static_cast<std::size_t>(count), 0x77);
}

void decode_block_8bit(std::span<const uint16_t> vram, const Region& block, int clut_x, int clut_y, std::vector<uint32_t>& out) {
    const int w = block.width() * 2, h = block.height();
    out.resize(static_cast<std::size_t>(w) * h);
    const uint16_t* clut = vram.data() + static_cast<std::size_t>(clut_y & (kVramH - 1)) * kVramW;
    auto expand = [](uint32_t v) { return (v << 3) | (v >> 2); };
    for (int y = 0; y < h; ++y) {
        const uint16_t* row = vram.data() + static_cast<std::size_t>((block.y0 + y) & (kVramH - 1)) * kVramW + block.x0;
        for (int x = 0; x < w; ++x) {
            const uint16_t word = row[x >> 1];
            const int index = (x & 1) ? (word >> 8) : (word & 255);
            const uint16_t c = clut[(clut_x + index) & (kVramW - 1)];
            out[static_cast<std::size_t>(y) * w + x] =
                c == 0 ? 0u : (expand(c & 31) | (expand((c >> 5) & 31) << 8) | (expand((c >> 10) & 31) << 16) | 0xFF000000u);
        }
    }
}

std::string hash_to_hex(uint64_t h) {
    static const char digits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, h >>= 4) s[static_cast<std::size_t>(i)] = digits[h & 15];
    return s;
}

bool hash_from_hex(std::string_view text, uint64_t& out) {
    if (text.empty() || text.size() > 16) return false;
    uint64_t v = 0;
    for (char c : text) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    out = v;
    return true;
}

void BlockRegistry::add_upload(const Region& r) {
    invalidate(r);
    if (r.width() < kMinWords || r.width() > kMaxWords || r.height() < kMinRows || r.height() > kMaxRows) return;
    if (blocks_.size() >= kMaxBlocks)
        for (std::size_t drop = 0; drop < kMaxBlocks / 4; ++drop) blocks_.pop_front();
    Block b;
    b.rect = r;
    b.id = next_id_++;
    blocks_.push_back(b);
}

Block* BlockRegistry::add_implicit(const Region& r) {
    if (r.width() < kMinImplicitWords || r.width() > kMaxWords || r.height() < kMinImplicitRows || r.height() > kMaxRows)
        return nullptr;
    for (const Block& b : blocks_)
        if (b.rect.intersects(r)) return nullptr;
    if (blocks_.size() >= kMaxBlocks) return nullptr;  // never drop blocks that pieces of this draw still point to
    Block b;
    b.rect = r;
    b.id = next_id_++;
    blocks_.push_back(b);
    return &blocks_.back();
}

void BlockRegistry::invalidate(const Region& r) {
    std::erase_if(blocks_, [&](const Block& b) { return b.rect.intersects(r); });
}

Block* BlockRegistry::find(int x, int y) {
    for (Block& b : blocks_)
        if (x >= b.rect.x0 && x < b.rect.x1 && y >= b.rect.y0 && y < b.rect.y1) return &b;
    return nullptr;
}

uint64_t BlockRegistry::hash_of(Block& b, std::span<const uint16_t> vram) const {
    if (!b.hashed) {
        b.hash = hash_vram_rect(vram, b.rect);
        b.hashed = true;
    }
    return b.hash;
}

namespace {

void split_rec(BlockRegistry& reg, int page_x, int page_y, int u, int v, int w, int h, bool implicit_blocks,
               std::vector<Piece>& out) {
    if (w <= 0 || h <= 0) return;
    const int x_texel = page_x * 2 + u;
    const int y = page_y + v;
    Block* b = reg.find(x_texel >> 1, y);
    if (!b && implicit_blocks) b = reg.add_implicit({x_texel >> 1, y, (x_texel + w + 1) >> 1, y + h});
    if (!b) {
        out.push_back({u, v, w, h, nullptr, 0, 0});
        return;
    }
    const int bx0 = b->rect.x0 * 2, bx1 = b->rect.x1 * 2;
    const int cw = std::min(x_texel + w, bx1) - x_texel;
    const int ch = std::min(y + h, b->rect.y1) - y;
    out.push_back({u, v, cw, ch, b, bx0 - page_x * 2, b->rect.y0 - page_y});
    if (cw < w) split_rec(reg, page_x, page_y, u + cw, v, w - cw, ch, implicit_blocks, out);
    if (ch < h) split_rec(reg, page_x, page_y, u, v + ch, w, h - ch, implicit_blocks, out);
}

}  // namespace

std::vector<Piece> split_by_blocks(BlockRegistry& registry, int page_x, int page_y, int u, int v, int w, int h,
                                   bool implicit_blocks) {
    std::vector<Piece> out;
    if (u < 0 || v < 0 || u + w > 256 || v + h > 256 || page_x * 2 + u + w > kVramW * 2 || page_y + v + h > kVramH) {
        out.push_back({u, v, w, h, nullptr, 0, 0});
        return out;
    }
    split_rec(registry, page_x, page_y, u, v, w, h, implicit_blocks, out);
    return out;
}

ReplacementOffset replacement_offset(int block_u0, int block_v0, int atlas_x, int atlas_y, int scale) {
    return {atlas_x - block_u0 * scale, atlas_y - block_v0 * scale};
}

std::optional<std::pair<int, int>> ShelfAtlas::alloc(int w, int h) {
    if (w <= 0 || h <= 0 || w > w_ || h > h_) return std::nullopt;
    for (Shelf& s : shelves_) {
        if (s.h >= h && s.h <= h * 2 && s.used + w <= w_) {
            const int x = s.used;
            s.used += w;
            return std::make_pair(x, s.y);
        }
    }
    if (next_y_ + h > h_) return std::nullopt;
    shelves_.push_back({next_y_, h, w});
    next_y_ += h;
    return std::make_pair(0, shelves_.back().y);
}

void ShelfAtlas::reset() {
    shelves_.clear();
    next_y_ = 0;
}

}  // namespace dw3::texrep
