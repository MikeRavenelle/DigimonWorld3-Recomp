// DW3Recomp entry point: boots the recompiled game on the native kernel.
//
// Settings: user-facing options live in settings.toml in the user data dir (see docs/SETTINGS.md); defaults are
// overridden by that file and the file by the flags below, which are never written back.
//   --config PATH     read (and create, if missing) this settings file instead of <user data dir>/settings.toml
//   --no-config       neither read nor write any settings file (defaults plus flags only)
//   --preset P         original (default), enhanced or remaster: a bundle of the graphics options below; flags that
//                      follow on the command line still override its members
//
// Options:
//   --disc PATH        disc image (.cue); default game/Digimon World 3 (USA).cue
//   --frames N         exit after N emulated frames (audit runs)
//   --screenshot PATH  save a BMP of the last frame before exiting
//   --trace-bios       log every kernel call
//   --memcard1 PATH    memory card for slot 1 (.mcd/.gme, created formatted if missing);
//                      default <user data dir>/memcards/slot1.mcd
//   --memcard2 PATH    memory card for slot 2; default <user data dir>/memcards/slot2.mcd, inserted only if it exists
//   --no-memcard       no memory cards inserted (games see "no card")
//   --press B@F[+N]    hold button B (UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE START SELECT L1 R1 L2 R2)
//                      from frame F for N frames (default 6)
//   --intro            with --frames, do not press START at frame 300 (by default audit runs skip the intro)
//   --uncapped         do not pace to real time (always on with --frames)
//   --renderer R       auto (default: the plume GPU renderer, falling back to software if it cannot start; on Windows
//                      Direct3D 12 first, then Vulkan; on macOS Metal), gpu (the same without the software fallback),
//                      vulkan, d3d12 or metal (that API only), or software (CPU rasterizer shown through an SDL renderer)
//   --internal-res N   GPU renderer internal resolution multiplier, 1 (default) to 8
//   --true-color       GPU renderer: no dithering, full 8-bit shading and blending on screen (game-visible VRAM unchanged)
//   --precise-geometry GPU renderer: sub-pixel 3D vertices and perspective-correct textures from the GTE's projections
//                      (best with --internal-res > 1; off by default)
//   --texture-filter F GPU renderer: nearest (default), bilinear (3D triangles) or xbr (sprites, needs --internal-res > 1)
//   --hd-backgrounds   GPU renderer: show the replacement backgrounds of the texture pack (--texture-pack NAME, a folder
//                      under <user data dir>/texture-packs)
//   --dump-textures DIR  GPU renderer: record the background images the game draws and write them to DIR on exit
//                      (input of tools/texpack, which builds a texture pack)
//   --window-size WxH  initial window size in pixels (default 1280x960)
//   --fullscreen       start in (borderless, desktop) fullscreen; F11 or Alt+Enter toggles it
//   --resize-at F WxH  resize the window at frame F (repeatable; for checking swap chain resizing in audit runs)
//   --fullscreen-at F  toggle fullscreen at frame F (repeatable)
//   --compare-at F     press the Original/enhanced comparison hotkey at frame F (repeatable; for audit runs)
//   --scale M          fit (default, 4:3 letterbox), integer or stretch
//   --filter F         nearest (default), linear or fsr
//   --no-vsync         present without waiting for the display refresh (always on with --uncapped/--frames)
//   --mute             no audio output (the SPU still runs; audio is also off for --frames audit runs)
//   --audio            with --frames: play audio and pace to real time (logs buffer underruns and drops at exit)
//   --screenshot-every N DIR   save the window image every N frames
//   --vram-dump PATH   save the whole VRAM as a BMP on exit
//   --trace-io LO:HI   log bus accesses to the hex physical range LO..HI
//   --load-state PATH  start from a save state file instead of booting (it must come from the same game; a load
//                      warns when an inserted memory card differs from the one the state was taken with)
//
// Hotkeys: F9 switches between the Original picture and the chosen enhanced preset (a quick comparison, not saved);
// Tab (held) or F10 (latched) fast-forwards at system.fast_forward_speed; on a gamepad, pressing the right stick
// fast-forwards while held and pressing the left stick latches it. F4 (the gamepad's Guide button) cycles
// gameplay.game_speed through 1x to 4x.
//
// Save states (keyboard): F5 saves and F7 loads the current slot, F6 / F8 select the previous / next slot (1 to 9).
// Slots are <user data dir>/states/slotN.state. A state is written or restored within a frame or two of the key
// press, at the next point where the recompiled game can be interrupted safely.

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <span>
#include <optional>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "audio_out.h"
#include "dw3/host.h"
#include "dw3/platform.h"
#include "ingame/field_menu.h"
#include "ingame/gameplay.h"
#include "present/native_window.h"
#include "settings/controller.h"
#include "settings/fastforward.h"
#include "present/post_options.h"
#include "present/presenter.h"
#include "present/texture_packs.h"

