#pragma once

// Native (HLE) PlayStation kernel: the A0/B0/C0 functions Digimon World 3 calls, syscalls, the
// exception/interrupt path and boot. No BIOS image is used.
//
// Interrupt delivery mirrors the real kernel: on a deliverable IRQ the registers are saved, the
// kernel's own work runs (pad polling, root-counter events), then the game's SysEnqIntRP chains,
// then the HookEntryInt "custom exit" (libetc's trapIntr) if one is set. That exit ends with
// ReturnFromException, which unwinds back here (C++ exception) and the saved registers are restored.

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "dw3/machine.h"

namespace dw3 {

// Thrown by B0:17 ReturnFromException; caught by the interrupt path. Generated code must be built
// with unwind tables (-fexceptions) so this can cross recompiled frames.
struct ReturnFromException {};

// Digital pad state as the kernel stores it (active-low button bits, psx-spx "Controller" layout).
struct PadState {
    bool connected = true;
    uint16_t buttons = 0xFFFF;  // all released
};

struct KernelEvent {
    uint32_t cls = 0, spec = 0, mode = 0, func = 0;
    uint32_t status = 0;  // 0 = free, else EvStWAIT/ACTIVE/ALREADY
};

class Bios {
public:
    static constexpr int kMaxEvents = 16;  // SYSTEM.CNF EVENT = 16
    static constexpr uint32_t kEvStWait = 0x1000, kEvStActive = 0x2000, kEvStAlready = 0x4000;
    static constexpr uint32_t kEvMdIntr = 0x1000, kEvMdNoIntr = 0x2000;

    explicit Bios(Machine& m);
    Bios(const Bios&) = delete;
    Bios& operator=(const Bios&) = delete;

    // Copies a PS-X EXE into RAM and prepares registers (pc is returned; sp from SYSTEM.CNF STACK).
    uint32_t load_exe(std::span<const uint8_t> psx_exe, uint32_t stack = 0x801FFF00);
    // Calls the entry point (crt0). Returns only if the game's entry returns.
    void run(uint32_t entry);

    // Kernel services (also reachable through dw3_bios_call).
    void call(RecompContext& ctx, uint32_t table, uint32_t fn);
    void syscall(RecompContext& ctx, uint32_t pc);
    void interrupt(RecompContext& ctx);
    void deliver_event(uint32_t cls, uint32_t spec);

    std::array<PadState, 2> pads;
    std::function<void(const std::string&)> log;     // optional trace of kernel calls
    std::function<void(const std::string&)> on_tty;  // game debug output (printf), one line per call

    // Introspection for tests and diagnostics.
    const KernelEvent& event(int i) const { return events_[static_cast<std::size_t>(i)]; }
    uint32_t hook_entry() const { return hook_; }
    uint64_t interrupts_delivered() const { return interrupts_; }
    uint64_t calls() const { return calls_; }

private:
    uint32_t& reg(RecompContext& c, int r) { return c.r[r]; }
    uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func);
    int event_index(uint32_t handle) const;
    void wait_until(RecompContext& ctx, const std::function<bool()>& done);
    void poll_pads();
    void run_int_chains(RecompContext& ctx);
    void card_async(uint32_t spec_sw, uint32_t spec_hw);
    [[noreturn]] void unknown(uint32_t table, uint32_t fn);

    Machine& m_;
    std::array<KernelEvent, kMaxEvents> events_{};
    std::array<uint32_t, 4> int_chain_{};  // SysEnqIntRP heads per priority (RAM addresses)
    uint32_t hook_ = 0;                    // HookEntryInt jmp_buf address (0 = default exit)
    uint32_t pad_buf_[2]{}, pad_size_[2]{};
    uint32_t outdated_pad_dest_ = 0;
    bool pad_started_ = false;
    bool clear_pad_ = true;
    std::array<bool, 4> clear_rcnt_{true, true, true, true};
    bool in_exception_ = false;
    std::string tty_;
    uint64_t interrupts_ = 0, calls_ = 0;
};

}  // namespace dw3
