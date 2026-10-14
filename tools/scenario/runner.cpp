// dw3scenario: headless in-game scenario runner.
//
//   dw3scenario <file.toml> [--disc PATH] [--game-dir DIR] [--state-dir DIR] [--record] [--out DIR] [--wav PATH] [--no-xa] [--watch ADDR[:SIZE]]... [--dump FRAME:PATH]...
//
// Boots the recompiled game with the scenario's scripted pad on port 1 (no window, no SDL, uncapped), evaluates
// every check at its frame and writes BMPs of the display to <out>/<name>/<at>_<label>.bmp for checks with
// `screenshot = true` and for failed checks. --record prints the actual screen hash of every check as a
// ready-to-paste `screen_hash = "..."` line and does not fail on hash mismatches. The SPU runs normally (no audio
// device); its output is metered per frame for `audio_*` checks. --wav PATH saves the whole mixed run as a WAV.
// --watch ADDR[:SIZE] (SIZE 1, 2 or 4, default 4; repeatable) prints every change of that RAM location with the
// real and script frame, for finding the addresses that [[wait]] and [[check]] conditions use. --watch
// *PTR+OFFSET[:SIZE] follows the pointer word at PTR first (like `via` in a ram condition). --dump FRAME:PATH
// writes the 2 MiB of main RAM to PATH once the script reaches FRAME (repeatable), for diffing game states; a PATH ending
// in .bmp writes the whole 1024x512 VRAM as an image instead.
// [[savestate]] / [[loadstate]] / start_state entries (see scenario.h) read and write save states under --state-dir
// (default game/audit/states); relative state paths are resolved against it.
//
// Exit codes: 0 all checks pass, 1 failure, 2 usage/scenario error, 77 disc missing (ctest SKIP); the same for a missing "game:" memory card image.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "dw3/host.h"
#include "dw3/hw_renderer.h"
#include "dw3/recomp.h"
#include "dw3/state.h"
#include "dw3/texrep/stage_dump.h"
#include "dw3/upload_recorder.h"
#include "field_menu.h"
#include "audio_groups.h"
#include "gameplay.h"
#include "scenario.h"
#include "frame_stats.h"
#include "present/post_options.h"

namespace {

namespace sc = dw3::scenario;

struct Options {
    std::string file;
    std::string disc = std::string(DW3_SOURCE_DIR) + "/game/Digimon World 3 (USA).cue";
    std::string game_dir;  // --game-dir: base of "game:" memory card paths (default: the disc's directory)
    std::string out = std::string(DW3_SOURCE_DIR) + "/game/audit/scenarios";
    std::string state_dir = std::string(DW3_SOURCE_DIR) + "/game/audit/states";
    bool record = false;
    std::string wav;  // --wav: whole-run audio dump
    std::vector<sc::RamCheck> watches;  // --watch (value/mask unused)
    std::vector<std::pair<int64_t, std::string>> dumps;  // --dump, sorted by frame
    std::string texture_pack;  // --texture-pack: a texture pack directory for the GPU renderer
    std::vector<std::string> settings;  // --setting id=value, applied after the scenario's own
    bool sync_replacements = false;  // --sync-replacements: decode replacement images on the render thread (the old behaviour)
    bool gpu = false;  // --gpu: use the GPU renderer whatever the scenario says
    bool frame_stats = false;  // --frame-stats: per-frame host timing, printed at the end (adds a window-sized render each frame)
    int stats_width = 1920, stats_height = 1080;  // --frame-stats-size WxH: the output size drawn for the stats
    int64_t frames_from = 0, frames_to = -1, frames_step = 1;  // --gpu-frames FROM:TO[:STEP]:DIR: rendered frames in that range of emulated frames as BMPs
    std::string frames_dir;
    bool no_xa = false;  // --no-xa: drop XA-ADPCM sectors (isolates SPU voice output when auditing)
};

// Port 1 pad driven by the scenario's input script.
class ScriptPad final : public dw3::PadSource {
public:
    explicit ScriptPad(const sc::Timeline& t) : timeline_(t) {}
    bool connected(int port) override { return port == 0; }
    uint16_t buttons(int port) override { return port == 0 ? timeline_.buttons() : 0; }

private:
    const sc::Timeline& timeline_;
};

std::string slug(const std::string& label) {
    std::string out;
    for (char c : label) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "check" : out;
}

// 24-bit BMP from RGBA8888 (R in the low byte).
bool write_bmp(const std::filesystem::path& path, const std::vector<uint32_t>& rgba, int w, int h) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t row = (static_cast<uint32_t>(w) * 3 + 3) & ~3u;
    const uint32_t size = 54 + row * static_cast<uint32_t>(h);
    uint8_t hdr[54] = {'B', 'M'};
    auto put32 = [&](int off, uint32_t v) { std::memcpy(hdr + off, &v, 4); };  // little-endian hosts only
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(w));
    put32(22, static_cast<uint32_t>(h));
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, row * static_cast<uint32_t>(h));
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    std::vector<uint8_t> line(row, 0);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            uint32_t p = rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
            line[static_cast<std::size_t>(x) * 3 + 0] = static_cast<uint8_t>(p >> 16);
            line[static_cast<std::size_t>(x) * 3 + 1] = static_cast<uint8_t>(p >> 8);
            line[static_cast<std::size_t>(x) * 3 + 2] = static_cast<uint8_t>(p);
        }
        f.write(reinterpret_cast<const char*>(line.data()), static_cast<std::streamsize>(row));
    }
    return static_cast<bool>(f);
}

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

// What a replayed stretch of the script must reproduce frame by frame.
struct FrameDigest {
    uint64_t ram = 0, vram = 0, regs = 0;
    bool operator==(const FrameDigest&) const = default;
};