namespace {

struct Options {
    std::string disc = "game/Digimon World 3 (USA).cue";
    int max_frames = -1;
    std::string screenshot;
    bool trace_bios = false;
    std::string vram_dump;
    std::string dump_textures;
    uint32_t trace_lo = 0, trace_hi = 0;
    bool uncapped = false;
    bool audio = false;
    struct WindowAction {
        int64_t frame;
        int w, h;  // 0x0: toggle fullscreen
    };
    std::vector<WindowAction> window_actions;
    std::vector<int64_t> compare_frames;
    bool intro = false;
    int shot_every = 0;
    std::string shot_dir;
    std::string load_state;
    struct Press {
        uint16_t button;
        int64_t from, to;
    };
    std::vector<Press> presses;
    // Settings given as flags: (setting id, value text) in command-line order.
    std::vector<std::pair<std::string, std::string>> overrides;
    std::string config_path;
    bool no_config = false;
};

uint16_t button_from_name(const std::string& n) {
    static const std::map<std::string, uint16_t> names = {
        {"UP", dw3::kPadUp},         {"DOWN", dw3::kPadDown},     {"LEFT", dw3::kPadLeft},
        {"RIGHT", dw3::kPadRight},   {"CROSS", dw3::kPadCross},   {"CIRCLE", dw3::kPadCircle},
        {"SQUARE", dw3::kPadSquare}, {"TRIANGLE", dw3::kPadTriangle}, {"START", dw3::kPadStart},
        {"SELECT", dw3::kPadSelect}, {"L1", dw3::kPadL1},         {"R1", dw3::kPadR1},
        {"L2", dw3::kPadL2},         {"R2", dw3::kPadR2},
    };
    auto it = names.find(n);
    return it == names.end() ? 0 : it->second;
}

// Port 1 pad: keyboard + first SDL gamepad, plus scripted presses for audit runs.
class HostPad final : public dw3::PadSource {
public:
    const std::vector<Options::Press>* script = nullptr;
    int64_t frame = 0;
    SDL_Gamepad* gamepad = nullptr;

