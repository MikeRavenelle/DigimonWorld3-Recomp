// DW3Recomp entry point: boots the recompiled game on the native kernel.
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
//   --window-size WxH  initial window size in pixels (default 1280x960)
//   --fullscreen       start in (borderless, desktop) fullscreen; F11 or Alt+Enter toggles it
//   --resize-at F WxH  resize the window at frame F (repeatable; for checking swap chain resizing in audit runs)
//   --fullscreen-at F  toggle fullscreen at frame F (repeatable)
//   --scale M          fit (default, 4:3 letterbox), integer or stretch
//   --filter F         nearest (default) or linear
//   --no-vsync         present without waiting for the display refresh (always on with --uncapped/--frames)
//   --mute             no audio output (the SPU still runs; audio is also off for --frames audit runs)
//   --audio            with --frames: play audio and pace to real time (logs buffer underruns and drops at exit)
//   --screenshot-every N DIR   save the window image every N frames
//   --vram-dump PATH   save the whole VRAM as a BMP on exit
//   --trace-io LO:HI   log bus accesses to the hex physical range LO..HI
//   --load-state PATH  start from a save state file instead of booting (it must come from the same game; a load
//                      warns when an inserted memory card differs from the one the state was taken with)
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
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "audio_out.h"
#include "dw3/host.h"
#include "dw3/platform.h"
#include "present/native_window.h"
#include "present/presenter.h"

namespace {

struct Options {
    std::string disc = "game/Digimon World 3 (USA).cue";
    int max_frames = -1;
    std::string screenshot;
    bool trace_bios = false;
    std::string memcard[2];
    bool no_memcard = false;
    std::string vram_dump;
    uint32_t trace_lo = 0, trace_hi = 0;
    bool uncapped = false;
    bool mute = false;
    bool audio = false;
    int window_w = 1280, window_h = 960;
    bool fullscreen = false;
    struct WindowAction {
        int64_t frame;
        int w, h;  // 0x0: toggle fullscreen
    };
    std::vector<WindowAction> window_actions;
    bool intro = false;
    int shot_every = 0;
    std::string shot_dir;
    dw3app::RendererChoice renderer = dw3app::RendererChoice::Auto;
    dw3app::PresentOptions present;
    bool no_vsync = false;
    std::string load_state;
    struct Press {
        uint16_t button;
        int64_t from, to;
    };
    std::vector<Press> presses;
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
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--disc" && i + 1 < argc) o.disc = argv[++i];
        else if (a == "--frames" && i + 1 < argc) o.max_frames = std::atoi(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
        else if (a == "--trace-bios") o.trace_bios = true;
        else if (a == "--memcard1" && i + 1 < argc) o.memcard[0] = argv[++i];
        else if (a == "--memcard2" && i + 1 < argc) o.memcard[1] = argv[++i];
        else if (a == "--no-memcard") o.no_memcard = true;
        else if (a == "--uncapped") o.uncapped = true;
        else if (a == "--mute") o.mute = true;
        else if (a == "--audio") o.audio = true;
        else if (a == "--window-size" && i + 1 < argc) {
            if (!parse_size(argv[++i], o.window_w, o.window_h) || o.window_w < 160 || o.window_h < 120) {
                std::fprintf(stderr, "bad --window-size %s (WxH)\n", argv[i]);
                std::exit(2);
            }
        }
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--resize-at" && i + 2 < argc) {
            Options::WindowAction act{std::atoll(argv[++i]), 0, 0};
            if (!parse_size(argv[++i], act.w, act.h) || act.w < 160 || act.h < 120) {
                std::fprintf(stderr, "bad --resize-at size %s (WxH)\n", argv[i]);
                std::exit(2);
            }
            o.window_actions.push_back(act);
        }
        else if (a == "--fullscreen-at" && i + 1 < argc) o.window_actions.push_back({std::atoll(argv[++i]), 0, 0});
        else if (a == "--intro") o.intro = true;
        else if (a == "--renderer" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "auto") o.renderer = dw3app::RendererChoice::Auto;
            else if (v == "gpu") o.renderer = dw3app::RendererChoice::Gpu;
            else if (v == "vulkan") o.renderer = dw3app::RendererChoice::Vulkan;
            else if (v == "d3d12") o.renderer = dw3app::RendererChoice::D3D12;
            else if (v == "metal") o.renderer = dw3app::RendererChoice::Metal;
            else if (v == "software") o.renderer = dw3app::RendererChoice::Software;
            else {
                std::fprintf(stderr, "bad --renderer %s\n", v.c_str());
                std::exit(2);
            }
        }
        else if (a == "--scale" && i + 1 < argc) {
            if (!dw3::parse_scale_mode(argv[++i], o.present.scale)) {
                std::fprintf(stderr, "bad --scale %s\n", argv[i]);
                std::exit(2);
            }
        }
        else if (a == "--filter" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v != "nearest" && v != "linear") {
                std::fprintf(stderr, "bad --filter %s\n", v.c_str());
                std::exit(2);
            }
            o.present.linear = v == "linear";
        }
        else if (a == "--internal-res" && i + 1 < argc) {
            o.present.internal_res = std::atoi(argv[++i]);
            if (o.present.internal_res < 1 || o.present.internal_res > 8) {
                std::fprintf(stderr, "bad --internal-res %s (1..8)\n", argv[i]);
                std::exit(2);
            }
        }
        else if (a == "--true-color") o.present.true_color = true;
        else if (a == "--precise-geometry") o.present.precise_geometry = true;
        else if (a == "--texture-filter" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "nearest") o.present.texture_filter = 0;
            else if (v == "bilinear") o.present.texture_filter = 1;
            else if (v == "xbr") o.present.texture_filter = 2;
            else {
                std::fprintf(stderr, "bad --texture-filter %s (nearest, bilinear or xbr)\n", v.c_str());
                std::exit(2);
            }
        }
        else if (a == "--no-vsync") o.no_vsync = true;
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
    SDL_Log("DW3Recomp starting (os=%s arch=%s)", dw3::platform::os_name(dw3::platform::host_os()).data(),
            dw3::platform::arch_name(dw3::platform::host_arch()).data());

