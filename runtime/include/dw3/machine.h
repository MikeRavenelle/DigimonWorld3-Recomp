#pragma once

// Ties the recompiled code to the emulated hardware: owns the bus, GTE and the function lookup
// that resolves indirect jumps against whichever overlays are resident.

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "dw3/bus.h"
#include "dw3/cdrom.h"
#include "dw3/dma.h"
#include "dw3/gpu.h"
#include "dw3/gte.h"
#include "dw3/irq.h"
#include "dw3/mdec.h"
#include "dw3/precise_vertex.h"
#include "dw3/recomp.h"
#include "dw3/scheduler.h"
#include "dw3/sio.h"
#include "dw3/soft_rasterizer.h"
#include "dw3/spu.h"
#include "dw3/state.h"
#include "dw3/timers.h"
#include "dw3/video_timing.h"
#include "dw3/xa.h"

namespace dw3 {

// Unrecoverable runtime error (e.g. jump to code that was never recompiled). The default handler
// logs and aborts; tests replace it to throw.
using FatalHandler = std::function<void(const std::string& message)>;
void set_fatal_handler(FatalHandler h);
[[noreturn]] void fatal(const std::string& message);

// Save states.
//
// A state is guest state only: RAM, registers, hardware blocks and one guest PC, the label the code was about to
// jump to at an event check. It does not depend on how the recompiler laid out the host code.
//
// The recompiled game keeps its call stack on the host stack, but the guest call chain is also in guest memory:
// every non-leaf function saved $ra in its frame, and a leaf keeps it in r31. So only the innermost function has
// to be resumed. Machine::resume enters it with RecompContext::resume set to the PC (each generated function
// starts with a dispatch from that label address to the label) and, when it returns, continues at the address its
// `jr $ra` jumped to by entering whichever function has a label there, and so on. Generated code provides the
// labels: every return point of a call and every backward-jump target (RecompSegment::resume). The emitter
// refuses delay slots that overwrite $ra on a return, so r31 still holds the return target once the function has
// returned. Frames the host stack had above the innermost function are never rebuilt; the guest returns into them
// through the dispatcher instead, which is equivalent because generated code keeps no state outside the context.
//
// A state can only be taken at a safe point: the event check of a backward jump (after due events and any
// interrupt were handled) while no native kernel frame is active (no interrupt handler, event callback or
// WaitEvent in progress; see Bios::host_depth). Machine::run_at_safe_point queues work for the next such moment;
// the game reaches one every frame or so, because it spins in recompiled VSync/DrawSync loops.
//
// Loading unwinds whatever host stack exists (the load throws StateRestored out of the safe point to the code that
// started the game, which then calls Machine::resume).
struct StateRestored {};

class Machine {
public:
    explicit Machine(std::span<const RecompSegment> segments);
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    Bus& bus() { return bus_; }
    Gte& gte() { return gte_; }
    Scheduler& scheduler() { return sched_; }
    IrqController& irq() { return irq_; }
    VideoTiming& video() { return video_; }
    Timers& timers() { return timers_; }
    Dma& dma() { return dma_; }
    Gpu& gpu() { return gpu_; }
    Cdrom& cdrom() { return cdrom_; }
    Spu& spu() { return spu_; }
    XaDecoder& xa() { return xa_; }
    Sio& sio() { return sio_; }
    Mdec& mdec() { return mdec_; }
    RecompContext& ctx() { return bus_.ctx(); }

    // Sub-pixel vertex tracking: the GTE records the untruncated position of what it projects and the GPU front
    // end attaches it to polygon vertices that still carry the same word. Off by default (no cost).
    void enable_precise_geometry();
    const PreciseVertexCache& precise_vertices() const { return precise_; }

    // Runs due scheduler events, then delivers a pending CPU interrupt via on_interrupt.
    void service_events();
    // Forces an event check at the next opportunity (interrupt line raised, SR changed, ...).
    void request_service() { ctx().next_event = ctx().cycles; }

    // Copies a segment image into RAM and marks it resident (used for MAIN at boot and by tests).
    void load_segment_image(std::size_t index, std::span<const uint8_t> image);

    // Must be called whenever RAM in [addr, addr+len) is rewritten by DMA/CD (overlay loads).
    // Affected overlays become unidentified until their code is next needed.
    void invalidate(uint32_t addr, uint32_t len);

    // Resolves a recompiled function by address; fatal if no resident segment has it.
    RecompFunc lookup(uint32_t addr);

    // Registers a native kernel routine at a low-RAM address (device table handlers); consulted before the
    // recompiled segments. `addr` must be below 0x10000 (KUSEG/KSEG0/KSEG1 aliases all match).
    void register_native(uint32_t addr, RecompFunc fn) { natives_[phys(addr)] = fn; }

