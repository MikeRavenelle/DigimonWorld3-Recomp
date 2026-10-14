#include "scenario.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include <toml++/toml.hpp>

namespace dw3::scenario {

namespace {

// Bit layout matches dw3::PadButton (sio.h).
const std::map<std::string, uint16_t>& button_names() {
    static const std::map<std::string, uint16_t> names = {
        {"SELECT", 1u << 0},    {"L3", 1u << 1},       {"R3", 1u << 2},      {"START", 1u << 3},
        {"UP", 1u << 4},        {"RIGHT", 1u << 5},    {"DOWN", 1u << 6},    {"LEFT", 1u << 7},
        {"L2", 1u << 8},        {"R2", 1u << 9},       {"L1", 1u << 10},     {"R1", 1u << 11},
        {"TRIANGLE", 1u << 12}, {"CIRCLE", 1u << 13},  {"CROSS", 1u << 14},  {"SQUARE", 1u << 15},
    };
    return names;
}

[[noreturn]] void fail(const std::string& src, const std::string& msg) { throw std::runtime_error(src + ": " + msg); }

uint16_t parse_button(const std::string& src, const toml::table& t) {
    auto name = t["button"].value<std::string>();
    if (!name) fail(src, "entry without `button`");
    uint16_t b = button_from_name(*name);
    if (!b) fail(src, "unknown button '" + *name + "'");
    return b;
}

RamCheck parse_ram(const std::string& src, const toml::table& t) {
    RamCheck r;
    r.via = static_cast<uint32_t>(t["via"].value_or(int64_t{0}));
    r.addr = static_cast<uint32_t>(t["addr"].value_or(int64_t{0}));
    r.size = static_cast<int>(t["size"].value_or(int64_t{4}));
    r.value = static_cast<uint32_t>(t["value"].value_or(int64_t{0}));
    r.mask = static_cast<uint32_t>(t["mask"].value_or(int64_t{0xFFFFFFFF}));
    if (r.size != 1 && r.size != 2 && r.size != 4) fail(src, "ram size must be 1, 2 or 4");
    if (r.addr % static_cast<uint32_t>(r.size) != 0) fail(src, "ram addr must be aligned to its size");
    std::string cmp = t["cmp"].value_or(std::string("eq"));
    if (cmp == "eq") r.cmp = Compare::Eq;
    else if (cmp == "ne") r.cmp = Compare::Ne;
    else if (cmp == "lt") r.cmp = Compare::Lt;
    else if (cmp == "le") r.cmp = Compare::Le;
    else if (cmp == "gt") r.cmp = Compare::Gt;
    else if (cmp == "ge") r.cmp = Compare::Ge;
    else fail(src, "ram `cmp` must be eq, ne, lt, le, gt or ge, not '" + cmp + "'");
    return r;
}

// Wait/walk fields shared by both tables: label, timeout, hold, press.
void parse_wait_common(const std::string& src, const toml::table& t, const char* what, Wait& w) {
    w.label = t["label"].value_or(std::string());
    w.timeout = t["timeout"].value_or(int64_t{0});
    if (w.timeout <= 0) fail(src, std::string("[[") + what + "]] needs `timeout` > 0 (real frames)");
    if (auto* hold = t["hold"].as_array()) {
        for (auto& hn : *hold) {
            auto name = hn.value<std::string>();
            uint16_t b = name ? button_from_name(*name) : 0;
            if (!b) fail(src, std::string("[[") + what + "]] `hold` has an unknown button");
            w.hold |= b;
        }
    }
    if (auto name = t["press"].value<std::string>()) {
        w.tap = button_from_name(*name);
        if (!w.tap) fail(src, std::string("[[") + what + "]] unknown `press` button '" + *name + "'");
    }
    w.tap_every = t["every"].value_or(int64_t{40});
    w.tap_hold = t["press_hold"].value_or(int64_t{4});
    if (w.tap && w.tap_every <= w.tap_hold) fail(src, std::string("[[") + what + "]] needs every > press_hold");
    if (auto* ram = t["ram"].as_array())
        for (auto& rn : *ram) w.ram.push_back(parse_ram(src, *rn.as_table()));
    if (auto* guard = t["guard"].as_array())
        for (auto& gn : *guard) w.guard.push_back(parse_ram(src, *gn.as_table()));
}

bool in_ram(uint32_t p) { return (p & 0x1FFFFFFFu) < 0x200000u && p != 0; }

const char* const kMergedTables[] = {"press", "repeat", "reboot", "wait", "walk", "check", "poke"};

// Merges the tables of the `include` files into `root`, included entries first. `seen` guards against cycles.
void merge_includes(toml::table& root, const std::filesystem::path& file, std::vector<std::filesystem::path>& seen) {
    auto* inc = root["include"].as_array();
    if (!inc) return;
    const std::string src = file.string();
    auto canon = std::filesystem::weakly_canonical(file);
    if (std::find(seen.begin(), seen.end(), canon) != seen.end()) fail(src, "`include` cycle");
    seen.push_back(canon);
    std::map<std::string, toml::array> merged;
    for (auto& n : *inc) {
        auto rel = n.value<std::string>();
        if (!rel) fail(src, "`include` wants a list of file names");
        auto path = file.parent_path() / *rel;
        toml::table sub;
        try {
            sub = toml::parse_file(path.string());
        } catch (const toml::parse_error& e) {
            fail(path.string(), e.what());
        }
        merge_includes(sub, path, seen);
        for (const char* key : kMergedTables)
            if (auto* a = sub[key].as_array())
                for (auto& e : *a) merged[key].push_back(e);
    }
    seen.pop_back();
    for (const char* key : kMergedTables) {
        auto it = merged.find(key);
        if (it == merged.end()) continue;
        if (auto* own = root[key].as_array())
            for (auto& e : *own) it->second.push_back(e);
        root.insert_or_assign(key, std::move(it->second));
    }
    root.erase("include");
}

}  // namespace

bool read_ram(const RamReader& read, const RamCheck& r, uint32_t& out) {
    uint32_t addr = r.addr;
    if (r.via) {
        uint32_t p = read(r.via, 4);
        if (!in_ram(p)) return false;
        addr += p;
    }
    out = read(addr, r.size);
    return true;
}

int32_t sign_extend(uint32_t v, int size) {
    if (size == 1) return static_cast<int8_t>(v);
    if (size == 2) return static_cast<int16_t>(v);
    return static_cast<int32_t>(v);
}

uint16_t steer_buttons(const Steer& s, int32_t x, int32_t z) {
    uint16_t b = 0;
    if (s.use_x) {
        if (x < s.target_x - s.tolerance) b |= button_from_name("RIGHT");
        else if (x > s.target_x + s.tolerance) b |= button_from_name("LEFT");
    }
    if (s.use_z) {
        if (z < s.target_z - s.tolerance) b |= button_from_name("DOWN");
        else if (z > s.target_z + s.tolerance) b |= button_from_name("UP");
    }
    return b;
}

bool steer_reached(const Steer& s, int32_t x, int32_t z) {
    return (!s.use_x || std::abs(x - s.target_x) <= s.tolerance) && (!s.use_z || std::abs(z - s.target_z) <= s.tolerance);
}

bool ram_matches(const RamCheck& r, uint32_t actual) {
    const uint32_t a = actual & r.mask, v = r.value & r.mask;
    switch (r.cmp) {
        case Compare::Eq: return a == v;
        case Compare::Ne: return a != v;
        case Compare::Lt: return a < v;
        case Compare::Le: return a <= v;
        case Compare::Gt: return a > v;
        case Compare::Ge: return a >= v;
    }
    return false;
}

uint16_t button_from_name(const std::string& name) {
    auto it = button_names().find(name);
    return it == button_names().end() ? 0 : it->second;
}

Scenario parse(const std::string& text, const std::string& src) {
    toml::table root;
    try {
        root = toml::parse(text, src);
    } catch (const toml::parse_error& e) {
        fail(src, e.what());
    }
    Scenario s;
    s.name = root["name"].value_or(std::string());
    s.description = root["description"].value_or(std::string());
    s.frames = root["frames"].value_or(int64_t{0});
    s.skip_intro = root["skip_intro"].value_or(true);
    s.memcard = root["memcard"].value_or(std::string("none"));
    s.gpu = root["gpu"].value_or(false);
    if (const auto* list = root["settings"].as_array())
        for (const auto& item : *list) {
            const auto text = item.value<std::string>();
            if (!text || text->find('=') == std::string::npos) fail(src, "`settings` entries must be strings like \"gameplay.god_mode=true\"");
            s.settings.push_back(*text);
        }
    if (s.name.empty()) fail(src, "missing `name`");
    if (s.frames <= 0) fail(src, "`frames` must be > 0");
    if (s.memcard != "none" && s.memcard != "blank" && !(s.memcard.rfind("game:", 0) == 0 && s.memcard.size() > 5))
        fail(src, "`memcard` must be \"none\", \"blank\" or \"game:<file>\"");

    if (auto* arr = root["press"].as_array()) {
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            int64_t at = t["at"].value_or(int64_t{-1});
            if (at < 0) fail(src, "[[press]] without `at`");
            s.presses.push_back({parse_button(src, t), at, at + t["hold"].value_or(int64_t{4})});
        }
    }
    if (auto* arr = root["repeat"].as_array()) {
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            uint16_t b = parse_button(src, t);
            int64_t from = t["from"].value_or(int64_t{0}), to = t["to"].value_or(s.frames);
            int64_t every = t["every"].value_or(int64_t{0}), hold = t["hold"].value_or(int64_t{3});
            if (every <= hold) fail(src, "[[repeat]] needs every > hold");
            for (int64_t f = from; f < to; f += every) s.presses.push_back({b, f, f + hold});
        }
    }
    if (auto* arr = root["reboot"].as_array()) {
        for (auto& n : *arr) {
            int64_t at = (*n.as_table())["at"].value_or(int64_t{-1});
            if (at <= 0 || at >= s.frames) fail(src, "[[reboot]] `at` must be within (0, frames)");
            if (!s.reboots.empty() && at <= s.reboots.back()) fail(src, "[[reboot]] entries must be in increasing order");
            s.reboots.push_back(at);
        }
    }
    s.start_state = root["start_state"].value_or(std::string());
    s.start_frame = root["start_frame"].value_or(int64_t{0});
    if (s.start_frame < 0 || s.start_frame >= s.frames) fail(src, "`start_frame` must be within [0, frames)");
    if (s.start_frame && s.start_state.empty()) fail(src, "`start_frame` needs `start_state`");
    if (auto* arr = root["poke"].as_array()) {
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            Poke p;
            p.at = t["at"].value_or(int64_t{-1});
            p.addr = t["addr"].value_or(0u);
            p.size = static_cast<int>(t["size"].value_or(int64_t{4}));
            p.value = t["value"].value_or(0u);
            if (p.at < 0 || p.at > s.frames) fail(src, "[[poke]] `at` must be within [0, frames]");
            if (p.addr < 0x80000000u) fail(src, "[[poke]] needs `addr` in main RAM (0x80000000 and up)");
            if (p.size != 1 && p.size != 2 && p.size != 4) fail(src, "[[poke]] `size` must be 1, 2 or 4");
            s.pokes.push_back(p);
        }
        std::stable_sort(s.pokes.begin(), s.pokes.end(), [](const Poke& a, const Poke& b) { return a.at < b.at; });
    }
    for (const char* key : {"savestate", "loadstate"}) {
        auto* arr = root[key].as_array();
        if (!arr) continue;
        const bool load = std::string(key) == "loadstate";
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            StateOp op;
            op.at = t["at"].value_or(int64_t{-1});
            op.path = t["path"].value_or(std::string());
            op.replay = t["replay"].value_or(false);
            if (op.at < 0 || op.at > s.frames) fail(src, std::string("[[") + key + "]] `at` must be within [0, frames]");
            if (op.path.empty()) fail(src, std::string("[[") + key + "]] needs `path`");
            if (op.replay && !load) fail(src, "[[savestate]] has no `replay`");
            (load ? s.loads : s.saves).push_back(std::move(op));
        }
    }
    auto by_at = [](const StateOp& a, const StateOp& b) { return a.at < b.at; };
    std::stable_sort(s.saves.begin(), s.saves.end(), by_at);
    std::stable_sort(s.loads.begin(), s.loads.end(), by_at);
    for (const auto& l : s.loads) {
        if (!l.replay) continue;
        bool found = false;
        for (const auto& sv : s.saves) found = found || (sv.path == l.path && sv.at < l.at);
        if (!found) fail(src, "[[loadstate]] with `replay` needs an earlier [[savestate]] with the same `path`");
    }
    auto add_wait = [&](Wait w) {
        if (w.at <= 0 || w.at > s.frames) fail(src, "[[wait]]/[[walk]] `at` must be within (0, frames]");
        s.waits.push_back(std::move(w));
    };
    if (auto* arr = root["wait"].as_array()) {
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            Wait w;
            w.at = t["at"].value_or(int64_t{-1});
            parse_wait_common(src, t, "wait", w);
            if (w.ram.empty()) fail(src, "[[wait]] needs at least one `ram` condition");
            add_wait(std::move(w));
        }
    }
    if (auto* arr = root["walk"].as_array()) {
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            Wait w;
            w.at = t["at"].value_or(int64_t{-1});
            parse_wait_common(src, t, "walk", w);
            Steer st;
            auto* xt = t["x"].as_table();
            auto* zt = t["z"].as_table();
            if (!xt || !zt) fail(src, "[[walk]] needs `x` and `z` position locations");
            st.x = parse_ram(src, *xt);
            st.z = parse_ram(src, *zt);
            auto* target = t["target"].as_array();
            if (!target || target->size() != 2) fail(src, "[[walk]] needs `target = [x, z]`");
            st.target_x = static_cast<int32_t>((*target)[0].value_or(int64_t{0}));
            st.target_z = static_cast<int32_t>((*target)[1].value_or(int64_t{0}));
            st.tolerance = static_cast<int32_t>(t["tolerance"].value_or(int64_t{8}));
            if (st.tolerance < 0) fail(src, "[[walk]] `tolerance` must be >= 0");
            std::string axes = t["axes"].value_or(std::string("xz"));
            if (axes != "xz" && axes != "x" && axes != "z") fail(src, "[[walk]] `axes` must be \"xz\", \"x\" or \"z\"");
            st.use_x = axes.find('x') != std::string::npos;
            st.use_z = axes.find('z') != std::string::npos;
            w.steer = st;
            add_wait(std::move(w));
        }
    }
    std::stable_sort(s.waits.begin(), s.waits.end(), [](const Wait& a, const Wait& b) { return a.at < b.at; });
    if (auto* arr = root["margin_watch"].as_array()) {
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            MarginWatch w;
            w.from = t["from"].value_or(int64_t{-1});
            w.to = t["to"].value_or(int64_t{-1});
            if (w.from < 0 || w.to < w.from || w.to > s.frames) fail(src, "[[margin_watch]] needs 0 <= from <= to <= frames");
            w.label = t["label"].value_or(std::string());
            w.lit_changes_max = static_cast<int>(t["lit_changes_max"].value_or(int64_t{0}));
            w.strip = t["strip"].value_or(false);
            s.margin_watches.push_back(std::move(w));
        }
    }
    if (auto* arr = root["flicker_watch"].as_array()) {
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            FlickerWatch w;
            w.from = t["from"].value_or(int64_t{-1});
            w.to = t["to"].value_or(int64_t{-1});
            if (w.from < 0 || w.to < w.from || w.to > s.frames) fail(src, "[[flicker_watch]] needs 0 <= from <= to <= frames");
            w.label = t["label"].value_or(std::string());
            w.flicker_blocks_max = static_cast<int>(t["flicker_blocks_max"].value_or(int64_t{40}));
            s.flicker_watches.push_back(std::move(w));
        }
    }
    if (auto* arr = root["check"].as_array()) {
        for (auto& n : *arr) {
            auto& t = *n.as_table();
            Check c;
            c.at = t["at"].value_or(int64_t{-1});
            if (c.at < 0 || c.at > s.frames) fail(src, "[[check]] `at` must be within [0, frames]");
            c.label = t["label"].value_or(std::string());
            if (auto v = t["display_enabled"].value<bool>()) c.display_enabled = *v;
            if (auto v = t["display_width"].value<int64_t>()) c.display_width = static_cast<int>(*v);
            if (auto v = t["display_height"].value<int64_t>()) c.display_height = static_cast<int>(*v);
            if (auto v = t["min_primitives"].value<int64_t>()) c.min_primitives = static_cast<uint64_t>(*v);
            if (auto v = t["min_commands"].value<int64_t>()) c.min_commands = static_cast<uint64_t>(*v);
            if (auto v = t["tty_contains"].value<std::string>()) c.tty_contains = *v;
            if (auto v = t["disc_backgrounds"].value<int64_t>()) {
                if (*v < 1) fail(src, "`disc_backgrounds` must be >= 1");
                c.min_disc_backgrounds = static_cast<int>(*v);
            }
            if (auto v = t["screen_hash"].value<std::string>()) {
                c.screen_hash = hash_from_hex(*v);
                if (!c.screen_hash) fail(src, "bad `screen_hash` (want 64 hex digits)");
            }
            c.max_hash_distance = static_cast<int>(t["max_hash_distance"].value_or(int64_t{24}));
            c.screenshot = t["screenshot"].value_or(false);
            if (auto v = t["margin_colors_min"].value<int64_t>()) c.margin_colors_min = static_cast<int>(*v);
            if (auto v = t["margin_colors_max"].value<int64_t>()) c.margin_colors_max = static_cast<int>(*v);
            if (auto v = t["backdrop_pieces_min"].value<int64_t>()) c.backdrop_pieces_min = static_cast<uint64_t>(*v);
            if (auto v = t["squares_hidden_min"].value<int64_t>()) c.squares_hidden_min = static_cast<uint64_t>(*v);
            if (auto v = t["squares_drawn_min"].value<int64_t>()) c.squares_drawn_min = static_cast<uint64_t>(*v);
            if (auto v = t["squares_drawn_max"].value<int64_t>()) c.squares_drawn_max = static_cast<uint64_t>(*v);
            if (auto v = t["wide_field_frames_min"].value<int64_t>()) c.wide_field_frames_min = static_cast<uint64_t>(*v);
            if (auto v = t["wide_field_fallbacks_max"].value<int64_t>()) c.wide_field_fallbacks_max = static_cast<uint64_t>(*v);
            if (auto v = t["wide_margin_polygons_min"].value<int64_t>()) c.wide_margin_polygons_min = static_cast<uint64_t>(*v);
            if (auto v = t["wide_margin_polygons_max"].value<int64_t>()) c.wide_margin_polygons_max = static_cast<uint64_t>(*v);
            if (auto v = t["audio_min_rms"].value<double>()) c.audio_min_rms = *v;
            if (auto v = t["audio_max_rms"].value<double>()) c.audio_max_rms = *v;
            c.audio_frames = static_cast<int>(t["audio_frames"].value_or(int64_t{60}));
            if (c.audio_frames <= 0) fail(src, "`audio_frames` must be > 0");
            if (c.audio_min_rms && c.audio_max_rms && *c.audio_min_rms > *c.audio_max_rms)
                fail(src, "`audio_min_rms` exceeds `audio_max_rms`");
            c.wav = t["wav"].value_or(false);
            if (auto* ram = t["ram"].as_array()) {
                for (auto& rn : *ram) {
                    auto& rt = *rn.as_table();
                    c.ram.push_back(parse_ram(src, rt));
                }
            }
            s.checks.push_back(std::move(c));
        }
    }
    std::stable_sort(s.checks.begin(), s.checks.end(), [](const Check& a, const Check& b) { return a.at < b.at; });
    return s;
}

