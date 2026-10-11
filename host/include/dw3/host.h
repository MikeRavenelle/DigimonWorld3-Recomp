#pragma once

// Reusable boot logic shared by the SDL app and the headless scenario runner: opens the disc, builds the
// Machine + native BIOS, wires the CD-ROM, memory cards and tty capture, and runs the game with a per-frame hook.
//
// The game never returns from Bios::run, so a run ends by throwing from the frame hook (Host::stop(), or any
// exception) or from a runtime fatal; Host::run() catches both and reports how it ended. One Host per process.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dw3/bios.h"
#include "dw3/machine.h"
#include "psxdisc/disc.h"

namespace dw3::host {

enum class CardMode {
    Files,  // .mcd-backed cards (slot 1 created if missing, slot 2 only if it exists); persisted on writes
    None,   // no cards inserted
    Blank,  // fresh formatted in-memory card in slot 1; never touches the disk
};

struct Config {
    std::string disc_path = "game/Digimon World 3 (USA).cue";
    CardMode card_mode = CardMode::Files;
    std::array<std::string, 2> card_path;  // Files mode overrides; empty = <user data dir>/memcards/slotN.mcd
    bool trace_bios = false;
    uint32_t trace_lo = 0, trace_hi = 0;  // bus I/O trace range (physical addresses), 0/0 = off
    std::function<void(const std::string&)> log;  // diagnostics (optional)
};

// Thrown by Host::stop() to unwind out of the game.
struct StopRun {};

struct RunResult {
    enum class End { Stopped, Fatal, Returned } end = End::Returned;
    std::string fatal_message;
};

class Host {
public:
    // Opens the disc and boot executable and builds the machine. On failure returns null and sets `error`.
    static std::unique_ptr<Host> create(const Config& cfg, std::string& error);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    Machine& machine() { return *machine_; }
    Bios& bios() { return *bios_; }
    int card_count() const;  // cards currently inserted

    // Everything the game printed so far (one line per call, '\n'-terminated). Extra per-line callback:
    std::function<void(const std::string&)> on_tty;
    const std::string& tty() const { return tty_; }

    // Boots the game; `frame_hook` runs at every emulated VBLANK. Returns once stopped, fatal or returned.
    // Flushes dirty memory cards before returning.
    RunResult run(std::function<void(uint64_t frame)> frame_hook);
    [[noreturn]] static void stop() { throw StopRun{}; }

private:
    Host();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::optional<psxdisc::Disc> disc_;
    std::unique_ptr<Machine> machine_;
    std::unique_ptr<Bios> bios_;
    std::vector<uint8_t> exe_;
    std::string tty_;
};

}  // namespace dw3::host
