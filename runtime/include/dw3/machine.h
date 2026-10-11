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
#include "dw3/recomp.h"
#include "dw3/scheduler.h"
#include "dw3/sio.h"
#include "dw3/soft_rasterizer.h"
#include "dw3/spu.h"
#include "dw3/timers.h"
#include "dw3/video_timing.h"

namespace dw3 {

// Unrecoverable runtime error (e.g. jump to code that was never recompiled). The default handler
// logs and aborts; tests replace it to throw.
using FatalHandler = std::function<void(const std::string& message)>;
void set_fatal_handler(FatalHandler h);
[[noreturn]] void fatal(const std::string& message);

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
    Sio& sio() { return sio_; }
    Mdec& mdec() { return mdec_; }
    RecompContext& ctx() { return bus_.ctx(); }

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

    // Diagnostics.
    uint64_t lookups() const { return lookups_; }
    uint64_t identifications() const { return identifications_; }

private:
    static uint32_t phys(uint32_t a) { return dw3_phys(a) & 0x1FFFFFu; }
    bool probes_match(const RecompSegment& s) const;
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
    Cdrom cdrom_;
    Spu spu_;
    Sio sio_;
    Mdec mdec_;
    SoftRasterizer soft_raster_;
    std::span<const RecompSegment> segments_;
    std::vector<bool> resident_;
    std::vector<bool> verified_;  // resident and confirmed since last invalidate
    uint64_t lookups_ = 0, identifications_ = 0;
    std::unordered_map<uint32_t, RecompFunc> natives_;
    std::array<uint32_t, 16> mem_ctrl_{};  // 0x1F801000-0x1F80103F (delays/sizes), stored only
    uint32_t ram_size_ = 0x00000B88;       // 0x1F801060
    uint32_t cache_ctrl_ = 0;              // 0xFFFE0130
};

}  // namespace dw3
