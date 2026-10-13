#include "dw3/texrep/field_backdrop.h"

#include <algorithm>
#include <cstdlib>

namespace dw3::texrep {

namespace {

constexpr int kCell = kStageCell;
// A scroll that is further than this from the one of the previous frame is not trusted to settle a tie.
constexpr int kTieDistance = 160;
// Frames without a resolved map after which the previous one is forgotten.
constexpr int kForgetAfterFrames = 30;

unsigned layer_bit(int layer) { return layer == 1 ? layers::kFront : layer == 2 ? layers::kBack : layers::kMain; }
unsigned group_of(int layer) { return layer == 1 ? layers::kFront : layers::kBackground; }

}  // namespace

uint64_t FieldBackdrop::geometry_key(int x, int y, int w, int h) {
    auto field = [](int v) { return static_cast<uint64_t>(static_cast<uint16_t>(v)); };
    return field(x) | (field(y) << 16) | (field(w) << 32) | (field(h) << 48);
}

void FieldBackdrop::begin_frame() {
    if (resolved_) {
        prev_map_ = map_;
        prev_sx_ = sx_;
        prev_sy_ = sy_;
        idle_frames_ = 0;
    } else if (++idle_frames_ >= kForgetAfterFrames) {
        prev_map_ = -1;  // out of the field: a later map is not compared with this one
    }
    votes_.clear();
    matches_.clear();
    drawn_.clear();
    consumed_[0] = consumed_[1] = 0;
    resolved_ = false;
    map_ = -1;
}

bool FieldBackdrop::add_sprite(const BackdropSprite& s) {
    drawn_.insert(geometry_key(s.x, s.y, s.w, s.h));
    if (!library_ || !library_->ready()) return false;
    if (s.keyed) {
        bool matched = false;
        for (const StageLibrary::TileRef& ref : library_->refs(s.block, s.clut)) {
            const StageLibrary::Tile& tile = library_->map(ref.map).tiles[ref.tile];
            for (const StagePiece& piece : tile.pieces) {
                if (piece.ax != s.bu || piece.ay != s.bv || piece.w != s.w || piece.h != s.h) continue;
                const int sx = tile.col * kCell + piece.x - s.x, sy = tile.row * kCell + piece.y - s.y;
                auto it = std::find_if(votes_.begin(), votes_.end(), [&](const Hypothesis& h) {
                    return h.map == static_cast<int>(ref.map) && h.sx == sx && h.sy == sy;
                });
                if (it == votes_.end()) {
                    votes_.push_back({static_cast<int>(ref.map), sx, sy, 0});
                    it = votes_.end() - 1;
                }
                ++it->votes;
                matches_.push_back({static_cast<int>(it - votes_.begin()), piece.layer, s.color});
                matched = true;
            }
        }
        if (matched) {
            resolved_ = false;
            return true;
        }
    }
    // Not a picture that is known: it still is background when it has the shape and place of a piece.
    return fits_known_piece(s);
}

bool FieldBackdrop::fits_known_piece(const BackdropSprite& s) const {
    int map = map_, sx = sx_, sy = sy_;
    if (!resolved_) {
        if (!votes_.empty()) {
            const Hypothesis* best = &votes_.front();
            for (const Hypothesis& h : votes_)
                if (h.votes > best->votes) best = &h;
            map = best->map;
            sx = best->sx;
            sy = best->sy;
        } else if (prev_map_ >= 0) {
            map = prev_map_;
            sx = prev_sx_;
            sy = prev_sy_;
        } else {
            return false;
        }
    }
    const StageLibrary::Map& m = library_->map(static_cast<uint32_t>(map));
    for (const StageLibrary::Tile& tile : m.tiles)
        for (const StagePiece& piece : tile.pieces)
            if (tile.col * kCell + piece.x - sx == s.x && tile.row * kCell + piece.y - sy == s.y && piece.w == s.w && piece.h == s.h)
                return true;
    return false;
}

bool FieldBackdrop::resolve() {
    if (resolved_) return true;
    if (votes_.empty() || !library_) return false;
    int most = 0;
    for (const Hypothesis& h : votes_) most = std::max(most, h.votes);
    const Hypothesis* pick = nullptr;
    int candidates = 0;
    for (const Hypothesis& h : votes_) {
        if (h.votes != most) continue;
        ++candidates;
        if (!pick) {
            pick = &h;
            continue;
        }
        // Ties go to the map of the previous frame, then to the scroll nearest to the previous one.
        auto rank = [&](const Hypothesis& x) {
            const int same_map = x.map == prev_map_ ? 0 : 1;
            return std::pair<int, int>{same_map, std::abs(x.sx - prev_sx_) + std::abs(x.sy - prev_sy_)};
        };
        if (rank(h) < rank(*pick)) pick = &h;
    }
    if (candidates > 1 && (prev_map_ < 0 || pick->map != prev_map_ ||
                           std::abs(pick->sx - prev_sx_) + std::abs(pick->sy - prev_sy_) > kTieDistance))
        return false;
    map_ = pick->map;
    sx_ = pick->sx;
    sy_ = pick->sy;
    picked_ = static_cast<int>(pick - votes_.data());
    resolved_ = true;
    return true;
}

bool FieldBackdrop::is_placeholder(int x, int y, int w, int h) {
    constexpr int kSquare = 64;
    if (w != kSquare || h != kSquare || !resolve()) return false;
    const int mx = x + sx_, my = y + sy_;
    const StageLibrary::Map& m = library_->map(static_cast<uint32_t>(map_));
    return mx >= 0 && my >= 0 && mx % kSquare == 0 && my % kSquare == 0 && mx < m.cols * kCell && my < m.rows * kCell;
}

unsigned FieldBackdrop::due() {
    if (matches_.empty() || !resolve()) return 0;
    unsigned mask = 0;
    for (std::size_t i = 0; i < matches_.size(); ++i) {
        const Match& m = matches_[i];
        if (m.hypothesis != picked_) continue;
        const unsigned group = group_of(m.layer);
        if (i >= consumed_[group == layers::kFront ? 1 : 0]) mask |= group;
    }
    return mask;
}

std::vector<BackdropFill> FieldBackdrop::take_missing(unsigned layer_mask, const BackdropRect& view) {
    std::vector<BackdropFill> out;
    if (!resolve()) return out;
    if (layer_mask & layers::kBackground) consumed_[0] = matches_.size();
    if (layer_mask & layers::kFront) consumed_[1] = matches_.size();
    auto color_of = [&](int layer) {
        for (auto it = matches_.rbegin(); it != matches_.rend(); ++it)
            if (it->hypothesis == picked_ && it->layer == layer) return last_color_[layer] = it->color;
        return last_color_[layer];
    };
    const StageLibrary::Map& m = library_->map(static_cast<uint32_t>(map_));
    std::vector<BackdropFill> by_layer[3];
    std::vector<BackdropFill> present_main;
    for (uint32_t t = 0; t < m.tiles.size(); ++t) {
        const StageLibrary::Tile& tile = m.tiles[t];
        for (const StagePiece& piece : tile.pieces) {
            if (piece.layer < 0 || piece.layer > 2 || !(layer_mask & layer_bit(piece.layer))) continue;
            BackdropFill f;
            f.map = static_cast<uint32_t>(map_);
            f.tile = t;
            f.piece = &piece;
            f.x = tile.col * kCell + piece.x - sx_;
            f.y = tile.row * kCell + piece.y - sy_;
            const BackdropRect r{f.x, f.y, f.x + piece.w, f.y + piece.h};
            if (!r.intersects(view)) continue;
            const bool drawn = drawn_.count(geometry_key(f.x, f.y, piece.w, piece.h)) != 0;
            if (drawn && piece.layer != 0) continue;
            (drawn ? present_main : by_layer[piece.layer]).push_back(f);
        }
    }
    for (int layer = 0; layer < 3; ++layer)
        for (BackdropFill& f : by_layer[layer]) f.color = color_of(layer);
    for (BackdropFill& f : present_main) f.color = color_of(0);
    // A back piece that is drawn now lies over main pieces the game drew earlier; those are drawn again over it.
    out = by_layer[2];
    for (const BackdropFill& main : present_main) {
        const BackdropRect r{main.x, main.y, main.x + main.piece->w, main.y + main.piece->h};
        const bool covered = std::any_of(by_layer[2].begin(), by_layer[2].end(), [&](const BackdropFill& back) {
            return r.intersects({back.x, back.y, back.x + back.piece->w, back.y + back.piece->h});
        });
        if (covered) out.push_back(main);
    }
    out.insert(out.end(), by_layer[0].begin(), by_layer[0].end());
    out.insert(out.end(), by_layer[1].begin(), by_layer[1].end());
    // Pieces listed now count as drawn, so a second request in the same frame does not list them again.
    for (const BackdropFill& f : out) drawn_.insert(geometry_key(f.x, f.y, f.piece->w, f.piece->h));
    return out;
}

}  // namespace dw3::texrep
