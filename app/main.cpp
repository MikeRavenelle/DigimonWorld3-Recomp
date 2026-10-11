// DW3Recomp entry point: boots the recompiled game on the native kernel.
//
// Options:
//   --disc PATH        disc image (.cue); default game/Digimon World 3 (USA).cue
//   --frames N         exit after N emulated frames (audit runs)
//   --screenshot PATH  save a BMP of the last frame before exiting
//   --trace-bios       log every kernel call

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "dw3/bios.h"
#include "dw3/machine.h"
#include "dw3/platform.h"
#include "psxdisc/disc.h"
#include "psxdisc/iso9660.h"

namespace {

struct Options {
    std::string disc = "game/Digimon World 3 (USA).cue";
    int max_frames = -1;
    std::string screenshot;
    bool trace_bios = false;
    std::string vram_dump;
    uint32_t trace_lo = 0, trace_hi = 0;
    bool uncapped = false;
    int shot_every = 0;
    std::string shot_dir;
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

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--disc" && i + 1 < argc) o.disc = argv[++i];
        else if (a == "--frames" && i + 1 < argc) o.max_frames = std::atoi(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
        else if (a == "--trace-bios") o.trace_bios = true;
        else if (a == "--uncapped") o.uncapped = true;
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

// Feeds raw disc sectors to the emulated CD-ROM drive.
class DiscSource final : public dw3::SectorSource {
public:
    explicit DiscSource(psxdisc::Disc& d) : disc_(d) {}
    uint32_t sector_count() const override { return disc_.sector_count(); }
    bool read_raw(uint32_t lba, uint8_t out[2352]) override {
        return disc_.read_raw(lba, std::span<uint8_t, psxdisc::kRawSectorSize>(out, psxdisc::kRawSectorSize));
    }

private:
    psxdisc::Disc& disc_;
};

std::vector<uint8_t> read_boot_exe(psxdisc::Disc& disc) {
    auto root = psxdisc::read_root(disc);
    if (!root) return {};
    for (const auto& f : psxdisc::walk(disc, root->lba, root->size))
        if (f.path == "SLUS_014.36") return disc.read_file(f.lba, f.size);
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parse(argc, argv);
    SDL_Log("DW3Recomp starting (os=%s arch=%s)", dw3::platform::os_name(dw3::platform::host_os()).data(),
            dw3::platform::arch_name(dw3::platform::host_arch()).data());

#if !DW3_HAVE_GENERATED
    SDL_Log("no recompiled code linked: run `cmake --build <dir> --target recompile` and reconfigure");
    return 1;
#else
    auto disc = psxdisc::Disc::open(opt.disc);
    if (!disc) {
        SDL_Log("cannot open disc image %s", opt.disc.c_str());
        return 1;
    }
    auto exe = read_boot_exe(*disc);
    if (exe.empty()) {
        SDL_Log("SLUS_014.36 not found on disc");
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    // Temporary presentation path until the plume renderer lands.
    if (!SDL_CreateWindowAndRenderer("Digimon World 3 Recomp", 1280, 960, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("window creation failed: %s", SDL_GetError());
        return 1;
    }

    static dw3::Machine machine({dw3_segments, dw3_segment_count});
    static dw3::Bios bios(machine);
    static DiscSource disc_source(*disc);
    machine.cdrom().set_disc(&disc_source);
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
    if (opt.trace_bios) bios.log = [](const std::string& s) { SDL_Log("%s", s.c_str()); };
    bios.on_tty = [](const std::string& s) { SDL_Log("[game] %s", s.c_str()); };
    if (opt.trace_hi) {
        machine.bus().trace_lo = opt.trace_lo;
        machine.bus().trace_hi = opt.trace_hi;
        machine.bus().on_trace = [](uint32_t a, int size, bool write, uint32_t v) {
            SDL_Log("io %s%d %08X %s %08X @%lld", write ? "W" : "R", size * 8, a, write ? "<-" : "->", v,
                    static_cast<long long>(machine.ctx().cycles));
        };
    }
    static std::map<uint32_t, uint64_t> unmapped;  // physical address -> access count
    machine.bus().on_unmapped = [](uint32_t addr, int size, bool write, uint32_t value) {
        if (unmapped[addr]++ == 0)
            SDL_Log("unmapped %s%d %08X%s", write ? "W" : "R", size * 8, addr,
                    write ? (" = " + std::to_string(value)).c_str() : "");
    };

    auto shutdown = [&](int code) {
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
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::exit(code);
    };
    dw3::set_fatal_handler([&](const std::string& msg) {
        SDL_Log("FATAL: %s", msg.c_str());
        shutdown(2);
    });

    // The game owns the main thread; each emulated VBLANK pumps the window and presents.
    SDL_Texture* screen = nullptr;
    int screen_w = 0, screen_h = 0;
    std::vector<uint32_t> pixels;
    machine.video().on_frame = [&](uint64_t frame) {
        host_pad.frame = static_cast<int64_t>(frame);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) shutdown(0);
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !host_pad.gamepad) host_pad.gamepad = SDL_OpenGamepad(e.gdevice.which);
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        // Present the GPU's display area (software path until the plume renderer lands).
        int w = 0, h = 0;
        if (machine.gpu().snapshot_display(pixels, w, h)) {
            if (!screen || w != screen_w || h != screen_h) {
                if (screen) SDL_DestroyTexture(screen);
                screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                SDL_SetTextureScaleMode(screen, SDL_SCALEMODE_NEAREST);
                screen_w = w;
                screen_h = h;
            }
            SDL_UpdateTexture(screen, nullptr, pixels.data(), w * 4);
            SDL_RenderTexture(renderer, screen, nullptr, nullptr);
        }
        if (opt.shot_every > 0 && frame % static_cast<uint64_t>(opt.shot_every) == 0) {
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr)) {
                char name[64];
                std::snprintf(name, sizeof(name), "/frame_%06llu.bmp", static_cast<unsigned long long>(frame));
                SDL_SaveBMP(s, (opt.shot_dir + name).c_str());
                SDL_DestroySurface(s);
            }
        }
        if (opt.max_frames >= 0 && static_cast<int64_t>(frame) >= opt.max_frames) {
            if (!opt.screenshot.empty()) {
                if (SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr)) {
                    SDL_SaveBMP(s, opt.screenshot.c_str());
                    SDL_DestroySurface(s);
                    SDL_Log("saved screenshot %s", opt.screenshot.c_str());
                }
            }
            shutdown(0);
        }
        SDL_RenderPresent(renderer);
        // Real-time pacing against emulated time (exact NTSC/PAL frame period from the video clock).
        if (!opt.uncapped && opt.max_frames < 0) {
            static const uint64_t start_ns = SDL_GetTicksNS();
            static const int64_t start_cycles = machine.ctx().cycles;
            uint64_t target = start_ns + static_cast<uint64_t>(
                static_cast<double>(machine.ctx().cycles - start_cycles) * 1e9 / 33868800.0);
            uint64_t now = SDL_GetTicksNS();
            if (target > now) SDL_DelayPrecise(target - now);
        }
    };

    uint32_t entry = bios.load_exe(exe);
    SDL_Log("booting SLUS_014.36 at %08X", entry);
    bios.run(entry);
    SDL_Log("game entry returned");
    shutdown(0);
#endif
}
