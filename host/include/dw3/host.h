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
#include <span>
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
    std::vector<uint8_t> blank_card;  // Blank mode: start from this image (e.g. the previous boot's) instead of a fresh one
    // Index of the disc's content that save states use to refer to data read from it; built on the first save and
    // cached. Empty = <user data dir>/cache/disc.index.
    std::string disc_index_cache;
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
    const std::vector<uint8_t>& card_image(int slot) const;  // current contents (empty if no card)

    // Everything the game printed so far (one line per call, '\n'-terminated). Extra per-line callback:
    std::function<void(const std::string&)> on_tty;
    const std::string& tty() const { return tty_; }

    // Save states (file format and safe points: see state.h and machine.h). A state belongs to the game, not to a
    // build of the recompiler. It does not contain the memory card images, only a hash of each; loading with other
    // card contents logs a warning. Loading never writes the card files.
    //
    // save_state queues a capture for the next safe point while the game runs; `done` then receives the state file
    // (header included) or an error message.
    void save_state(std::function<void(std::vector<uint8_t> file, const std::string& error)> done);
    // Like save_state, but only the serialized machine state is taken at the safe point; the disc references,
    // compression and `done` run on a worker thread, so `done` must not touch game-thread state. Saves are
    // delivered in the order they were requested. A save that arrives before the disc index is ready waits for
    // it on the worker thread.
    void save_state_async(std::function<void(std::vector<uint8_t> file, const std::string& error)> done);
    // Starts the worker and has it load (or build) the disc index now rather than at the first save.
    void prepare_save_states();
    // Loads a state file. Before run() it is applied at once and the game then continues from it; while the game is
    // running it is applied at the next safe point, abandoning the current game position. `done` receives an empty
    // string on success. A file that fails the header checks is rejected immediately; one that turns out to be
    // inconsistent while it is being applied ends the run with a fatal error.
    void load_state(std::vector<uint8_t> file, std::function<void(const std::string& error)> done = nullptr);

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
    bool running_ = false;
    struct Worker;
    std::unique_ptr<Worker> worker_;
    void start_worker();
    std::string disc_index_cache_path() const;
    uint64_t identity() const;
    std::vector<uint8_t> capture_payload();
    std::vector<uint8_t> capture();
    void apply(std::span<const uint8_t> payload);
    std::vector<uint8_t> unpack(std::span<const uint8_t> file);
    std::vector<uint8_t> exe_;
    std::string tty_;
};

}  // namespace dw3::host
