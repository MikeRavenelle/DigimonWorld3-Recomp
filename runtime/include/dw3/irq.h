#pragma once

// Interrupt controller: I_STAT (0x1F801070) and I_MASK (0x1F801074).

#include <cstdint>
#include <functional>

#include "dw3/bus.h"
#include "dw3/state.h"

namespace dw3 {

enum class IrqSource : int {
    Vblank = 0,
    Gpu = 1,
    Cdrom = 2,
    Dma = 3,
    Tmr0 = 4,
    Tmr1 = 5,
    Tmr2 = 6,
    PadMemcard = 7,
    Sio = 8,
    Spu = 9,
    Lightpen = 10,
};

class IrqController {
public:
    static constexpr uint32_t kBase = 0x1F801070u;
    static constexpr uint32_t kBitsMask = 0x7FFu;  // 11 sources

    explicit IrqController(Bus& bus);

    void raise(IrqSource s) { raise_mask(1u << static_cast<int>(s)); }
    void raise_mask(uint32_t bits);
    uint32_t stat() const { return stat_; }
    uint32_t mask() const { return mask_; }
    bool pending() const { return (stat_ & mask_) != 0; }

    // Register access (also reachable through the bus).
    uint32_t read(uint32_t off, int size) const;
    void write(uint32_t off, uint32_t value, int size);

    // Called whenever pending() changes (true = line went high). The CPU/BIOS layer uses this to
    // set COP0 CAUSE.IP2 and deliver the exception when cpu_interrupt_enabled().
    std::function<void(bool pending)> on_line_change;

    // Save state. Loading does not call on_line_change; the owner re-syncs the CPU line afterwards.
    void io(StateIo& s);

private:
    void notify(bool before);

    uint32_t stat_ = 0, mask_ = 0;
};

// COP0 gating: SR.IEc (bit 0) and SR.IM2 (bit 10) both set.
inline bool cpu_interrupt_enabled(const RecompContext& ctx) {
    uint32_t sr = ctx.cop0[12];
    return (sr & 1u) != 0 && (sr & (1u << 10)) != 0;
}

// Mirrors the interrupt line into CAUSE.IP2 (bit 10).
inline void set_cause_ip2(RecompContext& ctx, bool line) {
    if (line) ctx.cop0[13] |= (1u << 10);
    else ctx.cop0[13] &= ~(1u << 10);
}

// True when an exception should be taken now: line high and the CPU has interrupts enabled.
inline bool cpu_interrupt_deliverable(const RecompContext& ctx, const IrqController& irq) {
    return irq.pending() && cpu_interrupt_enabled(ctx);
}

}  // namespace dw3