uint64_t fnv_words(const void* data, std::size_t bytes) {
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    std::size_t n = bytes / 8;
    for (std::size_t i = 0; i < n; ++i) {
        uint64_t w;
        std::memcpy(&w, p + i * 8, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    return h;
}

FrameDigest digest_of(dw3::Machine& m) {
    FrameDigest d;
    d.ram = fnv_words(m.bus().ram(), DW3_RAM_SIZE);
    auto v = m.gpu().vram();
    d.vram = fnv_words(v.data(), v.size() * sizeof(uint16_t));
    const RecompContext& c = m.ctx();
    uint64_t regs[] = {c.hi, c.lo, static_cast<uint64_t>(c.cycles), c.r[29], c.r[31], c.r[4], c.r[2], c.r[16]};
    d.regs = fnv_words(regs, sizeof(regs));
    return d;
}

std::string resolve_state_path(const Options& opt, const std::string& p) {
    std::filesystem::path path(p);
    return path.is_absolute() ? p : (std::filesystem::path(opt.state_dir) / path).string();
}

struct Results {
    int pass = 0, fail = 0;
};

void report(Results& r, bool ok, const std::string& field, const std::string& detail) {
    std::printf("    %s %s: %s\n", ok ? "PASS" : "FAIL", field.c_str(), detail.c_str());
    (ok ? r.pass : r.fail)++;
}

// The keys of every field map background on the disc: block hash -> palette hashes the files give it.
using DiscKeys = std::map<uint64_t, std::set<uint64_t>>;

bool load_disc_keys(const std::string& disc, DiscKeys& keys, std::string& error) {
    auto source = dw3::texrep::StageSource::open(disc, error);
    if (!source) return false;
    std::vector<uint8_t> data;
    for (const std::string& map : source->maps()) {
        dw3::texrep::StagePack pack;
        if (!source->read(map, data, error) || !dw3::texrep::parse_stage_pack(data, pack, error)) {
            error = map + ": " + error;
            return false;
        }
        for (const dw3::texrep::StageTile& t : pack.tiles)
            if (t.tracked()) keys[t.block_hash].insert(t.clut_hash);
    }
    return true;
}

// Evaluates one check against the live machine. Returns the number of failures it added.
int evaluate(const Options& opt, const sc::Scenario& s, const sc::Check& c, dw3::host::Host& host, Results& total,
             const sc::AudioMeter& meter, const dw3::UploadRecorder* recorder, const DiscKeys& disc_keys,
             dw3::HwRenderer* hw) {
    dw3::Machine& m = host.machine();
    Results r;
    std::printf("  check @%lld \"%s\"\n", static_cast<long long>(c.at), c.label.c_str());
    const auto& d = m.gpu().display();
    auto num = [](auto v) { return std::to_string(v); };
    if (c.display_enabled)
        report(r, d.enabled == *c.display_enabled, "display_enabled",
               "actual " + std::string(d.enabled ? "true" : "false") + ", expected " + (*c.display_enabled ? "true" : "false"));
    if (c.display_width)
        report(r, d.width == *c.display_width, "display_width", "actual " + num(d.width) + ", expected " + num(*c.display_width));
    if (c.display_height)
        report(r, d.height == *c.display_height, "display_height",
               "actual " + num(d.height) + ", expected " + num(*c.display_height));
    if (c.min_primitives)
        report(r, m.gpu().primitives() >= *c.min_primitives, "min_primitives",
               "actual " + num(m.gpu().primitives()) + ", expected >= " + num(*c.min_primitives));
    if (c.min_commands)
        report(r, m.gpu().commands() >= *c.min_commands, "min_commands",
               "actual " + num(m.gpu().commands()) + ", expected >= " + num(*c.min_commands));
    if (c.min_disc_backgrounds) {
        int matched = 0, wrong_palette = 0;
        if (recorder)
            for (const auto& [block, palette] : recorder->keys()) {
                const auto it = disc_keys.find(block);
                if (it == disc_keys.end()) continue;
                if (it->second.count(palette)) ++matched;
                else ++wrong_palette;
            }
        report(r, matched >= *c.min_disc_backgrounds && wrong_palette == 0, "disc_backgrounds",
               num(matched) + " keys drawn so far are on the disc, " + num(wrong_palette) + " with another palette, expected >= " +
                   num(*c.min_disc_backgrounds) + " and none with another palette");
    }
    if (c.tty_contains) {
        bool ok = host.tty().find(*c.tty_contains) != std::string::npos;
        report(r, ok, "tty_contains", std::string(ok ? "found" : "not found") + " \"" + *c.tty_contains + "\"");
    }
    auto read = [&](uint32_t addr, int size) -> uint32_t {
        RecompContext* ctx = &m.ctx();
        return size == 1 ? dw3_read8(ctx, addr) : size == 2 ? dw3_read16(ctx, addr) : dw3_read32(ctx, addr);
    };
    for (const auto& ram : c.ram) {
        uint32_t v = 0;
        const bool ok = sc::read_ram(read, ram, v);
        const std::string name = (ram.via ? hex32(ram.via) + "->" : std::string()) + hex32(ram.addr) + "/" + num(ram.size);
        report(r, ok && sc::ram_matches(ram, v), "ram[" + name + "]",
               ok ? "actual " + hex32(v) + ", expected " + hex32(ram.value) : "pointer " + hex32(ram.via) + " is not a RAM address");
    }

    if (c.audio_min_rms || c.audio_max_rms) {
        double rms = meter.rms(c.audio_frames);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.1f over %d frames", rms, c.audio_frames);
        if (c.audio_min_rms)
            report(r, rms >= *c.audio_min_rms, "audio_min_rms", std::string(buf) + ", expected >= " + std::to_string(*c.audio_min_rms));
        if (c.audio_max_rms)
            report(r, rms <= *c.audio_max_rms, "audio_max_rms", std::string(buf) + ", expected <= " + std::to_string(*c.audio_max_rms));
    }
    if (opt.record)
        std::printf("    RECORD audio rms %.1f over %d frames\n", meter.rms(c.audio_frames), c.audio_frames);
    if (c.wav) {
        auto path = std::filesystem::path(opt.out) / s.name / (num(c.at) + "_" + slug(c.label) + ".wav");
        auto pcm = meter.window(c.audio_frames);
        if (sc::write_wav(path.string(), pcm)) std::printf("    audio %s\n", path.string().c_str());
        else std::printf("    cannot write %s\n", path.string().c_str());
    }

    std::vector<uint32_t> pixels;
    int w = 0, h = 0;
    bool have_image = m.gpu().snapshot_display(pixels, w, h);
    if (c.screen_hash || opt.record) {
        if (!have_image) {
            if (c.screen_hash) report(r, false, "screen_hash", "display is disabled, no image");
            else std::printf("    (display disabled; no hash)\n");
        } else {
            auto actual = sc::perceptual_hash(pixels.data(), w, h);
            if (opt.record) {
                std::printf("    RECORD screen_hash = \"%s\"\n", sc::hash_to_hex(actual).c_str());
                if (c.screen_hash) {
                    int dist = sc::hash_distance(actual, *c.screen_hash);
                    std::printf("    INFO   previous hash distance %d (max %d)\n", dist, c.max_hash_distance);
                }
            } else {
                int dist = sc::hash_distance(actual, *c.screen_hash);
                report(r, dist <= c.max_hash_distance, "screen_hash",
                       "distance " + num(dist) + ", allowed <= " + num(c.max_hash_distance) + ", actual " +
                           sc::hash_to_hex(actual));
            }
        }
    }
    // The GPU renderer's frame as a 1280x720 window would show it.
    std::vector<uint32_t> gpu_pixels;
    constexpr int kGpuW = 1280, kGpuH = 720;
    const bool gpu_image = hw && hw->render_to_image(m.gpu().display(), dw3::ScaleMode::Fit, false, nullptr, kGpuW, kGpuH, gpu_pixels);
    if (c.backdrop_pieces_min || c.squares_hidden_min || c.squares_drawn_min || c.squares_drawn_max) {
        if (!hw) {
            report(r, false, "backdrop", "no GPU renderer");
        } else {
            const dw3::HwStats& st = hw->stats();
            if (c.backdrop_pieces_min) report(r, st.backdrop_pieces >= *c.backdrop_pieces_min, "backdrop_pieces_min", "drawn " + num(st.backdrop_pieces) + ", expected >= " + num(*c.backdrop_pieces_min));
            if (c.squares_hidden_min) report(r, st.squares_hidden >= *c.squares_hidden_min, "squares_hidden_min", "hidden " + num(st.squares_hidden) + ", expected >= " + num(*c.squares_hidden_min));
            if (c.squares_drawn_min) report(r, st.squares_drawn >= *c.squares_drawn_min, "squares_drawn_min", "drawn " + num(st.squares_drawn) + ", expected >= " + num(*c.squares_drawn_min));
            if (c.squares_drawn_max) report(r, st.squares_drawn <= *c.squares_drawn_max, "squares_drawn_max", "drawn " + num(st.squares_drawn) + ", expected <= " + num(*c.squares_drawn_max));
        }
    }
    if (c.wide_field_frames_min || c.wide_field_fallbacks_max || c.wide_margin_polygons_min || c.wide_margin_polygons_max) {
        if (!hw) {
            report(r, false, "wide_field", "no GPU renderer");
        } else {
            const dw3::HwStats& st = hw->stats();
            if (c.wide_field_frames_min) report(r, st.wide_field_frames >= *c.wide_field_frames_min, "wide_field_frames_min", "shown wide " + num(st.wide_field_frames) + ", expected >= " + num(*c.wide_field_frames_min));
            if (c.wide_field_fallbacks_max) report(r, st.wide_field_fallbacks <= *c.wide_field_fallbacks_max, "wide_field_fallbacks_max", "shown at 4:3 " + num(st.wide_field_fallbacks) + ", expected <= " + num(*c.wide_field_fallbacks_max));
            if (c.wide_margin_polygons_min) report(r, st.wide_margin_polygons >= *c.wide_margin_polygons_min, "wide_margin_polygons_min", "drawn " + num(st.wide_margin_polygons) + ", expected >= " + num(*c.wide_margin_polygons_min));
            if (c.wide_margin_polygons_max) report(r, st.wide_margin_polygons <= *c.wide_margin_polygons_max, "wide_margin_polygons_max", "drawn " + num(st.wide_margin_polygons) + ", expected <= " + num(*c.wide_margin_polygons_max));
        }
    }
    if (c.margin_colors_min || c.margin_colors_max) {
        if (!gpu_image) {
            report(r, false, "margin_colors", "no GPU renderer frame");
        } else {
            std::set<uint32_t> colours;
            constexpr int kMargin = kGpuW / 10;
            for (int y = 0; y < kGpuH; ++y)
                for (int x = 0; x < kGpuW; ++x)
                    if (x < kMargin || x >= kGpuW - kMargin) colours.insert(gpu_pixels[static_cast<std::size_t>(y) * kGpuW + x]);
            const int n = static_cast<int>(colours.size());
            if (c.margin_colors_min) report(r, n >= *c.margin_colors_min, "margin_colors_min", "actual " + num(n) + ", expected >= " + num(*c.margin_colors_min));
            if (c.margin_colors_max) report(r, n <= *c.margin_colors_max, "margin_colors_max", "actual " + num(n) + ", expected <= " + num(*c.margin_colors_max));
        }
    }
    if (gpu_image && (c.screenshot || r.fail > 0)) {
        auto path = std::filesystem::path(opt.out) / s.name / (num(c.at) + "_" + slug(c.label) + "_gpu.bmp");
        if (write_bmp(path, gpu_pixels, kGpuW, kGpuH)) std::printf("    screenshot %s\n", path.string().c_str());
    }
    if (have_image && (c.screenshot || r.fail > 0)) {
        auto path = std::filesystem::path(opt.out) / s.name / (num(c.at) + "_" + slug(c.label) + ".bmp");
        if (write_bmp(path, pixels, w, h)) std::printf("    screenshot %s\n", path.string().c_str());
        else std::printf("    cannot write %s\n", path.string().c_str());
    }
    total.pass += r.pass;
    total.fail += r.fail;
    return r.fail;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--disc" && i + 1 < argc) opt.disc = argv[++i];
        else if (a == "--game-dir" && i + 1 < argc) opt.game_dir = argv[++i];
        else if (a == "--out" && i + 1 < argc) opt.out = argv[++i];
        else if (a == "--texture-pack" && i + 1 < argc) opt.texture_pack = argv[++i];
        else if (a == "--state-dir" && i + 1 < argc) opt.state_dir = argv[++i];
        else if (a == "--record") opt.record = true;
        else if (a == "--wav" && i + 1 < argc) opt.wav = argv[++i];
        else if (a == "--no-xa") opt.no_xa = true;
        else if (a == "--setting" && i + 1 < argc) opt.settings.push_back(argv[++i]);
        else if (a == "--gpu") opt.gpu = true;
        else if (a == "--sync-replacements") opt.sync_replacements = true;
        else if (a == "--frame-stats") opt.frame_stats = true;
        else if (a == "--frame-stats-size" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &opt.stats_width, &opt.stats_height) != 2) {
                std::fprintf(stderr, "--frame-stats-size wants WxH\n");
                return 2;
            }
        }
        else if (a == "--gpu-frames" && i + 1 < argc) {
            const std::string v = argv[++i];
            const auto c1 = v.find(':'), c2 = v.find(':', c1 + 1), c3 = v.find(':', c2 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) {
                std::fprintf(stderr, "--gpu-frames wants FROM:TO[:STEP]:DIR\n");
                return 2;
            }
            opt.frames_from = std::stoll(v.substr(0, c1));
            opt.frames_to = std::stoll(v.substr(c1 + 1, c2 - c1 - 1));
            if (c3 != std::string::npos) opt.frames_step = std::max<int64_t>(1, std::stoll(v.substr(c2 + 1, c3 - c2 - 1)));
            opt.frames_dir = v.substr(c3 == std::string::npos ? c2 + 1 : c3 + 1);
            opt.gpu = true;
        } else if (a == "--dump" && i + 1 < argc) {
            std::string v = argv[++i];
            auto colon = v.find(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "--dump wants FRAME:PATH\n");
                return 2;
            }
            opt.dumps.emplace_back(std::stoll(v.substr(0, colon)), v.substr(colon + 1));
            std::sort(opt.dumps.begin(), opt.dumps.end());
        } else if (a == "--watch" && i + 1 < argc) {
            std::string v = argv[++i];
            sc::RamCheck w;
            auto colon = v.find(':');
            w.size = colon == std::string::npos ? 4 : std::stoi(v.substr(colon + 1));
            std::string loc = v.substr(0, colon);
            if (!loc.empty() && loc[0] == '*') {
                auto plus = loc.find('+');
                if (plus == std::string::npos) {
                    std::fprintf(stderr, "--watch *PTR+OFFSET[:SIZE]\n");
                    return 2;
                }
                w.via = static_cast<uint32_t>(std::stoul(loc.substr(1, plus - 1), nullptr, 0));
                w.addr = static_cast<uint32_t>(std::stoul(loc.substr(plus + 1), nullptr, 0));
            } else {
                w.addr = static_cast<uint32_t>(std::stoul(loc, nullptr, 0));
            }
            if (w.size != 1 && w.size != 2 && w.size != 4) {
                std::fprintf(stderr, "--watch size must be 1, 2 or 4\n");
                return 2;
            }
            opt.watches.push_back(w);
        }
        else if (!a.empty() && a[0] != '-' && opt.file.empty()) opt.file = a;
        else {
            std::fprintf(stderr, "usage: dw3scenario <file.toml> [--setting ID=VALUE] [--gpu] [--frame-stats] [--frame-stats-size WxH] [--sync-replacements] [--disc PATH] [--game-dir DIR] [--state-dir DIR] [--record] [--out DIR] [--wav PATH] [--no-xa] [--watch ADDR[:SIZE]] [--dump FRAME:PATH]\n");
            return 2;
        }
    }
    if (opt.file.empty()) {
        std::fprintf(stderr, "usage: dw3scenario <file.toml> [--disc PATH] [--game-dir DIR] [--state-dir DIR] [--record] [--out DIR] [--wav PATH] [--no-xa] [--watch ADDR[:SIZE]] [--dump FRAME:PATH]\n");
        return 2;
    }
    sc::Scenario scenario;
    try {
        scenario = sc::load(opt.file);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scenario error: %s\n", e.what());
        return 2;
    }
    std::error_code ec;
    if (!std::filesystem::exists(opt.disc, ec)) {
        std::printf("SKIP: disc image not found: %s\n", opt.disc.c_str());
        return 77;
    }

    if (opt.game_dir.empty()) opt.game_dir = std::filesystem::path(opt.disc).parent_path().string();
    dw3::host::Config cfg;
    cfg.disc_path = opt.disc;
    cfg.disc_index_cache = (std::filesystem::path(opt.state_dir) / "disc.index").string();
    if (scenario.memcard == "none") cfg.card_mode = dw3::host::CardMode::None;
    else if (scenario.memcard == "blank") cfg.card_mode = dw3::host::CardMode::Blank;
    else if (scenario.memcard.rfind("game:", 0) == 0) {
        std::filesystem::path card = std::filesystem::path(opt.game_dir) / scenario.memcard.substr(5);
        std::ifstream f(card, std::ios::binary);
        if (!f) {
            std::printf("SKIP: memory card image not found: %s\n", card.string().c_str());
            return 77;
        }
        cfg.blank_card.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (cfg.blank_card.size() != 128 * 1024) {
            std::fprintf(stderr, "scenario error: %s is not a 128 KiB card image\n", card.string().c_str());
            return 2;
        }
        // Images converted from DexDrive files can leave the header frame's checksum at zero, which the game's card
        // library rejects as an unformatted card. Repair it in memory (the file itself is never touched).
        auto& img = cfg.blank_card;
        if (img[0] == 'M' && img[1] == 'C' && img[127] == 0) img[127] = static_cast<uint8_t>('M' ^ 'C');
        cfg.card_mode = dw3::host::CardMode::Blank;
    } else {
        std::fprintf(stderr, "scenario error: unknown memcard \"%s\"\n", scenario.memcard.c_str());
        return 2;
    }
    cfg.log = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); };
    std::string error;
    std::unique_ptr<dw3::host::Host> host;
    std::unique_ptr<dw3::HwRenderer> hw;  // after the host so it goes first; null: the software rasterizer draws
    sc::Timeline timeline(scenario);
    DiscKeys disc_keys;
    std::unique_ptr<dw3::UploadRecorder> recorder;
    const bool wants_disc_keys = std::any_of(scenario.checks.begin(), scenario.checks.end(), [](const sc::Check& c) { return c.min_disc_backgrounds.has_value(); });
    if (wants_disc_keys && !load_disc_keys(opt.disc, disc_keys, error)) {
        std::fprintf(stderr, "scenario error: reading the stage packs of the disc: %s\n", error.c_str());
        return 2;
    }
    ScriptPad pad(timeline);
    auto read_ram = [&](uint32_t addr, int size) -> uint32_t {
        RecompContext* ctx = &host->machine().ctx();
        return size == 1 ? dw3_read8(ctx, addr) : size == 2 ? dw3_read16(ctx, addr) : dw3_read32(ctx, addr);
    };
    std::vector<uint32_t> watch_last(opt.watches.size(), 0);
    std::vector<bool> watch_seen(opt.watches.size(), false);
    std::size_t waits_seen = 0, dumps_done = 0, pokes_done = 0;
    // [[margin_watch]]: per window, the lit state of the outer columns of each rendered frame and the frames kept for a strip.
    struct MarginTrace {
        int changes = 0, frames = 0;
        int lit = -1;
        std::vector<std::vector<uint32_t>> kept;
        std::vector<char> kept_lit;
    };
    std::vector<MarginTrace> margin_traces(scenario.margin_watches.size());
    auto trace_margins = [&](int64_t script) {
        if (!hw) return;
        std::vector<uint32_t> px;
        constexpr int kW = 1280, kH = 720, kOuter = kW / 25;
        bool rendered = false;
        for (std::size_t i = 0; i < scenario.margin_watches.size(); ++i) {
            const auto& w = scenario.margin_watches[i];
            if (script < w.from || script > w.to) continue;
            if (!rendered) rendered = hw->render_to_image(host->machine().gpu().display(), dw3::ScaleMode::Fit, false, nullptr, kW, kH, px);
            if (!rendered) return;
            bool lit = false;
            for (int y = 0; y < kH && !lit; ++y)
                for (int x = 0; x < kW && !lit; ++x) {
                    if (x == kOuter) x = kW - kOuter;
                    const uint32_t p = px[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
                    lit = ((p & 0xFF) | ((p >> 8) & 0xFF) | ((p >> 16) & 0xFF)) > 8;
                }
            MarginTrace& t = margin_traces[i];
            if (t.lit >= 0 && t.lit != (lit ? 1 : 0)) ++t.changes;
            t.lit = lit ? 1 : 0;
            ++t.frames;
            if (w.strip && t.kept.size() < 48) {
                // The few frames before the first switch, then the frames after it.
                t.kept.push_back(px);
                t.kept_lit.push_back(lit ? 1 : 0);
                if (t.changes == 0 && t.kept.size() > 6) {
                    t.kept.erase(t.kept.begin());
                    t.kept_lit.erase(t.kept_lit.begin());
                }
            }
        }
    };
    // [[flicker_watch]]: per window, the last two different pictures (grey, 640x360) and the most flickering blocks seen.
    struct FlickerTrace {
        std::vector<uint8_t> older, last;
        int frames = 0, pictures = 0;
        int worst_blocks = 0;
    };
    std::vector<FlickerTrace> flicker_traces(scenario.flicker_watches.size());
    auto trace_flicker = [&](int64_t script) {
        if (!hw) return;
        std::vector<uint32_t> px;
        bool rendered = false;
        constexpr int kW = 1280, kH = 720, kGreyW = kW / 2, kGreyH = kH / 2;
        std::vector<uint8_t> grey;
        for (std::size_t i = 0; i < scenario.flicker_watches.size(); ++i) {
            const auto& w = scenario.flicker_watches[i];
            if (script < w.from || script > w.to) continue;
            if (!rendered) {
                rendered = hw->render_to_image(host->machine().gpu().display(), dw3::ScaleMode::Fit, false, nullptr, kW, kH, px);
                if (!rendered) return;
                grey.resize(static_cast<std::size_t>(kGreyW) * kGreyH);
                for (int y = 0; y < kGreyH; ++y)
                    for (int x = 0; x < kGreyW; ++x) {
                        const uint32_t p = px[static_cast<std::size_t>(y * 2) * kW + static_cast<std::size_t>(x * 2)];
                        grey[static_cast<std::size_t>(y) * kGreyW + static_cast<std::size_t>(x)] =
                            static_cast<uint8_t>((((p & 0xFF) * 77) + (((p >> 8) & 0xFF) * 151) + (((p >> 16) & 0xFF) * 28)) >> 8);
                    }
            }
            FlickerTrace& t = flicker_traces[i];
            ++t.frames;
            if (!t.last.empty() && t.last == grey) continue;
            if (!t.older.empty()) {
                constexpr int kBlock = 16;
                std::vector<int> flicker((kGreyW / kBlock) * (kGreyH / kBlock + 1), 0);
                for (int y = 0; y < kGreyH; ++y)
                    for (int x = 0; x < kGreyW; ++x) {
                        const std::size_t k = static_cast<std::size_t>(y) * kGreyW + static_cast<std::size_t>(x);
                        const int to_middle = std::abs(static_cast<int>(t.last[k]) - static_cast<int>(t.older[k]));
                        const int outer = std::abs(static_cast<int>(grey[k]) - static_cast<int>(t.older[k]));
                        if (to_middle > 24 && outer < 8) ++flicker[static_cast<std::size_t>((y / kBlock) * (kGreyW / kBlock) + x / kBlock)];
                    }
                const int blocks = static_cast<int>(std::count_if(flicker.begin(), flicker.end(), [](int n) { return n * 4 >= kBlock * kBlock; }));
                t.worst_blocks = std::max(t.worst_blocks, blocks);
            }
            t.older = std::move(t.last);
            t.last = grey;
            ++t.pictures;
        }
    };
    bool wait_timeout = false;
    int64_t frames_written = 0;

    bool want_wav = !opt.wav.empty();
    for (const auto& c : scenario.checks) want_wav = want_wav || c.wav;
    sc::AudioMeter meter(want_wav);
    std::vector<int16_t> pcm_buf(4096);

    // Save states. A [[savestate]] snapshots the runner next to the machine so that a replayed [[loadstate]] can
    // rewind the script and check that the second pass reproduces the first one frame by frame.
    struct Snapshot {
        sc::Timeline timeline;
        std::size_t next_check = 0, waits_seen = 0;
        int64_t last_frame = 0;
        std::vector<FrameDigest> digests;  // frames run since the state was taken (first pass)
        bool recording = false;
    };
    std::map<std::string, Snapshot> snapshots;
    std::size_t save_next = 0, load_next = 0;
    int64_t compare_index = 0;
    const Snapshot* comparing = nullptr;
    bool state_failed = false;
    int64_t replayed_frames = 0;
    std::string state_error;
    auto fail_state = [&](const std::string& what) {
        state_failed = true;
        state_error = what;
    };

    std::printf("scenario %s: %s\n", scenario.name.c_str(), scenario.description.c_str());
    Results total;
    std::size_t next = 0;
    int64_t last_frame = 0;
    auto t0 = std::chrono::steady_clock::now();
    dw3::host::RunResult result;
    // --frame-stats: host time per frame. "cpu" is the emulation and the renderer's recording between two callbacks,
    // "present" a window-sized render with the output filters (the app's present without the swap chain), "host" their
    // sum. The renderer's own counters (GPU waits, submits, replacement image loads) are recorded per frame too.
    using StatsClock = std::chrono::steady_clock;
    dw3app::FrameStats frame_stats;
    StatsClock::time_point stats_last_end{};
    dw3::HwStats stats_before;
    auto stats_ns = [](StatsClock::time_point a, StatsClock::time_point b) {
        return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    };
    // One boot per [[reboot]]: each gets a fresh machine; the memory card image carries over.
    for (std::size_t boot = 0; boot <= scenario.reboots.size(); ++boot) {
        if (host) cfg.blank_card = host->card_image(0);
        host.reset();
        hw.reset();
        host = dw3::host::Host::create(cfg, error);
        if (!host) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        if (opt.no_xa) host->machine().cdrom().set_xa_sink(nullptr);
        host->machine().sio().set_pad_source(0, &pad);
        if (wants_disc_keys) {
            recorder = std::make_unique<dw3::UploadRecorder>(host->machine().gpu().rasterizer());
            host->machine().gpu().set_rasterizer(recorder.get());
        }
        // The game's settings screen runs as in the app, on default settings that are never written anywhere.
        dw3app::Settings scenario_settings;
        std::vector<std::string> entries = scenario.settings;
        entries.insert(entries.end(), opt.settings.begin(), opt.settings.end());
        for (const std::string& entry : entries) {
            const std::size_t eq = entry.find('=');
            const dw3app::SettingInfo* info = dw3app::find_setting(entry.substr(0, eq));
            std::string error;
            if (!info || !dw3app::set_text(scenario_settings, *info, entry.substr(eq + 1), error)) {
                std::fprintf(stderr, "scenario setting %s: %s\n", entry.c_str(), info ? error.c_str() : "unknown id");
                return 2;
            }
        }
        if (scenario.gpu || opt.gpu || scenario_settings.graphics.widescreen != 0 || scenario_settings.graphics.seamless_bg) {
            dw3::HwOptions hw_options;
            hw_options.true_color = scenario_settings.graphics.true_color;
            hw_options.filter = static_cast<dw3::TextureFilter>(scenario_settings.graphics.texture_filter);
            hw_options.precise_geometry = scenario_settings.graphics.precise_geometry;
            hw_options.synchronous_replacements = !opt.frame_stats || opt.sync_replacements;
#if defined(__APPLE__)
            hw_options.api = dw3::GraphicsApi::Metal;
#endif
            hw_options.seamless_inside = scenario_settings.graphics.seamless_bg;
            if (scenario_settings.graphics.seamless_bg || scenario_settings.graphics.widescreen != 0) {
                std::string library_error;
                hw_options.stage_library = dw3::texrep::StageLibrary::open_async(opt.disc, library_error);
                if (!hw_options.stage_library) {
                    std::fprintf(stderr, "scenario error: seamless_bg: %s\n", library_error.c_str());
                    return 2;
                }
                hw_options.stage_library->wait_until_ready();  // the same picture on every run
            }
            if (!opt.texture_pack.empty()) {
                std::string pack_error;
                hw_options.texture_pack = dw3::texrep::TexturePack::load(opt.texture_pack, pack_error);
                if (!hw_options.texture_pack) {
                    std::fprintf(stderr, "scenario error: texture pack: %s\n", pack_error.c_str());
                    return 2;
                }
            }
            hw_options.wide_frame = dw3::make_wide_frame(static_cast<dw3::WideAspect>(scenario_settings.graphics.widescreen));
            std::string hw_error;
            hw = dw3::HwRenderer::create(nullptr, {}, hw_error, scenario_settings.graphics.internal_res, hw_options);
            if (!hw) {
                std::printf("SKIP no usable GPU: %s\n", hw_error.c_str());
                return 77;
            }
            host->machine().gpu().set_rasterizer(hw.get());
            if (scenario_settings.graphics.precise_geometry) host->machine().enable_precise_geometry();
        }
        dw3app::SettingsController settings_controller(scenario_settings, scenario_settings, nullptr, {});
        dw3app::ingame::FieldMenuSettings settings_screen(host->machine(), settings_controller);
        dw3app::ingame::GameplayHooks gameplay(host->machine(), settings_controller);
        dw3app::ingame::AudioGroups audio_groups(host->machine(), settings_controller);
        int64_t base = last_frame;  // real frames before this boot; shifted when a state is loaded
        if (boot == 0 && !scenario.start_state.empty()) {
            std::vector<uint8_t> file;
            std::string err;
            const std::string path = resolve_state_path(opt, scenario.start_state);
            if (!dw3::savestate::read_file(path, file, err)) {
                std::printf("FAIL start_state: %s (does the fixture scenario that writes it run first?)\n", err.c_str());
                return 1;
            }
            host->load_state(std::move(file), [&](const std::string& e) {
                if (!e.empty()) fail_state("start_state " + path + ": " + e);
            });
            if (state_failed) {
                std::printf("FAIL %s\n", state_error.c_str());
                return 1;
            }
            // Real frames continue from the script frame the state belongs to.
            base = scenario.start_frame - static_cast<int64_t>(host->machine().video().frame_count());
            last_frame = scenario.start_frame;
            timeline.seek(scenario.start_frame, scenario.start_frame);
            std::printf("  start_state %s at script frame %lld\n", path.c_str(), static_cast<long long>(scenario.start_frame));
        }
        const int64_t until = boot < scenario.reboots.size() ? scenario.reboots[boot] : -1;  // script frame
        if (boot) std::printf("  reboot @%lld\n", static_cast<long long>(scenario.reboots[boot - 1]));
        const dw3::PostFx stats_post = dw3app::post_fx_from(scenario_settings.graphics);
        result = host->run([&](uint64_t frame) {
            int64_t stats_cpu = -1;
            if (opt.frame_stats && hw && stats_last_end != StatsClock::time_point{}) {
                stats_cpu = stats_ns(stats_last_end, StatsClock::now());
                frame_stats.series("cpu").add(stats_cpu);
                const dw3::HwStats& now_stats = hw->stats();
                frame_stats.series("gpu_wait").add(static_cast<int64_t>(now_stats.gpu_wait_ns - stats_before.gpu_wait_ns));
                frame_stats.series("submit").add(static_cast<int64_t>(now_stats.submit_ns - stats_before.submit_ns));
                frame_stats.series("replace").add(static_cast<int64_t>(now_stats.replacement_ns - stats_before.replacement_ns));
            }
            const std::size_t checks_at_start = next;
            struct StatsGuard {
                std::function<void()> finish;
                ~StatsGuard() { finish(); }
            } stats_guard{[&] {
                if (!opt.frame_stats || !hw) return;
                const auto began = StatsClock::now();
                std::vector<uint32_t> shown;
                hw->render_to_image(host->machine().gpu().display(), dw3::ScaleMode::Fit, scenario_settings.graphics.output_filter >= 1,
                                    &stats_post, opt.stats_width, opt.stats_height, shown);
                const int64_t present = stats_ns(began, StatsClock::now());
                // A check renders at its own size, so this frame's render had to resize the capture target: not a frame cost.
                if (stats_cpu >= 0 && next == checks_at_start) {
                    frame_stats.series("present").add(present);
                    frame_stats.series("host").add(stats_cpu + present);
                }
                stats_last_end = StatsClock::now();
                stats_before = hw->stats();
            }};
            settings_screen.apply_pending();
            if (hw) hw->set_wide_mode(dw3::wide_mode_for_game_mode(read_ram(0x800554D0, 4)));
            last_frame = base + static_cast<int64_t>(frame);
            timeline.advance(last_frame, read_ram);
            const int64_t script = timeline.script_frame();
            trace_margins(script);
            trace_flicker(script);
            if (hw && last_frame >= opt.frames_from && last_frame <= opt.frames_to && (last_frame - opt.frames_from) % opt.frames_step == 0) {
                std::vector<uint32_t> px;
                const dw3::PostFx post = dw3app::post_fx_from(scenario_settings.graphics);
                if (hw->render_to_image(host->machine().gpu().display(), dw3::ScaleMode::Fit, scenario_settings.graphics.output_filter >= 1, &post, opt.stats_width, opt.stats_height, px)) {
                    char name[32];
                    std::snprintf(name, sizeof name, "f%06lld.bmp", static_cast<long long>(frames_written++));
                    write_bmp(std::filesystem::path(opt.frames_dir) / name, px, opt.stats_width, opt.stats_height);
                }
            }
            while (dumps_done < opt.dumps.size() && script >= opt.dumps[dumps_done].first) {
                const auto& d = opt.dumps[dumps_done++];
                if (std::filesystem::path(d.second).extension() == ".bmp") {
                    std::vector<uint32_t> vram;
                    host->machine().gpu().snapshot_vram(vram);
                    write_bmp(d.second, vram, dw3::kVramWidth, dw3::kVramHeight);
                } else {
                    std::ofstream f(d.second, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(host->machine().bus().ram()), 2 * 1024 * 1024);
                }
                std::printf("  dump @%lld -> %s\n", static_cast<long long>(script), d.second.c_str());
            }
            while (pokes_done < scenario.pokes.size() && script >= scenario.pokes[pokes_done].at) {
                const auto& p = scenario.pokes[pokes_done++];
                uint8_t* ram = host->machine().bus().ram();
                const uint32_t offset = p.addr & (DW3_RAM_SIZE - 1);
                for (int b = 0; b < p.size; ++b) ram[offset + b] = static_cast<uint8_t>(p.value >> (8 * b));
            }
            for (std::size_t i = 0; i < opt.watches.size(); ++i) {
                const auto& w = opt.watches[i];
                uint32_t v = 0;
                const bool valid = sc::read_ram(read_ram, w, v);
                const uint32_t shown = valid ? v : 0xDEADDEADu;  // unreadable pointer
                if (watch_seen[i] && shown == watch_last[i]) continue;
                std::printf("  watch %s%s/%d = %s  (real %lld, script %lld)\n", w.via ? (hex32(w.via) + "->").c_str() : "",
                            hex32(w.addr).c_str(), w.size, valid ? hex32(v).c_str() : "(no pointer)",
                            static_cast<long long>(last_frame), static_cast<long long>(script));
                watch_last[i] = shown;
                watch_seen[i] = true;
            }
            while (waits_seen < timeline.completed_waits()) {
                const auto& w = scenario.waits[waits_seen++];
                std::printf("  wait @%lld \"%s\": satisfied after %lld frames (timeout %lld)\n", static_cast<long long>(w.at),
                            w.label.c_str(), static_cast<long long>(timeline.last_wait_frames()), static_cast<long long>(w.timeout));
            }
            if (timeline.failed()) {
                wait_timeout = true;
                dw3::host::Host::stop();
                return;
            }
            for (;;) {  // everything the SPU produced during this video frame
                std::size_t n = host->machine().spu().drain_samples(pcm_buf);
                meter.add_frame(std::span<const int16_t>(pcm_buf.data(), n * 2));
                if (n < pcm_buf.size() / 2) break;
            }
            if (until >= 0 && script >= until) dw3::host::Host::stop();
            // Replay bookkeeping: record the frames after a save, compare them after a replayed load.
            if (comparing) {
                FrameDigest d = digest_of(host->machine());
                const FrameDigest& want = comparing->digests[static_cast<std::size_t>(compare_index)];
                if (!(d == want)) {
                    std::printf("  FAIL replay diverged %lld frames after the state was loaded (script frame %lld): %s%s%s\n",
                                static_cast<long long>(compare_index + 1), static_cast<long long>(script),
                                d.ram != want.ram ? "RAM " : "", d.vram != want.vram ? "VRAM " : "",
                                d.regs != want.regs ? "CPU/time " : "");
                    fail_state("replay diverged");
                    dw3::host::Host::stop();
                }
                if (++compare_index == static_cast<int64_t>(comparing->digests.size())) {
                    replayed_frames = compare_index;
                    std::printf("  PASS replay: %lld frames identical (RAM, VRAM, CPU state)\n", static_cast<long long>(compare_index));
                    comparing = nullptr;
                }
            }
            for (auto& [path, snap] : snapshots)
                if (snap.recording) snap.digests.push_back(digest_of(host->machine()));
            while (next < scenario.checks.size() && script >= scenario.checks[next].at)
                evaluate(opt, scenario, scenario.checks[next++], *host, total, meter, recorder.get(), disc_keys, hw.get());
            while (save_next < scenario.saves.size() && script >= scenario.saves[save_next].at) {
                const std::string name = scenario.saves[save_next++].path;
                const std::string path = resolve_state_path(opt, name);
                host->save_state([&, name, path](std::vector<uint8_t> file, const std::string& err) {
                    std::string werr;
                    if (!err.empty() || !dw3::savestate::write_file(path, file, werr)) {
                        fail_state("savestate " + path + ": " + (err.empty() ? werr : err));
                        return;
                    }
                    std::printf("  savestate @%lld -> %s (%zu bytes)\n", static_cast<long long>(timeline.script_frame()),
                                path.c_str(), file.size());
                    Snapshot snap{timeline, next, waits_seen, last_frame, {}, false};
                    for (const auto& l : scenario.loads)
                        if (l.path == name && l.replay) snap.recording = true;
                    snapshots.insert_or_assign(name, std::move(snap));
                });
            }
            while (load_next < scenario.loads.size() && script >= scenario.loads[load_next].at) {
                const sc::StateOp op = scenario.loads[load_next++];
                const std::string path = resolve_state_path(opt, op.path);
                std::vector<uint8_t> file;
                std::string err;
                if (!dw3::savestate::read_file(path, file, err)) {
                    fail_state("loadstate: " + err);
                    dw3::host::Host::stop();
                }
                host->load_state(std::move(file), [&, op, path](const std::string& e) {
                    if (!e.empty()) {
                        fail_state("loadstate " + path + ": " + e);
                        return;
                    }
                    int64_t continue_at = last_frame;
                    if (op.replay) {
                        Snapshot& snap = snapshots.at(op.path);
                        snap.recording = false;
                        timeline = snap.timeline;
                        next = snap.next_check;
                        waits_seen = snap.waits_seen;
                        continue_at = snap.last_frame;
                        comparing = &snap;
                        compare_index = 0;
                    }
                    base = continue_at - static_cast<int64_t>(host->machine().video().frame_count());
                    last_frame = continue_at;
                    std::printf("  loadstate @%lld <- %s%s\n", static_cast<long long>(op.at), path.c_str(),
                                op.replay ? " (replay)" : "");
                });
                if (state_failed) dw3::host::Host::stop();
            }
            if (state_failed) dw3::host::Host::stop();
            if (until < 0 && next >= scenario.checks.size() && script >= scenario.frames && !comparing &&
                save_next >= scenario.saves.size() && load_next >= scenario.loads.size())
                dw3::host::Host::stop();
        });
        if (wait_timeout || result.end != dw3::host::RunResult::End::Stopped) break;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("  audio: %llu frames generated, %llu XA sectors decoded, ring overruns %llu, XA fifo %zu queued / %llu dropped\n",
                static_cast<unsigned long long>(host->machine().spu().frames_generated()),
                static_cast<unsigned long long>(host->machine().xa().sectors()),
                static_cast<unsigned long long>(host->machine().spu().audio().overruns()),
                host->machine().xa().queued(), static_cast<unsigned long long>(host->machine().xa().fifo().overruns()));
    if (!opt.wav.empty()) {
        if (sc::write_wav(opt.wav, meter.all())) std::printf("  audio %s (%zu frames)\n", opt.wav.c_str(), meter.all().size() / 2);
        else std::printf("  cannot write %s\n", opt.wav.c_str());
    }
    bool broken = false;
    if (result.end == dw3::host::RunResult::End::Fatal) {
        std::printf("  FAIL runtime fatal at frame %lld: %s\n", static_cast<long long>(last_frame),
                    result.fatal_message.c_str());
        broken = true;
    } else if (result.end == dw3::host::RunResult::End::Returned) {
        std::printf("  FAIL game entry returned at frame %lld\n", static_cast<long long>(last_frame));
        broken = true;
    }
    if (state_failed) {
        std::printf("  FAIL %s\n", state_error.c_str());
        broken = true;
    }
    if (wait_timeout) {
        const auto* w = timeline.failed_wait();
        if (timeline.guard_failed())
            std::printf("  FAIL wait @%lld \"%s\": guard condition broke (real frame %lld)\n",
                        static_cast<long long>(w->at), w->label.c_str(), static_cast<long long>(last_frame));
        else
            std::printf("  FAIL wait @%lld \"%s\" timed out after %lld frames (real frame %lld)\n", static_cast<long long>(w->at),
                        w->label.c_str(), static_cast<long long>(w->timeout), static_cast<long long>(last_frame));
        for (const auto& r : w->ram) {
            uint32_t v = 0;
            if (sc::read_ram(read_ram, r, v)) std::printf("    ram[%s/%d] actual %s\n", hex32(r.addr).c_str(), r.size, hex32(v).c_str());
        }
        for (const auto& r : w->guard) {
            uint32_t v = 0;
            if (sc::read_ram(read_ram, r, v)) std::printf("    guard[%s/%d] actual %s\n", hex32(r.addr).c_str(), r.size, hex32(v).c_str());
        }
        if (w->steer) {
            uint32_t x = 0, z = 0;
            if (sc::read_ram(read_ram, w->steer->x, x) && sc::read_ram(read_ram, w->steer->z, z))
                std::printf("    position (%d, %d), target (%d, %d), tolerance %d\n", sc::sign_extend(x, w->steer->x.size),
                            sc::sign_extend(z, w->steer->z.size), w->steer->target_x, w->steer->target_z, w->steer->tolerance);
        }
        std::vector<uint32_t> px;
        int pw = 0, ph = 0;
        if (host->machine().gpu().snapshot_display(px, pw, ph)) {
            auto path = std::filesystem::path(opt.out) / scenario.name / ("timeout_" + slug(w->label) + ".bmp");
            if (write_bmp(path, px, pw, ph)) std::printf("    screenshot %s\n", path.string().c_str());
        }
        broken = true;
    } else if (next < scenario.checks.size()) {
        std::printf("  FAIL %zu check(s) never reached\n", scenario.checks.size() - next);
        broken = true;
    }
    for (std::size_t i = 0; i < scenario.margin_watches.size(); ++i) {
        const auto& w = scenario.margin_watches[i];
        const MarginTrace& t = margin_traces[i];
        const bool pass = t.frames > 0 && t.changes <= w.lit_changes_max;
        std::printf("  %s margin_watch \"%s\" [%lld, %lld]: %d frames, margins switched %d times, allowed <= %d\n", pass ? "PASS" : "FAIL",
                    w.label.c_str(), static_cast<long long>(w.from), static_cast<long long>(w.to), t.frames, t.changes, w.lit_changes_max);
        ++(pass ? total.pass : total.fail);
        if (!t.kept.empty()) {
            // Contact sheet: consecutive frames around the first switch at 1/8 size, eight to a row, a frame whose margins are lit marked by a red bar on top.
            constexpr int kTileW = 160, kTileH = 90, kCols = 8, kStep = 8;
            const std::size_t count = std::min<std::size_t>(t.kept.size(), 48);
            const int rows = static_cast<int>((count + kCols - 1) / kCols);
            std::vector<uint32_t> sheet(static_cast<std::size_t>(kCols * kTileW) * static_cast<std::size_t>(rows * kTileH), 0);
            for (std::size_t n = 0; n < count; ++n) {
                const std::size_t index = n;
                const int ox = static_cast<int>(n % kCols) * kTileW, oy = static_cast<int>(n / kCols) * kTileH;
                for (int y = 0; y < kTileH; ++y)
                    for (int x = 0; x < kTileW; ++x)
                        sheet[static_cast<std::size_t>(oy + y) * static_cast<std::size_t>(kCols * kTileW) + static_cast<std::size_t>(ox + x)] =
                            y < 3 && t.kept_lit[index] ? 0xFF0000FFu : t.kept[index][static_cast<std::size_t>(y * kStep) * 1280 + static_cast<std::size_t>(x * kStep)];
            }
            auto path = std::filesystem::path(opt.out) / scenario.name / ("margin_watch_" + slug(w.label) + ".bmp");
            if (write_bmp(path, sheet, kCols * kTileW, rows * kTileH)) std::printf("    strip %s\n", path.string().c_str());
        }
    }
    for (std::size_t i = 0; i < scenario.flicker_watches.size(); ++i) {
        const auto& w = scenario.flicker_watches[i];
        const FlickerTrace& t = flicker_traces[i];
        const bool pass = t.pictures >= 3 && t.worst_blocks <= w.flicker_blocks_max;
        std::printf("  %s flicker_watch \"%s\" [%lld, %lld]: %d frames, %d different pictures, most flickering blocks %d, allowed <= %d\n",
                    pass ? "PASS" : "FAIL", w.label.c_str(), static_cast<long long>(w.from), static_cast<long long>(w.to), t.frames,
                    t.pictures, t.worst_blocks, w.flicker_blocks_max);
        ++(pass ? total.pass : total.fail);
    }
    bool ok = !broken && total.fail == 0;
    if (opt.frame_stats) std::fputs(frame_stats.report().c_str(), stdout);
    if (opt.frame_stats && hw)
        std::printf("  frame-stats replacement images loaded %llu, %.1f ms total on the render thread, GPU waits %.1f ms, submits %.1f ms\n",
                    static_cast<unsigned long long>(hw->stats().replacement_loads), static_cast<double>(hw->stats().replacement_ns) / 1e6,
                    static_cast<double>(hw->stats().gpu_wait_ns) / 1e6, static_cast<double>(hw->stats().submit_ns) / 1e6);
    std::printf("%s %s: %d passed, %d failed, %lld frames in %.1fs\n", ok ? "PASS" : "FAIL", scenario.name.c_str(),
                total.pass, total.fail, static_cast<long long>(last_frame), secs);
    return ok ? 0 : 1;
}
