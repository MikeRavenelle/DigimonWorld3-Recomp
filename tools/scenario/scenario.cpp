#include "scenario.h"

#include <algorithm>
#include <bit>
#include <cstdio>
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

}  // namespace

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
    if (s.name.empty()) fail(src, "missing `name`");
    if (s.frames <= 0) fail(src, "`frames` must be > 0");
    if (s.memcard != "none" && s.memcard != "blank") fail(src, "`memcard` must be \"none\" or \"blank\"");

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
            if (auto v = t["screen_hash"].value<std::string>()) {
                c.screen_hash = hash_from_hex(*v);
                if (!c.screen_hash) fail(src, "bad `screen_hash` (want 64 hex digits)");
            }
            c.max_hash_distance = static_cast<int>(t["max_hash_distance"].value_or(int64_t{24}));
            c.screenshot = t["screenshot"].value_or(false);
            if (auto* ram = t["ram"].as_array()) {
                for (auto& rn : *ram) {
                    auto& rt = *rn.as_table();
                    RamCheck r;
                    r.addr = static_cast<uint32_t>(rt["addr"].value_or(int64_t{0}));
                    r.size = static_cast<int>(rt["size"].value_or(int64_t{4}));
                    r.value = static_cast<uint32_t>(rt["value"].value_or(int64_t{0}));
                    if (r.size != 1 && r.size != 2 && r.size != 4) fail(src, "ram check size must be 1, 2 or 4");
                    c.ram.push_back(r);
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
    std::ostringstream ss;
    ss << t;
    return parse(ss.str(), path);
}

uint16_t buttons_at(const Scenario& s, int64_t frame) {
    uint16_t b = 0;
    if (s.skip_intro && frame >= kIntroSkipFrame && frame < kIntroSkipFrame + 4) b |= 1u << 3;  // START
    for (const auto& p : s.presses)
        if (frame >= p.from && frame < p.to) b |= p.button;
    return b;
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