    bool connected(int port) override { return port == 0; }
    uint16_t buttons(int port) override {
        if (port != 0) return 0;
        uint16_t b = 0;
        const bool* k = SDL_GetKeyboardState(nullptr);
        auto key = [&](SDL_Scancode sc, uint16_t bit) {
            if (k[sc]) b |= bit;
        };
        key(SDL_SCANCODE_UP, dw3::kPadUp);
        key(SDL_SCANCODE_DOWN, dw3::kPadDown);
        key(SDL_SCANCODE_LEFT, dw3::kPadLeft);
        key(SDL_SCANCODE_RIGHT, dw3::kPadRight);
        key(SDL_SCANCODE_Z, dw3::kPadCross);
        key(SDL_SCANCODE_X, dw3::kPadCircle);
        key(SDL_SCANCODE_A, dw3::kPadSquare);
        key(SDL_SCANCODE_S, dw3::kPadTriangle);
        key(SDL_SCANCODE_RETURN, dw3::kPadStart);
        key(SDL_SCANCODE_BACKSPACE, dw3::kPadSelect);
        key(SDL_SCANCODE_Q, dw3::kPadL1);
        key(SDL_SCANCODE_W, dw3::kPadR1);
        key(SDL_SCANCODE_1, dw3::kPadL2);
        key(SDL_SCANCODE_2, dw3::kPadR2);
        if (gamepad) {
            auto pad = [&](SDL_GamepadButton gb, uint16_t bit) {
                if (SDL_GetGamepadButton(gamepad, gb)) b |= bit;
            };
            pad(SDL_GAMEPAD_BUTTON_DPAD_UP, dw3::kPadUp);
            pad(SDL_GAMEPAD_BUTTON_DPAD_DOWN, dw3::kPadDown);
            pad(SDL_GAMEPAD_BUTTON_DPAD_LEFT, dw3::kPadLeft);
            pad(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, dw3::kPadRight);
            pad(SDL_GAMEPAD_BUTTON_SOUTH, dw3::kPadCross);
            pad(SDL_GAMEPAD_BUTTON_EAST, dw3::kPadCircle);
            pad(SDL_GAMEPAD_BUTTON_WEST, dw3::kPadSquare);
            pad(SDL_GAMEPAD_BUTTON_NORTH, dw3::kPadTriangle);
            pad(SDL_GAMEPAD_BUTTON_START, dw3::kPadStart);
            pad(SDL_GAMEPAD_BUTTON_BACK, dw3::kPadSelect);
            pad(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, dw3::kPadL1);
            pad(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, dw3::kPadR1);
            if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= dw3::kPadL2;
            if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= dw3::kPadR2;
        }
        if (script)
            for (const auto& p : *script)
                if (frame >= p.from && frame < p.to) b |= p.button;
        return b;
    }
};

bool parse_size(const char* text, int& w, int& h) {
    char* end = nullptr;
    const long a = std::strtol(text, &end, 10);
    if (end == text || (*end != 'x' && *end != 'X')) return false;
    const char* rest = end + 1;
    const long b = std::strtol(rest, &end, 10);
    if (end == rest || *end != '\0') return false;
    w = static_cast<int>(a);
    h = static_cast<int>(b);
    return true;
}

Options parse(int argc, char** argv) {
    Options o;
    auto set = [&](const char* id, std::string value) { o.overrides.emplace_back(id, std::move(value)); };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--disc" && i + 1 < argc) o.disc = argv[++i];
        else if (a == "--frames" && i + 1 < argc) o.max_frames = std::atoi(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
        else if (a == "--trace-bios") o.trace_bios = true;
        else if (a == "--config" && i + 1 < argc) o.config_path = argv[++i];
        else if (a == "--no-config") o.no_config = true;
        else if (a == "--memcard1" && i + 1 < argc) set("system.memcard1", argv[++i]);
        else if (a == "--memcard2" && i + 1 < argc) set("system.memcard2", argv[++i]);
        else if (a == "--no-memcard") set("system.memcards", "false");
        else if (a == "--uncapped") o.uncapped = true;
        else if (a == "--mute") set("audio.mute", "true");
        else if (a == "--audio") o.audio = true;
        else if (a == "--window-size" && i + 1 < argc) {
            int w = 0, h = 0;
            if (!parse_size(argv[++i], w, h)) {
                std::fprintf(stderr, "bad --window-size %s (WxH)\n", argv[i]);
                std::exit(2);
            }
            set("graphics.window_width", std::to_string(w));
            set("graphics.window_height", std::to_string(h));
        }
        else if (a == "--fullscreen") set("graphics.fullscreen", "true");
        else if (a == "--resize-at" && i + 2 < argc) {
            Options::WindowAction act{std::atoll(argv[++i]), 0, 0};
            if (!parse_size(argv[++i], act.w, act.h) || act.w < 160 || act.h < 120) {
                std::fprintf(stderr, "bad --resize-at size %s (WxH)\n", argv[i]);
                std::exit(2);
            }
            o.window_actions.push_back(act);
        }
        else if (a == "--fullscreen-at" && i + 1 < argc) o.window_actions.push_back({std::atoll(argv[++i]), 0, 0});
        else if (a == "--compare-at" && i + 1 < argc) o.compare_frames.push_back(std::atoll(argv[++i]));
        else if (a == "--intro") o.intro = true;
        else if (a == "--renderer" && i + 1 < argc) set("graphics.renderer", argv[++i]);
        else if (a == "--preset" && i + 1 < argc) set("graphics.preset", argv[++i]);
        else if (a == "--scale" && i + 1 < argc) set("graphics.scale_mode", argv[++i]);
        else if (a == "--filter" && i + 1 < argc) set("graphics.output_filter", argv[++i]);
        else if (a == "--internal-res" && i + 1 < argc) set("graphics.internal_res", argv[++i]);
        else if (a == "--true-color") set("graphics.true_color", "true");
        else if (a == "--precise-geometry") set("graphics.precise_geometry", "true");
        else if (a == "--texture-filter" && i + 1 < argc) set("graphics.texture_filter", argv[++i]);
        else if (a == "--no-vsync") set("graphics.vsync", "false");
        else if (a == "--hd-backgrounds") set("graphics.hd_backgrounds", "true");
        else if (a == "--texture-pack" && i + 1 < argc) set("graphics.texture_pack", argv[++i]);
        else if (a == "--dump-textures" && i + 1 < argc) o.dump_textures = argv[++i];
        else if (a == "--fast-forward-speed" && i + 1 < argc) set("system.fast_forward_speed", argv[++i]);
        else if (a == "--encounters" && i + 1 < argc) set("gameplay.encounters", argv[++i]);
        else if (a == "--game-speed" && i + 1 < argc) set("gameplay.game_speed", argv[++i]);
        else if (a == "--god-mode") set("gameplay.god_mode", "true");
        else if (a == "--screenshot-every" && i + 2 < argc) {
            o.shot_every = std::atoi(argv[++i]);
            o.shot_dir = argv[++i];
        }
        else if (a == "--press" && i + 1 < argc) {
            std::string spec = argv[++i];
            auto at = spec.find('@'), plus = spec.find('+');
            uint16_t bit = button_from_name(spec.substr(0, at));
            if (!bit || at == std::string::npos) {
                std::fprintf(stderr, "bad --press %s\n", spec.c_str());
                std::exit(2);
            }
            int64_t from = std::stoll(spec.substr(at + 1, plus == std::string::npos ? std::string::npos : plus - at - 1));
            int64_t len = plus == std::string::npos ? 6 : std::stoll(spec.substr(plus + 1));
            o.presses.push_back({bit, from, from + len});
        }
        else if (a == "--load-state" && i + 1 < argc) o.load_state = argv[++i];
        else if (a == "--vram-dump" && i + 1 < argc) o.vram_dump = argv[++i];
        else if (a == "--trace-io" && i + 1 < argc) {
            std::string r = argv[++i];
            auto colon = r.find(':');
            o.trace_lo = static_cast<uint32_t>(std::stoul(r.substr(0, colon), nullptr, 16));
            o.trace_hi = static_cast<uint32_t>(std::stoul(r.substr(colon + 1), nullptr, 16));
        }
    }
    return o;
}

// Ctrl+C or SIGTERM asks the frame loop to stop; a second one exits at once, in case the loop is stuck (for
// example in a present the compositor never completes).
std::atomic<bool> g_quit_requested{false};

void on_quit_signal(int) {
    if (g_quit_requested.exchange(true)) std::_Exit(130);
}

}  // namespace

