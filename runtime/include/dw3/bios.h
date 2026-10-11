#pragma once

// Native (HLE) PlayStation kernel: the A0/B0/C0 functions Digimon World 3 calls, syscalls, the
// exception/interrupt path and boot. No BIOS image is used.
//
// Interrupt delivery mirrors the real kernel: on a deliverable IRQ the registers are saved, the
// kernel's own work runs (pad polling, root-counter events), then the game's SysEnqIntRP chains,
// then the HookEntryInt "custom exit" (libetc's trapIntr) if one is set. That exit ends with
// ReturnFromException, which unwinds back here (C++ exception) and the saved registers are restored.
//
// Memory cards: the card functions (A0:AB/AC, B0:4E/4F) and the "bu00:"/"bu10:" file functions operate
// directly on the images attached to Sio (see Sio::card_data), using dw3/memcard.h for the filesystem.
// Operations complete immediately; their completion events (SwCARD then HwCARD) are delivered later.
// Event/DIRENTRY details follow psx-spx "BIOS Memory Card Functions"/"BIOS File Functions", cross-checked
// against PCSX-Redux's HLE BIOS and OpenBIOS (GPL-2+/MIT); no code was copied.

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

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

    // Kernel device structures built in low RAM at construction (psx-spx "BIOS File Functions" / "Kernel
    // Variables": device table pointer/size at 0x150/0x154, FCB table at 0x140/0x144). Region 0x1000-0x1240
    // is otherwise unused by the HLE kernel (A0/B0/C0 tables are read at 0x674/0x874).
    static constexpr uint32_t kDevNames = 0x1000;   // "tty\0cdrom\0bu\0" strings and descriptions
    static constexpr uint32_t kDcbBase = 0x1040;    // DCB array, 0x50 bytes each: tty, cdrom, bu
    static constexpr uint32_t kDcbSize = 0x50;
    static constexpr uint32_t kDcbCount = 3;
    static constexpr uint32_t kFcbBase = 0x1140;    // FCB array, 0x2C bytes each
    static constexpr uint32_t kFcbSize = 0x2C;
    static constexpr uint32_t kFcbCount = 4;
    static constexpr uint32_t kNativeBase = 0x1200; // native handler entry points, 4 bytes apart
    enum NativeId : uint32_t { kNativeNop = 0, kNativeBuFirst = 1, kNativeBuNext = 2, kNativeCount = 3 };
    static constexpr uint32_t native_addr(NativeId id) { return kNativeBase + 4 * id; }

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
    std::function<void(int slot)> on_card_write;     // a BIOS call modified the card image in `slot` (0/1)

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
    // Memory card support (runtime/bios/bios_card.cpp).
    struct OpenFile {
        bool used = false;
        int slot = 0;
        std::string name;
        uint32_t pos = 0, mode = 0;
    };
    struct Search {
        bool active = false;
        int slot = 0, last_block = 0;
        std::string pattern;
        std::string device;  // DCB name used by firstfile ("bu")
    };
    void card_async(uint32_t spec, bool software = false);
    void card_modified(int slot);
    std::string read_cstr(RecompContext& c, uint32_t addr) const;
    bool card_dispatch(RecompContext& c, uint32_t table, uint32_t fn);  // false = not a card/file function
    uint32_t file_open(RecompContext& c, uint32_t path, uint32_t mode);
    uint32_t file_transfer(RecompContext& c, uint32_t fd, uint32_t buf, uint32_t len, bool write);
    uint32_t file_seek(uint32_t fd, int32_t off, uint32_t whence);
    // Kernel device table (runtime/bios/bios_devices.cpp).
    void init_kernel_devices();
    uint32_t find_dcb(const std::string& device);  // 0 if absent
    uint32_t file_first_via_dcb(RecompContext& c, uint32_t path);
    uint32_t file_next_via_dcb(RecompContext& c, uint32_t entry);
    uint32_t call_dcb_fn(RecompContext& c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);
    void native_device_call(RecompContext& c, uint32_t id);
    uint32_t bu_first(RecompContext& c, uint32_t fcb, uint32_t name, uint32_t entry);
    uint32_t bu_next(RecompContext& c, uint32_t fcb, uint32_t entry);
    uint32_t file_next(RecompContext& c, uint32_t entry);  // no completion event
    uint32_t file_misc(RecompContext& c, uint32_t fn, uint32_t a0, uint32_t a1);  // format / rename / erase
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
    std::array<OpenFile, 16> files_{};  // fds 0/1 are reserved for the console
    Search search_;
    uint32_t errno_ = 0;
    std::string tty_;
    uint64_t interrupts_ = 0, calls_ = 0;
};

}  // namespace dw3
