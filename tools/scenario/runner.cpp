// dw3scenario: headless in-game scenario runner.
//
//   dw3scenario <file.toml> [--disc PATH] [--record] [--out DIR] [--wav PATH]
//
// Boots the recompiled game with the scenario's scripted pad on port 1 (no window, no SDL, uncapped), evaluates
// every check at its frame and writes BMPs of the display to <out>/<name>/<at>_<label>.bmp for checks with
// `screenshot = true` and for failed checks. --record prints the actual screen hash of every check as a
// ready-to-paste `screen_hash = "..."` line and does not fail on hash mismatches. The SPU runs normally (no audio
// device); its output is metered per frame for `audio_*` checks. --wav PATH saves the whole mixed run as a WAV.
//
// Exit codes: 0 all checks pass, 1 failure, 2 usage/scenario error, 77 disc missing (ctest SKIP).

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "dw3/host.h"
#include "dw3/recomp.h"
#include "scenario.h"

namespace {

namespace sc = dw3::scenario;

struct Options {
    std::string file;
    std::string disc = std::string(DW3_SOURCE_DIR) + "/game/Digimon World 3 (USA).cue";
    std::string out = std::string(DW3_SOURCE_DIR) + "/game/audit/scenarios";
    bool record = false;
    std::string wav;  // --wav: whole-run audio dump
    bool no_xa = false;  // --no-xa: drop XA-ADPCM sectors (isolates SPU voice output when auditing)
};

// Port 1 pad driven by the scenario's input script.
class ScriptPad final : public dw3::PadSource {
public:
    explicit ScriptPad(const sc::Scenario& s) : scenario_(s) {}
    int64_t frame = 0;
    bool connected(int port) override { return port == 0; }
    uint16_t buttons(int port) override { return port == 0 ? sc::buttons_at(scenario_, frame) : 0; }

private:
    const sc::Scenario& scenario_;
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

struct Results {
    int pass = 0, fail = 0;
};

void report(Results& r, bool ok, const std::string& field, const std::string& detail) {
    std::printf("    %s %s: %s\n", ok ? "PASS" : "FAIL", field.c_str(), detail.c_str());
    (ok ? r.pass : r.fail)++;
}

// Evaluates one check against the live machine. Returns the number of failures it added.
int evaluate(const Options& opt, const sc::Scenario& s, const sc::Check& c, dw3::host::Host& host, Results& total,
             const sc::AudioMeter& meter) {
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
    if (c.tty_contains) {
        bool ok = host.tty().find(*c.tty_contains) != std::string::npos;
        report(r, ok, "tty_contains", std::string(ok ? "found" : "not found") + " \"" + *c.tty_contains + "\"");
    }
    for (const auto& ram : c.ram) {
        uint32_t v = 0;
        RecompContext* ctx = &m.ctx();
        switch (ram.size) {
            case 1: v = dw3_read8(ctx, ram.addr); break;
            case 2: v = dw3_read16(ctx, ram.addr); break;
            default: v = dw3_read32(ctx, ram.addr); break;
        }
        report(r, v == ram.value, "ram[" + hex32(ram.addr) + "/" + num(ram.size) + "]",
               "actual " + hex32(v) + ", expected " + hex32(ram.value));
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
        else if (a == "--out" && i + 1 < argc) opt.out = argv[++i];
        else if (a == "--record") opt.record = true;
        else if (a == "--wav" && i + 1 < argc) opt.wav = argv[++i];
        else if (a == "--no-xa") opt.no_xa = true;
        else if (!a.empty() && a[0] != '-' && opt.file.empty()) opt.file = a;
        else {
            std::fprintf(stderr, "usage: dw3scenario <file.toml> [--disc PATH] [--record] [--out DIR] [--wav PATH] [--no-xa]\n");
            return 2;
        }
    }
    if (opt.file.empty()) {
        std::fprintf(stderr, "usage: dw3scenario <file.toml> [--disc PATH] [--record] [--out DIR] [--wav PATH] [--no-xa]\n");
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

    dw3::host::Config cfg;
    cfg.disc_path = opt.disc;
    if (scenario.memcard == "none") cfg.card_mode = dw3::host::CardMode::None;
    else if (scenario.memcard == "blank") cfg.card_mode = dw3::host::CardMode::Blank;
    else {
        std::fprintf(stderr, "scenario error: unknown memcard \"%s\"\n", scenario.memcard.c_str());
        return 2;
    }
    cfg.log = [](const std::string& s) { std::fprintf(stderr, "%s\n", s.c_str()); };
    std::string error;
    std::unique_ptr<dw3::host::Host> host;
    ScriptPad pad(scenario);

    bool want_wav = !opt.wav.empty();
    for (const auto& c : scenario.checks) want_wav = want_wav || c.wav;
    sc::AudioMeter meter(want_wav);
    std::vector<int16_t> pcm_buf(4096);

    std::printf("scenario %s: %s\n", scenario.name.c_str(), scenario.description.c_str());
    Results total;
    std::size_t next = 0;
    int64_t last_frame = 0;
    auto t0 = std::chrono::steady_clock::now();
    dw3::host::RunResult result;
    // One boot per [[reboot]]: each gets a fresh machine; the memory card image carries over.
    for (std::size_t boot = 0; boot <= scenario.reboots.size(); ++boot) {
        if (host) cfg.blank_card = host->card_image(0);
        host.reset();
        host = dw3::host::Host::create(cfg, error);
        if (!host) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        if (opt.no_xa) host->machine().cdrom().set_xa_sink(nullptr);
        host->machine().sio().set_pad_source(0, &pad);
        const int64_t base = boot ? scenario.reboots[boot - 1] : 0;
        const int64_t until = boot < scenario.reboots.size() ? scenario.reboots[boot] : -1;
        if (boot) std::printf("  reboot @%lld\n", static_cast<long long>(base));
        result = host->run([&](uint64_t frame) {
            pad.frame = base + static_cast<int64_t>(frame);
            last_frame = pad.frame;
            for (;;) {  // everything the SPU produced during this video frame
                std::size_t n = host->machine().spu().drain_samples(pcm_buf);
                meter.add_frame(std::span<const int16_t>(pcm_buf.data(), n * 2));
                if (n < pcm_buf.size() / 2) break;
            }
            if (until >= 0 && pad.frame >= until) dw3::host::Host::stop();
            while (next < scenario.checks.size() && pad.frame >= scenario.checks[next].at)
                evaluate(opt, scenario, scenario.checks[next++], *host, total, meter);
            if (until < 0 && next >= scenario.checks.size() && pad.frame >= scenario.frames) dw3::host::Host::stop();
        });
        if (result.end != dw3::host::RunResult::End::Stopped) break;
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
    if (next < scenario.checks.size()) {
        std::printf("  FAIL %zu check(s) never reached\n", scenario.checks.size() - next);
        broken = true;
    }
    bool ok = !broken && total.fail == 0;
    std::printf("%s %s: %d passed, %d failed, %lld frames in %.1fs\n", ok ? "PASS" : "FAIL", scenario.name.c_str(),
                total.pass, total.fail, static_cast<long long>(last_frame), secs);
    return ok ? 0 : 1;
}