int main(int argc, char** argv) {
    // Arguments are UTF-8 on every platform (on Windows main() would otherwise get them in the ANSI code page).
    std::vector<std::string> args = dw3::platform::command_line_utf8(argc, argv);
    std::vector<char*> arg_ptrs;
    for (std::string& a : args) arg_ptrs.push_back(a.data());
    Options opt = parse(static_cast<int>(arg_ptrs.size()), arg_ptrs.data());
    // Settings: defaults, then the file, then flags. Audit runs and --no-config never touch the user's file.
    std::filesystem::path config_file;
    dw3app::Settings stored;
    if (!opt.no_config && (opt.config_path.empty() ? opt.max_frames < 0 : true)) {
        config_file = opt.config_path.empty() ? dw3app::default_settings_path() : dw3::platform::path_from_utf8(opt.config_path);
        dw3app::LoadReport report;
        stored = dw3app::load_settings_file(config_file, report);
        for (const std::string& m : report.messages) SDL_Log("settings: %s", m.c_str());
        if (!report.file_found) {
            std::string err;
            if (dw3app::save_settings_file(config_file, stored, err)) SDL_Log("created %s", dw3::platform::path_to_utf8(config_file).c_str());
            else SDL_Log("settings: %s", err.c_str());
        }
    }
    dw3app::Settings active = stored;
    {
        // The preset first, so that the other flags override its members whatever their order.
        auto apply_flag = [&](const std::pair<std::string, std::string>& f) {
            const dw3app::SettingInfo* info = dw3app::find_setting(f.first);
            std::string err;
            if (!info || !dw3app::set_text(active, *info, f.second, err)) {
                std::fprintf(stderr, "bad value for %s: %s\n", f.first.c_str(), err.c_str());
                std::exit(2);
            }
        };
        for (const auto& f : opt.overrides)
            if (f.first == "graphics.preset") apply_flag(f);
        for (const auto& f : opt.overrides)
            if (f.first != "graphics.preset") apply_flag(f);
    }
    SDL_Log("DW3Recomp starting (os=%s arch=%s)", dw3::platform::os_name(dw3::platform::host_os()).data(),
            dw3::platform::arch_name(dw3::platform::host_arch()).data());

#if !DW3_HAVE_GENERATED
    SDL_Log("no recompiled code linked: run `cmake --build <dir> --target recompile` and reconfigure");
    return 1;
#else
    dw3::host::Config cfg;
    cfg.disc_path = opt.disc;
    cfg.card_mode = active.system.memcards ? dw3::host::CardMode::Files : dw3::host::CardMode::None;
    cfg.card_path = {active.system.memcard1, active.system.memcard2};
    cfg.trace_bios = opt.trace_bios;
    cfg.trace_lo = opt.trace_lo;
    cfg.trace_hi = opt.trace_hi;
    cfg.log = [](const std::string& s) { SDL_Log("%s", s.c_str()); };
    std::string error;
    auto host = dw3::host::Host::create(cfg, error);
    if (!host) {
        SDL_Log("%s", error.c_str());
        return 1;
    }
    dw3::Machine& machine = host->machine();
    dw3::Bios& bios = host->bios();
    // Audit runs skip the intro FMV with START at frame 300 unless --intro is given.
    if (opt.max_frames >= 0 && !opt.intro) opt.presses.push_back({dw3::kPadStart, 300, 306});

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    std::signal(SIGINT, on_quit_signal);
    std::signal(SIGTERM, on_quit_signal);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const bool unpaced = opt.uncapped || opt.max_frames >= 0;
    dw3app::FastForwardInput fast_forward;
    // What the presenter should do for a configuration; vsync is off whenever the run is not paced to the display.
    dw3app::TexturePackCache texture_packs;
    auto present_options = [&](const dw3app::Settings& st) {
        dw3app::PresentOptions po;
        const auto& g = st.graphics;
        po.vsync = g.vsync && !unpaced && !fast_forward.active() && st.gameplay.game_speed == 0;
        po.linear = g.output_filter >= 1;
        po.post = dw3app::post_fx_from(g);
        po.internal_res = g.internal_res;
        po.true_color = g.true_color;
        po.texture_filter = g.texture_filter;
        po.precise_geometry = g.precise_geometry;
        po.texture_pack = texture_packs.get(g);
        const char* modes[] = {"fit", "integer", "stretch"};
        dw3::parse_scale_mode(modes[g.scale_mode], po.scale);
        return po;
    };
    SDL_Window* window = nullptr;
    std::unique_ptr<dw3app::Presenter> presenter = dw3app::create_presenter(
        "Digimon World 3 Recomp", active.graphics.window_width, active.graphics.window_height,
        static_cast<dw3app::RendererChoice>(active.graphics.renderer), present_options(active), [](const std::string& s) { SDL_Log("%s", s.c_str()); },
        window);
    if (!presenter) {
        SDL_Log("no presentation path available");
        return 1;
    }
    SDL_Log("presenting with %s", presenter->name());
    if (active.graphics.fullscreen) SDL_SetWindowFullscreen(window, true);
    presenter->attach(machine.gpu());
    const bool software_presenter = std::string(presenter->name()).find("software") != std::string::npos;
    if (software_presenter && (active.graphics.hd_backgrounds || !opt.dump_textures.empty()))
        SDL_Log("texture replacement and dumping need the GPU renderer; ignored");
    if (!opt.dump_textures.empty() && presenter->set_texture_dump(opt.dump_textures))
        SDL_Log("recording background images for %s", opt.dump_textures.c_str());
    bool precise_installed = false;
    // The GTE side of precise geometry is installed the first time it is wanted and then left in place; the renderer
    // decides per configuration whether to use it.
    auto ensure_precise = [&](const dw3app::Settings& st) {
        if (!st.graphics.precise_geometry || precise_installed) return;
        if (software_presenter) {
            SDL_Log("precise geometry needs the GPU renderer; ignored");
            return;
        }
        machine.enable_precise_geometry();
        precise_installed = true;
    };
    ensure_precise(active);

    dw3app::AudioOutput audio;
    audio.open(opt.uncapped || (opt.max_frames >= 0 && !opt.audio), active.audio.mute, active.audio.device,
               active.audio.volume);
    audio.set_speed(dw3app::game_speed_multiplier(active.gameplay.game_speed));

    // Live changes of settings: one function applies whatever changed.
    struct LiveSink final : dw3app::SettingsSink {
        std::function<bool(const dw3app::Settings&, std::span<const dw3app::SettingInfo* const>)> fn;
        bool apply(const dw3app::Settings& now, std::span<const dw3app::SettingInfo* const> changed) override { return fn(now, changed); }
    } sink;
    dw3app::SettingsController settings(stored, active, &sink, config_file);
    dw3app::ingame::FieldMenuSettings settings_screen(machine, settings);
    dw3app::ingame::GameplayHooks gameplay(machine, settings);
    sink.fn = [&](const dw3app::Settings& now, std::span<const dw3app::SettingInfo* const> changed) {
        bool ok = true, graphics = false, window_size = false;
        for (const dw3app::SettingInfo* info : changed) {
            const std::string_view id = info->id;
            if (id == "graphics.fullscreen") SDL_SetWindowFullscreen(window, now.graphics.fullscreen);
            else if (id == "graphics.window_width" || id == "graphics.window_height") window_size = true;
            else if (id == "gameplay.game_speed") {
                audio.set_speed(dw3app::game_speed_multiplier(now.gameplay.game_speed));
                graphics = true;  // vsync is off while the game runs faster than the display
            }
            else if (id == "audio.mute") audio.set_muted(now.audio.mute);
            else if (id == "audio.volume") audio.set_volume(now.audio.volume);
            else if (id == "audio.device") audio.set_device(now.audio.device);
            else if (id.starts_with("graphics.")) graphics = true;  // presentation, filters and the presets built from them
        }
        if (window_size && !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN))
            SDL_SetWindowSize(window, now.graphics.window_width, now.graphics.window_height);
        if (graphics) {
            ensure_precise(now);
            ok = presenter->update_options(present_options(now), machine.gpu());
        }
        return ok;
    };

    // Reads a state file; on failure `why` gets a short reason for the on-screen message.
    auto read_state_file = [](const std::string& path, std::vector<uint8_t>& data, std::string* why = nullptr) {
        std::string err;
        if (dw3::savestate::read_file(path, data, err)) return true;
        SDL_Log("%s", err.c_str());
        if (why) *why = err;
        return false;
    };
    bool pacing_reset = false;  // a state was loaded: emulated time jumped, so restart the real-time pacing
    if (!opt.load_state.empty()) {
        std::vector<uint8_t> data;
        if (!read_state_file(opt.load_state, data)) return 1;
        std::string load_error;
        host->load_state(std::move(data), [&](const std::string& e) { load_error = e; });
        if (!load_error.empty()) {
            SDL_Log("cannot load %s: %s", opt.load_state.c_str(), load_error.c_str());
            return 1;
        }
        SDL_Log("loaded state %s", opt.load_state.c_str());
        presenter->notify("Loaded " + dw3::platform::path_to_utf8(dw3::platform::path_from_utf8(opt.load_state).filename()));
    }

    static HostPad host_pad;
    host_pad.script = &opt.presses;
    {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids && n > 0) host_pad.gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    machine.sio().set_pad_source(0, &host_pad);
    std::size_t funcs = 0;
    for (uint32_t i = 0; i < dw3_segment_count; ++i) funcs += dw3_segments[i].func_count;
    SDL_Log("recompiled code: %u segments, %zu functions", dw3_segment_count, funcs);
    host->on_tty = [](const std::string& s) { SDL_Log("[game] %s", s.c_str()); };

    auto shutdown = [&](int code) -> int {
        if (!opt.dump_textures.empty()) {
            std::string dump_error;
            if (presenter->write_texture_dump(dump_error)) SDL_Log("wrote texture dump to %s", opt.dump_textures.c_str());
            else SDL_Log("texture dump failed: %s", dump_error.c_str());
        }
        if (!opt.vram_dump.empty()) {
            std::vector<uint32_t> v;
            machine.gpu().snapshot_vram(v);
            if (SDL_Surface* s = SDL_CreateSurfaceFrom(dw3::kVramWidth, dw3::kVramHeight, SDL_PIXELFORMAT_ABGR8888,
                                                       v.data(), dw3::kVramWidth * 4)) {
                SDL_SaveBMP(s, opt.vram_dump.c_str());
                SDL_DestroySurface(s);
                SDL_Log("saved VRAM dump %s", opt.vram_dump.c_str());
            }
        }
        SDL_Log("gpu: commands=%llu primitives=%llu display=%dx%d enabled=%d",
                static_cast<unsigned long long>(machine.gpu().commands()),
                static_cast<unsigned long long>(machine.gpu().primitives()), machine.gpu().display().width,
                machine.gpu().display().height, machine.gpu().display().enabled);
        SDL_Log("exit: frames=%llu cycles=%lld bios_calls=%llu irqs=%llu lookups=%llu",
                static_cast<unsigned long long>(machine.video().frame_count()),
                static_cast<long long>(machine.ctx().cycles), static_cast<unsigned long long>(bios.calls()),
                static_cast<unsigned long long>(bios.interrupts_delivered()),
                static_cast<unsigned long long>(machine.lookups()));
        audio.close();
        machine.gpu().set_rasterizer(nullptr);
        presenter.reset();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return code;
    };
    // The game owns the main thread; each emulated VBLANK pumps the window and presents.
    dw3app::Capture capture;
    auto save_capture = [&](const std::string& path) {
        if (capture.rgba.empty()) return false;
        SDL_Surface* s = SDL_CreateSurfaceFrom(capture.w, capture.h, SDL_PIXELFORMAT_ABGR8888, capture.rgba.data(),
                                               capture.w * 4);
        if (!s) return false;
        bool ok = SDL_SaveBMP(s, path.c_str());
        SDL_DestroySurface(s);
        return ok;
    };
    int state_slot = 1;
    const std::filesystem::path state_dir = dw3::platform::user_data_dir() / "states";
    auto slot_path = [&] { return dw3::platform::path_to_utf8(state_dir / ("slot" + std::to_string(state_slot) + ".state")); };
    // Saves are encoded and written on the host's worker thread; it hands the outcome back here and the frame
    // loop shows it. Declared static so it outlives the host, whose destructor finishes the queued saves.
    struct SaveResults {
        std::mutex mutex;
        std::vector<std::string> messages;
        std::array<int, 10> in_flight{};  // saves not yet on disk, per slot
    };
    static SaveResults saves;
    auto save_slot = [&] {
        const std::string path = slot_path();
        const int slot = state_slot;
        {
            std::lock_guard lock(saves.mutex);
            ++saves.in_flight[static_cast<std::size_t>(slot)];
        }
        host->save_state_async([path, slot](std::vector<uint8_t> file, const std::string& err) {
            std::string werr, message;
            if (!err.empty()) {
                SDL_Log("cannot save state: %s", err.c_str());
                message = "Save failed: " + err;
            } else if (!dw3::savestate::write_file(path, file, werr)) {
                SDL_Log("cannot save state: %s", werr.c_str());
                message = "Save failed: " + werr;
            } else {
                SDL_Log("saved state %s", path.c_str());
                message = "Saved slot " + std::to_string(slot);
            }
            std::lock_guard lock(saves.mutex);
            --saves.in_flight[static_cast<std::size_t>(slot)];
            saves.messages.push_back(std::move(message));
        });
    };
    auto select_slot = [&] {
        SDL_Log("state slot %d", state_slot);
        presenter->notify("Slot " + std::to_string(state_slot) +
                          (std::filesystem::exists(dw3::platform::path_from_utf8(slot_path())) ? "" : " (empty)"));
    };
    auto load_slot = [&] {
        const std::string path = slot_path();
        const int slot = state_slot;
        {
            std::lock_guard lock(saves.mutex);
            if (saves.in_flight[static_cast<std::size_t>(slot)] > 0) {
                presenter->notify("Slot " + std::to_string(slot) + " is still saving");
                return;
            }
        }
        if (!std::filesystem::exists(dw3::platform::path_from_utf8(path))) {
            SDL_Log("slot %d is empty", slot);
            presenter->notify("Slot " + std::to_string(slot) + " is empty");
            return;
        }
        std::vector<uint8_t> data;
        std::string why;
        if (!read_state_file(path, data, &why)) {
            presenter->notify("Load failed: " + why);
            return;
        }
        host->load_state(std::move(data), [&, path, slot](const std::string& e) {
            if (e.empty()) {
                pacing_reset = true;
                SDL_Log("loaded state %s", path.c_str());
                presenter->notify("Loaded slot " + std::to_string(slot));
            } else {
                SDL_Log("cannot load %s: %s", path.c_str(), e.c_str());
                presenter->notify("Load failed: " + e);
            }
        });
    };
    auto toggle_fullscreen = [&] {
        const bool now = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_SetWindowFullscreen(window, !now);
        SDL_Log("fullscreen %s", now ? "off" : "on");
    };
    auto toggle_fast_forward_latch = [&] {
        fast_forward.toggle_latch();
        const double mult = dw3app::fast_forward_multiplier(settings.active().system.fast_forward_speed);
        presenter->notify(fast_forward.latched()
                              ? (mult > 0 ? "Fast-forward on (" + std::to_string(static_cast<int>(mult)) + "x)" : std::string("Fast-forward on (unlimited)"))
                              : std::string("Fast-forward off"));
    };
    auto cycle_game_speed = [&] {
        const int next = (settings.active().gameplay.game_speed + 1) % 4;
        const std::string value = std::to_string(next + 1) + "x";
        settings.set("gameplay.game_speed", value);
        presenter->notify("Game speed " + value);
    };
    dw3app::Pacer pacer;
    bool pacer_started = false, fast_forwarding = false;
    auto on_frame = [&](uint64_t frame) {
        host_pad.frame = static_cast<int64_t>(frame);
        settings_screen.apply_pending();
        for (const auto& act : opt.window_actions) {
            if (act.frame != static_cast<int64_t>(frame)) continue;
            if (act.w > 0) {
                SDL_SetWindowSize(window, act.w, act.h);
                SDL_Log("window resized to %dx%d", act.w, act.h);
            } else {
                toggle_fullscreen();
            }
        }
        for (const int64_t f : opt.compare_frames)
            if (f == static_cast<int64_t>(frame)) {
                const std::string mode = settings.toggle_comparison();
                SDL_Log("visual preset: %s", mode.c_str());
                presenter->notify("Visual preset: " + mode);
            }
        audio.pump(machine.spu());
        {
            std::lock_guard lock(saves.mutex);
            for (std::string& message : saves.messages) presenter->notify(std::move(message));
            saves.messages.clear();
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) dw3::host::Host::stop();
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                switch (e.key.scancode) {
                    case SDL_SCANCODE_F5: save_slot(); break;
                    case SDL_SCANCODE_F7: load_slot(); break;
                    case SDL_SCANCODE_F6: state_slot = state_slot > 1 ? state_slot - 1 : 9; select_slot(); break;
                    case SDL_SCANCODE_F9: {
                        const std::string mode = settings.toggle_comparison();
                        SDL_Log("visual preset: %s", mode.c_str());
                        presenter->notify("Visual preset: " + mode);
                        break;
                    }
                    case SDL_SCANCODE_F4: cycle_game_speed(); break;
                    case SDL_SCANCODE_F10: toggle_fast_forward_latch(); break;
                    case SDL_SCANCODE_F11: toggle_fullscreen(); break;
                    case SDL_SCANCODE_RETURN:
                        if (e.key.mod & SDL_KMOD_ALT) toggle_fullscreen();
                        break;
                    case SDL_SCANCODE_F8: state_slot = state_slot < 9 ? state_slot + 1 : 1; select_slot(); break;
                    default: break;
                }
            }
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK)
                toggle_fast_forward_latch();
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) cycle_game_speed();
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !host_pad.gamepad) host_pad.gamepad = SDL_OpenGamepad(e.gdevice.which);
        }
        {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            bool held = keys[SDL_SCANCODE_TAB];
            if (host_pad.gamepad && SDL_GetGamepadButton(host_pad.gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) held = true;
            fast_forward.set_held(held);
        }
        if (g_quit_requested) dw3::host::Host::stop();
        const bool last = opt.max_frames >= 0 && static_cast<int64_t>(frame) >= opt.max_frames;
        const bool periodic = opt.shot_every > 0 && frame % static_cast<uint64_t>(opt.shot_every) == 0;
        const bool want_shot = periodic || (last && !opt.screenshot.empty());
        presenter->present(machine.gpu(), want_shot ? &capture : nullptr);
        if (periodic) {
            char name[64];
            std::snprintf(name, sizeof(name), "/frame_%06llu.bmp", static_cast<unsigned long long>(frame));
            save_capture(opt.shot_dir + name);
        }
        if (last) {
            if (!opt.screenshot.empty() && save_capture(opt.screenshot)) SDL_Log("saved screenshot %s", opt.screenshot.c_str());
            dw3::host::Host::stop();
        }
        // Real-time pacing against emulated time (exact NTSC/PAL frame period from the video clock). Fast-forward
        // changes the speed; the sound is dropped while it lasts so it can neither crackle nor build up latency.
        const bool ff = fast_forward.active();
        if (ff != fast_forwarding) {
            fast_forwarding = ff;
            audio.set_discard(ff);
            presenter->update_options(present_options(settings.active()), machine.gpu());
        }
        if (!unpaced || opt.audio) {
            const uint64_t now = SDL_GetTicksNS();
            if (!pacer_started || pacing_reset) {
                pacer.reset(now, machine.ctx().cycles);
                pacer_started = true;
                pacing_reset = false;
            }
            pacer.set_speed(ff ? dw3app::fast_forward_multiplier(settings.active().system.fast_forward_speed)
                                : dw3app::game_speed_multiplier(settings.active().gameplay.game_speed),
                            now,
                            machine.ctx().cycles);
            if (opt.uncapped) return;
            const uint64_t wait = pacer.delay_ns(now, machine.ctx().cycles);
            if (wait) SDL_DelayPrecise(wait);
        }
    };

    host->prepare_save_states();
    SDL_Log("booting SLUS_014.36");
    auto result = host->run(on_frame);
    if (result.end == dw3::host::RunResult::End::Fatal) {
        SDL_Log("FATAL: %s", result.fatal_message.c_str());
        return shutdown(2);
    }
    if (result.end == dw3::host::RunResult::End::Returned) SDL_Log("game entry returned");
    return shutdown(0);
#endif
}
