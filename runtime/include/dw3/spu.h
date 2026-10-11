#pragma once

// PS1 SPU register file and sound RAM (0x1F801C00-0x1F801FFF, 512 KiB SPU RAM, DMA channel 4).
// This layer models what software can observe: registers, SPUSTAT/SPUCNT handshakes, manual and
// DMA transfers into sound RAM, and voice key-on/off bookkeeping. Sample generation (ADPCM decode,
// ADSR, reverb, XA/CD mixing, audio output) builds on top of it.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "dw3/bus.h"
#include "dw3/dma.h"
#include "dw3/irq.h"

namespace dw3 {

class Spu : public DmaDevice {
public:
    static constexpr uint32_t kBase = 0x1F801C00u;
    static constexpr uint32_t kSize = 0x400;
    static constexpr uint32_t kRamSize = 512 * 1024;

    // Register offsets from kBase.
    static constexpr uint32_t kKeyOn = 0x188, kKeyOff = 0x18C, kEndx = 0x19C;
    static constexpr uint32_t kIrqAddr = 0x1A4, kTransferAddr = 0x1A6, kTransferFifo = 0x1A8;
    static constexpr uint32_t kSpuCnt = 0x1AA, kTransferCtrl = 0x1AC, kSpuStat = 0x1AE;

    Spu(Bus& bus, IrqController& irq, Dma& dma);

    uint16_t read16(uint32_t offset);
    void write16(uint32_t offset, uint16_t value);

    // DmaDevice (channel 4): RAM -> SPU RAM writes and SPU RAM -> RAM reads at the transfer address.
    void dma_write(std::span<const uint32_t> words) override;
    void dma_read(std::span<uint32_t> out) override;

    std::span<const uint8_t> ram() const { return ram_; }
    uint16_t reg(uint32_t offset) const { return regs_[(offset & (kSize - 1)) >> 1]; }
    uint32_t transfer_address() const { return transfer_addr_; }
    uint32_t voices_on() const { return voices_on_; }

private:
    void write_ram16(uint16_t v);
    uint16_t read_ram16();
    void check_irq(uint32_t addr);

    IrqController& irq_;
    std::array<uint16_t, kSize / 2> regs_{};
    std::vector<uint8_t> ram_;
    uint32_t transfer_addr_ = 0;  // byte address in SPU RAM
    uint32_t voices_on_ = 0;      // key-on bitmask (until the voice engine reports ENDX)
};

}  // namespace dw3
