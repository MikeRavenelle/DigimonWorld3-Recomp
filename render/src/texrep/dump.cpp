#include "dw3/texrep/dump.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <toml++/toml.hpp>

#include "dw3/texrep/blocks.h"

namespace dw3::texrep {

namespace {

bool overlaps(const PlacedBlock& a, const PlacedBlock& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

}  // namespace

std::vector<Component> build_components(std::span<const BlockSize> blocks, std::span<const PairOffset> pairs) {
    struct Group {
        std::vector<PlacedBlock> members;
    };
    std::vector<Group> groups;
    std::unordered_map<uint64_t, std::size_t> group_of;  // block hash -> index into groups
    std::unordered_map<uint64_t, std::size_t> member_index;
    for (const BlockSize& b : blocks) {
        if (group_of.count(b.hash)) continue;
        group_of[b.hash] = groups.size();
        groups.push_back({{PlacedBlock{b.hash, b.w, b.h, 0, 0}}});
    }
    auto find_member = [&](uint64_t hash) -> PlacedBlock* {
        for (PlacedBlock& m : groups[group_of[hash]].members)
            if (m.block == hash) return &m;
        return nullptr;
    };

    std::vector<PairOffset> sorted(pairs.begin(), pairs.end());
    std::stable_sort(sorted.begin(), sorted.end(), [](const PairOffset& l, const PairOffset& r) {
        if (l.count != r.count) return l.count > r.count;
        if (l.a != r.a) return l.a < r.a;
        return l.b < r.b;
    });
    for (const PairOffset& p : sorted) {
        if (!group_of.count(p.a) || !group_of.count(p.b) || p.a == p.b) continue;
        const std::size_t ga = group_of[p.a], gb = group_of[p.b];
        PlacedBlock* ma = find_member(p.a);
        PlacedBlock* mb = find_member(p.b);
        if (ga == gb) continue;  // already placed; the first (strongest) evidence stands
        const int dx = ma->x + p.dx - mb->x, dy = ma->y + p.dy - mb->y;
        bool clash = false;
        for (const PlacedBlock& m2 : groups[gb].members) {
            PlacedBlock shifted = m2;
            shifted.x += dx;
            shifted.y += dy;
            for (const PlacedBlock& m1 : groups[ga].members)
                if (overlaps(m1, shifted)) {
                    clash = true;
                    break;
                }
            if (clash) break;
        }
        if (clash) continue;
        for (PlacedBlock m2 : groups[gb].members) {
            m2.x += dx;
            m2.y += dy;
            groups[ga].members.push_back(m2);
            group_of[m2.block] = ga;
        }
        groups[gb].members.clear();
    }

    std::vector<Component> out;
    for (Group& g : groups) {
        if (g.members.empty()) continue;
        int min_x = g.members[0].x, min_y = g.members[0].y, max_x = min_x, max_y = min_y;
        for (const PlacedBlock& m : g.members) {
            min_x = std::min(min_x, m.x);
            min_y = std::min(min_y, m.y);
            max_x = std::max(max_x, m.x + m.w);
            max_y = std::max(max_y, m.y + m.h);
        }
        Component c;
        for (PlacedBlock m : g.members) {
            m.x -= min_x;
            m.y -= min_y;
            c.blocks.push_back(m);
        }
        std::sort(c.blocks.begin(), c.blocks.end(), [](const PlacedBlock& l, const PlacedBlock& r) { return l.block < r.block; });
        c.width = max_x - min_x;
        c.height = max_y - min_y;
        out.push_back(std::move(c));
    }
    // Largest first, so component numbers do not depend on hash values when the content is the same size.
    std::stable_sort(out.begin(), out.end(), [](const Component& l, const Component& r) {
        if (l.blocks.size() != r.blocks.size()) return l.blocks.size() > r.blocks.size();
        return l.blocks[0].block < r.blocks[0].block;
    });
    return out;
}

bool parse_dump_manifest(std::string_view text, DumpManifest& out, std::string& error) {
    toml::table root;
    try {
        root = toml::parse(std::string(text), std::string_view("dump.toml"));
    } catch (const toml::parse_error& e) {
        std::ostringstream ss;
        ss << e;
        error = ss.str();
        return false;
    }
    if (root["format"].value_or<int64_t>(0) != kDumpFormat) {
        error = "unsupported dump format";
        return false;
    }
    DumpManifest m;
    if (const toml::array* arr = root["component"].as_array())
        for (const toml::node& n : *arr) {
            const toml::table* t = n.as_table();
            if (!t) continue;
            DumpComponent c;
            c.id = static_cast<int>((*t)["id"].value_or<int64_t>(0));
            c.width = static_cast<int>((*t)["width"].value_or<int64_t>(0));
            c.height = static_cast<int>((*t)["height"].value_or<int64_t>(0));
            c.file = (*t)["file"].value_or<std::string>("");
            m.components.push_back(std::move(c));
        }
    if (const toml::array* arr = root["texture"].as_array())
        for (const toml::node& n : *arr) {
            const toml::table* t = n.as_table();
            if (!t) continue;
            DumpTexture d;
            if (!hash_from_hex((*t)["block"].value_or<std::string>(""), d.block) ||
                !hash_from_hex((*t)["clut"].value_or<std::string>(""), d.clut)) {
                error = "texture entry with a bad hash";
                return false;
            }
            d.width = static_cast<int>((*t)["width"].value_or<int64_t>(0));
            d.height = static_cast<int>((*t)["height"].value_or<int64_t>(0));
            d.file = (*t)["file"].value_or<std::string>("");
            d.uses = static_cast<uint32_t>((*t)["uses"].value_or<int64_t>(0));
            d.component = static_cast<int>((*t)["component"].value_or<int64_t>(0));
            d.x = static_cast<int>((*t)["x"].value_or<int64_t>(0));
            d.y = static_cast<int>((*t)["y"].value_or<int64_t>(0));
            d.primary = (*t)["primary"].value_or<bool>(false);
            if (const toml::array* pieces = (*t)["pieces"].as_array())
                for (const toml::node& pn : *pieces) {
                    const toml::array* v = pn.as_array();
                    if (!v || v->size() != 6) {
                        error = "texture entry with a bad piece";
                        return false;
                    }
                    int f[6];
                    for (std::size_t k = 0; k < 6; ++k) f[k] = static_cast<int>((*v)[k].value_or<int64_t>(0));
                    d.pieces.push_back({f[0], f[1], f[2], f[3], f[4], f[5]});
                }
            m.textures.push_back(std::move(d));
        }
    out = std::move(m);
    return true;
}

std::string write_dump_manifest(const DumpManifest& m) {
    toml::table root;
    root.insert("format", static_cast<int64_t>(kDumpFormat));
    toml::array comps;
    for (const DumpComponent& c : m.components) {
        toml::table t;
        t.insert("id", static_cast<int64_t>(c.id));
        t.insert("width", static_cast<int64_t>(c.width));
        t.insert("height", static_cast<int64_t>(c.height));
        t.insert("file", c.file);
        comps.push_back(std::move(t));
    }
    root.insert("component", std::move(comps));
    toml::array texs;
    for (const DumpTexture& d : m.textures) {
        toml::table t;
        t.insert("block", hash_to_hex(d.block));
        t.insert("clut", hash_to_hex(d.clut));
        t.insert("width", static_cast<int64_t>(d.width));
        t.insert("height", static_cast<int64_t>(d.height));
        t.insert("file", d.file);
        t.insert("uses", static_cast<int64_t>(d.uses));
        t.insert("component", static_cast<int64_t>(d.component));
        t.insert("x", static_cast<int64_t>(d.x));
        t.insert("y", static_cast<int64_t>(d.y));
        t.insert("primary", d.primary);
        if (!d.pieces.empty()) {
            toml::array pieces;
            for (const DumpPiece& p : d.pieces) {
                toml::array v;
                for (int f : {p.ax, p.ay, p.w, p.h, p.cx, p.cy}) v.push_back(static_cast<int64_t>(f));
                pieces.push_back(std::move(v));
            }
            t.insert("pieces", std::move(pieces));
        }
        texs.push_back(std::move(t));
    }
    root.insert("texture", std::move(texs));
    std::ostringstream ss;
    ss << root;
    return ss.str();
}

void TextureDump::add_variant(uint64_t block, uint64_t clut, int w, int h, std::vector<uint32_t> rgba) {
    Variant& v = variants_[{block, clut}];
    v.w = w;
    v.h = h;
    v.rgba = std::move(rgba);
}

void TextureDump::observe(uint64_t block, uint64_t clut, int ox, int oy) {
    auto it = variants_.find({block, clut});
    if (it != variants_.end()) ++it->second.uses;
    for (FrameDraw& d : frame_)
        if (d.block == block) {
            if (d.ox != ox || d.oy != oy) d.conflict = true;
            return;
        }
    frame_.push_back({block, ox, oy, false});
}

void TextureDump::end_frame() {
    for (std::size_t i = 0; i < frame_.size(); ++i) {
        if (frame_[i].conflict) continue;
        for (std::size_t j = i + 1; j < frame_.size(); ++j) {
            if (frame_[j].conflict) continue;
            FrameDraw a = frame_[i], b = frame_[j];
            if (a.block > b.block) std::swap(a, b);
            ++pairs_[{a.block, b.block}][{b.ox - a.ox, b.oy - a.oy}];
        }
    }
    frame_.clear();
}

bool TextureDump::write(const std::filesystem::path& dir, std::string& error) {
    end_frame();
    // Blocks that were used often enough, and for each the palette it was used with most.
    std::map<uint64_t, std::pair<uint64_t, uint32_t>> primary;  // block -> (clut, uses)
    std::map<uint64_t, uint32_t> total;
    for (const auto& [key, v] : variants_) {
        total[key.first] += v.uses;
        auto it = primary.find(key.first);
        if (it == primary.end() || v.uses > it->second.second) primary[key.first] = {key.second, v.uses};
    }
    std::vector<BlockSize> sizes;
    for (const auto& [hash, uses] : total) {
        if (uses < min_uses_) continue;
        const Variant& v = variants_[{hash, primary[hash].first}];
        sizes.push_back({hash, v.w, v.h});
    }
    std::vector<PairOffset> pairs;
    for (const auto& [key, offsets] : pairs_) {
        if (offsets.size() != 1) continue;  // seen at two different offsets: not one picture
        pairs.push_back({key.first, key.second, offsets.begin()->first.first, offsets.begin()->first.second,
                         offsets.begin()->second});
    }
    const std::vector<Component> comps = build_components(sizes, pairs);

    DumpManifest manifest;
    int index = 0;
    for (const Component& c : comps) {
        Image canvas;
        canvas.w = c.width;
        canvas.h = c.height;
        canvas.rgba.assign(static_cast<std::size_t>(c.width) * c.height, 0);
        DumpComponent dc;
        dc.id = index;
        dc.width = c.width;
        dc.height = c.height;
        char name[32];
        std::snprintf(name, sizeof name, "components/c%03d.png", index);
        dc.file = name;
        for (const PlacedBlock& pb : c.blocks) {
            for (const auto& [key, v] : variants_) {
                if (key.first != pb.block) continue;
                DumpTexture t;
                t.block = pb.block;
                t.clut = key.second;
                t.width = v.w;
                t.height = v.h;
                t.file = "textures/" + hash_to_hex(key.first) + "_" + hash_to_hex(key.second) + ".png";
                t.uses = v.uses;
                t.component = index;
                t.x = pb.x;
                t.y = pb.y;
                t.primary = primary[pb.block].first == key.second;
                if (t.primary)
                    for (int y = 0; y < v.h; ++y)
                        for (int x = 0; x < v.w; ++x)
                            canvas.rgba[static_cast<std::size_t>(pb.y + y) * c.width + pb.x + x] = v.rgba[static_cast<std::size_t>(y) * v.w + x];
                Image img;
                img.w = v.w;
                img.h = v.h;
                img.rgba = v.rgba;
                if (!write_png(dir / t.file, img, error)) return false;
                manifest.textures.push_back(std::move(t));
            }
        }
        if (!write_png(dir / dc.file, canvas, error)) return false;
        manifest.components.push_back(std::move(dc));
        ++index;
    }
    const std::string text = write_dump_manifest(manifest);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / "dump.toml", std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
        error = "cannot write " + (dir / "dump.toml").string();
        return false;
    }
    return true;
}

}  // namespace dw3::texrep