Scenario load(const std::string& path) {
    toml::table t;
    try {
        t = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        fail(path, e.what());
    }
    std::vector<std::filesystem::path> seen;
    merge_includes(t, path, seen);
    std::ostringstream ss;
    ss << t;
    return parse(ss.str(), path);
}

int64_t boot_start(const Scenario& s, int64_t frame) {
    int64_t start = 0;
    for (int64_t r : s.reboots)
        if (r <= frame) start = r;
    return start;
}

uint16_t buttons_at(const Scenario& s, int64_t frame) {
    uint16_t b = 0;
    int64_t since_boot = frame - boot_start(s, frame);
    if (s.skip_intro && since_boot >= kIntroSkipFrame && since_boot < kIntroSkipFrame + 4) b |= 1u << 3;  // START
    for (const auto& p : s.presses)
        if (frame >= p.from && frame < p.to) b |= p.button;
    return b;
}

bool Timeline::satisfied(const Wait& w, const RamReader& read) const {
    for (const auto& r : w.ram) {
        uint32_t v = 0;
        if (!read_ram(read, r, v) || !ram_matches(r, v)) return false;
    }
    if (w.steer) {
        uint32_t x = 0, z = 0;
        if (!read_ram(read, w.steer->x, x) || !read_ram(read, w.steer->z, z)) return false;
        if (!steer_reached(*w.steer, sign_extend(x, w.steer->x.size), sign_extend(z, w.steer->z.size))) return false;
    }
    return true;
}

