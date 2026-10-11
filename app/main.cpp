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
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--disc" && i + 1 < argc) o.disc = argv[++i];
        else if (a == "--frames" && i + 1 < argc) o.max_frames = std::atoi(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
        else if (a == "--trace-bios") o.trace_bios = true;
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

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
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
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) shutdown(0);
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
    };

    uint32_t entry = bios.load_exe(exe);
    SDL_Log("booting SLUS_014.36 at %08X", entry);
    bios.run(entry);
    SDL_Log("game entry returned");
    shutdown(0);
#endif
}