    // Replaces the recompiled function at `addr` (a resident-segment address, any KSEG alias) with a host routine.
    // Everything that resolves through lookup() (indirect calls, jr targets, vtable entries) then runs `fn` instead;
    // calls that the recompiler resolved statically bypass it, so hook only functions reached indirectly. `fn` runs
    // with the guest's register state at the function entry and finishes like a guest function would (return to the
    // caller by returning). It may call the original through lookup_original(). One override per address; a second
    // registration replaces the first, and a null `fn` removes it.
    void override_function(uint32_t addr, RecompFunc fn);
    // The recompiled function at `addr`, ignoring overrides.
    RecompFunc lookup_original(uint32_t addr);

    // Index of the resident segment covering `addr` (identifying overlays via probes if needed), or -1.
    int resident_segment(uint32_t addr);
    const RecompSegment& segment(std::size_t i) const { return segments_[i]; }

    // BIOS dispatch hook (installed by the HLE BIOS). Default: fatal.
    std::function<void(RecompContext&, uint32_t table, uint32_t fn)> on_bios_call;
    // Replaces service_events() entirely when set (unit tests).
    std::function<void(RecompContext&)> on_service_events;
    // Called when an interrupt is deliverable (IRQ pending and SR allows it). Installed by the BIOS.
    std::function<void(RecompContext&)> on_interrupt;
    std::function<void(RecompContext&, uint32_t pc)> on_syscall;

    // ---- Save states (see the comment above StateRestored) ----

    // Writes or reads the whole machine except the native BIOS and host-owned data (disc, memory cards, pads).
    // Throws StateError; saving also throws when not at a safe point. After a load, call resume() (directly or by
    // unwinding to the code that started the game) to continue. Overlays are matched by name, so a state loads in
    // any build that has the overlays it was taken with and a label at the saved PC.
    void io(StateIo& s);
    // True inside the event check of a backward jump with no native kernel frame active.
    bool at_safe_point();
    // Reports whether native kernel frames are active (installed by the BIOS); null means none.
    std::function<bool()> kernel_idle;
    // Runs `fn` at the next safe point, at the end of service_events. Actions may throw (StateRestored).
    void run_at_safe_point(std::function<void()> fn) { safe_actions_.push_back(std::move(fn)); }
    // True after a load, until resume() has been called.
    bool resume_pending() const { return resume_pending_; }
    // Continues the loaded state; returns when the guest returns to address 0 (the game entry returned).
    void resume();
    // The function that has a resume label at `pc` in the resident segment covering it, or null.
    RecompFunc find_resume(uint32_t pc);

    // Diagnostics.
    uint64_t lookups() const { return lookups_; }
    uint64_t identifications() const { return identifications_; }

private:
    static uint32_t phys(uint32_t a) { return dw3_phys(a) & 0x1FFFFFu; }
    bool probes_match(const RecompSegment& s) const;
    bool code_matches(const RecompSegment& s) const;
    bool covers(const RecompSegment& s, uint32_t addr) const {
        return phys(addr) >= phys(s.base) && phys(addr) < phys(s.base) + s.size;
    }

    // Declaration order is construction order: hardware blocks depend on the bus and scheduler.
    Bus bus_;
    Gte gte_;
    Scheduler sched_;
    IrqController irq_;
    VideoTiming video_;
    Timers timers_;
    Dma dma_;
    Gpu gpu_;
    PreciseVertexCache precise_;
    Cdrom cdrom_;
    Spu spu_;
    XaDecoder xa_;
    Sio sio_;
    Mdec mdec_;
    SoftRasterizer soft_raster_;
    std::span<const RecompSegment> segments_;
    std::vector<bool> resident_;
    std::vector<bool> verified_;  // resident and confirmed since last invalidate
    uint64_t lookups_ = 0, identifications_ = 0;
    std::unordered_map<uint32_t, RecompFunc> natives_;
    std::unordered_map<uint32_t, RecompFunc> overrides_;
    void run_safe_actions();
    std::vector<std::function<void()>> safe_actions_;
    bool resume_pending_ = false;
    uint32_t resume_pc_ = 0;
    std::vector<uint32_t> resume_counts_;  // entries of each segment's resume table
    std::array<uint32_t, 16> mem_ctrl_{};  // 0x1F801000-0x1F80103F (delays/sizes), stored only
    uint32_t ram_size_ = 0x00000B88;       // 0x1F801060
    uint32_t cache_ctrl_ = 0;              // 0xFFFE0130
};

}  // namespace dw3