void Timeline::advance(int64_t real_frame, const RamReader& read) {
    real_ = real_frame;
    steer_ = 0;
    if (timed_out_ || guard_failed_) return;
    if (waiting_) {
        const Wait& w = s_->waits[next_];
        if (satisfied(w, read)) {
            waiting_ = false;
            last_wait_frames_ = real_ - wait_start_;
            ++next_;
        } else if (real_ - wait_start_ >= w.timeout) {
            timed_out_ = true;
            return;
        }
    }
    if (!waiting_) {
        script_ = real_ - offset_;
        while (next_ < s_->waits.size() && s_->waits[next_].at <= script_) {
            if (satisfied(s_->waits[next_], read)) {
                last_wait_frames_ = 0;
                ++next_;
                continue;
            }
            waiting_ = true;
            wait_start_ = real_;
            break;
        }
    }
    if (waiting_) {
        const Wait& w = s_->waits[next_];
        script_ = w.at;
        offset_ = real_ - script_;
        for (const auto& g : w.guard) {
            uint32_t v = 0;
            if (!read_ram(read, g, v) || !ram_matches(g, v)) {
                guard_failed_ = true;
                return;
            }
        }
        if (w.steer) {
            uint32_t x = 0, z = 0;
            if (read_ram(read, w.steer->x, x) && read_ram(read, w.steer->z, z))
                steer_ = steer_buttons(*w.steer, sign_extend(x, w.steer->x.size), sign_extend(z, w.steer->z.size));
        }
    }
}