#if !DW3_HAVE_GENERATED
    SDL_Log("no recompiled code linked: run `cmake --build <dir> --target recompile` and reconfigure");
    return 1;
#else
    dw3::host::Config cfg;
    cfg.disc_path = opt.disc;
    cfg.card_mode = opt.no_memcard ? dw3::host::CardMode::None : dw3::host::CardMode::Files;
    cfg.card_path = {opt.memcard[0], opt.memcard[1]};
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
    opt.present.vsync = !(opt.no_vsync || opt.uncapped || opt.max_frames >= 0);
    SDL_Window* window = nullptr;
    std::unique_ptr<dw3app::Presenter> presenter = dw3app::create_presenter(
        "Digimon World 3 Recomp", opt.window_w, opt.window_h, opt.renderer, opt.present, [](const std::string& s) { SDL_Log("%s", s.c_str()); },
        window);
    if (!presenter) {
        SDL_Log("no presentation path available");
        return 1;
    }
    SDL_Log("presenting with %s", presenter->name());
    if (opt.fullscreen) SDL_SetWindowFullscreen(window, true);
    presenter->attach(machine.gpu());
    if (opt.present.precise_geometry) {
        if (std::string(presenter->name()).find("software") != std::string::npos)
            SDL_Log("--precise-geometry needs the GPU renderer; ignored");
        else
            machine.enable_precise_geometry();
    }

    dw3app::AudioOutput audio;
    audio.open(opt.mute || opt.uncapped || (opt.max_frames >= 0 && !opt.audio));

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
    auto on_frame = [&](uint64_t frame) {
        host_pad.frame = static_cast<int64_t>(frame);
        for (const auto& act : opt.window_actions) {
            if (act.frame != static_cast<int64_t>(frame)) continue;
            if (act.w > 0) {
                SDL_SetWindowSize(window, act.w, act.h);
                SDL_Log("window resized to %dx%d", act.w, act.h);
            } else {
                toggle_fullscreen();
            }
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
                    case SDL_SCANCODE_F11: toggle_fullscreen(); break;
                    case SDL_SCANCODE_RETURN:
                        if (e.key.mod & SDL_KMOD_ALT) toggle_fullscreen();
                        break;
                    case SDL_SCANCODE_F8: state_slot = state_slot < 9 ? state_slot + 1 : 1; select_slot(); break;
                    default: break;
                }
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !host_pad.gamepad) host_pad.gamepad = SDL_OpenGamepad(e.gdevice.which);
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
        // Real-time pacing against emulated time (exact NTSC/PAL frame period from the video clock).
        if (!opt.uncapped && (opt.max_frames < 0 || opt.audio)) {
            static uint64_t start_ns = SDL_GetTicksNS();
            static int64_t start_cycles = machine.ctx().cycles;
            if (pacing_reset) {
                start_ns = SDL_GetTicksNS();
                start_cycles = machine.ctx().cycles;
                pacing_reset = false;
            }
            uint64_t target = start_ns + static_cast<uint64_t>(
                static_cast<double>(machine.ctx().cycles - start_cycles) * 1e9 / 33868800.0);
            uint64_t now = SDL_GetTicksNS();
            if (target > now) SDL_DelayPrecise(target - now);
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