uint16_t Timeline::buttons() const {
    uint16_t b = buttons_at(*s_, script_);
    if (waiting_) {
        const Wait& w = s_->waits[next_];
        b |= w.hold | steer_;
        if (w.tap && (real_ - wait_start_) % w.tap_every < w.tap_hold) b |= w.tap;
    }
    return b;
}

void AudioMeter::add_frame(std::span<const int16_t> pcm) {
    Block b;
    b.first = samples_.size();
    b.count = pcm.size();
    for (int16_t v : pcm) b.sum_sq += static_cast<double>(v) * v;
    blocks_.push_back(b);
    if (keep_) samples_.insert(samples_.end(), pcm.begin(), pcm.end());
}

double AudioMeter::rms(int n) const {
    double sum = 0;
    std::size_t count = 0;
    std::size_t from = blocks_.size() > static_cast<std::size_t>(std::max(n, 0)) ? blocks_.size() - static_cast<std::size_t>(n) : 0;
    for (std::size_t i = from; i < blocks_.size(); ++i) {
        sum += blocks_[i].sum_sq;
        count += blocks_[i].count;
    }
    return count ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

std::vector<int16_t> AudioMeter::window(int n) const {
    std::size_t from = blocks_.size() > static_cast<std::size_t>(std::max(n, 0)) ? blocks_.size() - static_cast<std::size_t>(n) : 0;
    if (!keep_ || from >= blocks_.size()) return {};
    std::size_t start = blocks_[from].first;
    return std::vector<int16_t>(samples_.begin() + static_cast<std::ptrdiff_t>(start), samples_.end());
}

std::vector<uint8_t> wav_bytes(std::span<const int16_t> pcm, uint32_t rate) {
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto put16 = [&](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
    auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    tag("RIFF");
    put32(36 + bytes);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);  // PCM
    put16(2);  // channels
    put32(rate);
    put32(rate * 4);
    put16(4);
    put16(16);
    tag("data");
    put32(bytes);
    for (int16_t v : pcm) put16(static_cast<uint16_t>(v));
    return out;
}

bool write_wav(const std::string& path, std::span<const int16_t> pcm, uint32_t rate) {
    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto bytes = wav_bytes(pcm, rate);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

std::array<uint64_t, 4> perceptual_hash(const uint32_t* rgba, int width, int height) {
    double cell[256] = {};
    for (int cy = 0; cy < 16; ++cy) {
        int y0 = cy * height / 16, y1 = std::max(y0 + 1, (cy + 1) * height / 16);
        for (int cx = 0; cx < 16; ++cx) {
            int x0 = cx * width / 16, x1 = std::max(x0 + 1, (cx + 1) * width / 16);
            double sum = 0;
            for (int y = y0; y < y1 && y < height; ++y)
                for (int x = x0; x < x1 && x < width; ++x) {
                    uint32_t p = rgba[y * width + x];
                    sum += 0.299 * (p & 0xFF) + 0.587 * ((p >> 8) & 0xFF) + 0.114 * ((p >> 16) & 0xFF);
                }
            cell[cy * 16 + cx] = sum / ((y1 - y0) * (x1 - x0));
        }
    }
    double mean = 0;
    for (double c : cell) mean += c;
    mean /= 256.0;
    std::array<uint64_t, 4> h{};
    for (int i = 0; i < 256; ++i)
        if (cell[i] > mean) h[static_cast<std::size_t>(i / 64)] |= uint64_t{1} << (i % 64);
    return h;
}

int hash_distance(const std::array<uint64_t, 4>& a, const std::array<uint64_t, 4>& b) {
    int d = 0;
    for (std::size_t i = 0; i < 4; ++i) d += std::popcount(a[i] ^ b[i]);
    return d;
}

std::string hash_to_hex(const std::array<uint64_t, 4>& h) {
    std::string out;
    char buf[17];
    for (uint64_t w : h) {
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(w));
        out += buf;
    }
    return out;
}

std::optional<std::array<uint64_t, 4>> hash_from_hex(const std::string& hex) {
    if (hex.size() != 64) return std::nullopt;
    std::array<uint64_t, 4> h{};
    for (std::size_t i = 0; i < 4; ++i) {
        uint64_t v = 0;
        for (std::size_t k = 0; k < 16; ++k) {
            char c = hex[i * 16 + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return std::nullopt;
            v = (v << 4) | static_cast<uint64_t>(d);
        }
        h[i] = v;
    }
    return h;
}

}  // namespace dw3::scenario
