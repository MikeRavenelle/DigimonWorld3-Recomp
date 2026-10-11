#pragma once

// DMA controller at 0x1F801080: 7 channels (MADR/BCR/CHCR), DPCR and DICR. Data moves instantly when a
// transfer starts; the busy bit and the completion IRQ follow after a cost modelled with the Scheduler.

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "dw3/bus.h"
#include "dw3/irq.h"
#include "dw3/scheduler.h"
#include "dw3/state.h"

namespace dw3 {

enum class DmaChannel : int { MdecIn = 0, MdecOut = 1, Gpu = 2, Cdrom = 3, Spu = 4, Pio = 5, Otc = 6 };

// A peripheral behind a DMA channel (GPU, CD, SPU, MDEC implement this later). Channel 6 (OTC) is
// built in and needs no device. With no device attached, from-RAM words are discarded and to-RAM
// words read as zero.
struct DmaDevice {
    virtual ~DmaDevice() = default;
    // Called when a transfer is about to start; false defers it until Dma::device_ready().
    virtual bool dma_ready(bool /*to_ram*/) const { return true; }
    // RAM -> device (CHCR direction bit set). Sync 2 delivers each list node's payload separately.
    virtual void dma_write(std::span<const uint32_t> /*words*/) {}
    // device -> RAM. Must fill `out` completely.
    virtual void dma_read(std::span<uint32_t> out) { for (auto& w : out) w = 0; }
    virtual uint32_t dma_cycles_per_word() const { return 1; }
};

class Dma {
public:
    static constexpr uint32_t kBase = 0x1F801080u;
    static constexpr uint32_t kMaxLinkedNodes = 0x80000;  // 2 MiB of RAM holds at most this many nodes
    static constexpr uint32_t kRamMask = 0x1FFFFCu;

    // CHCR bits.
    static constexpr uint32_t kFromRam = 1u << 0, kStepBack = 1u << 1, kStart = 1u << 24, kTrigger = 1u << 28;

    Dma(Bus& bus, Scheduler& sched, IrqController& irq);

    void attach(DmaChannel ch, DmaDevice* dev) { dev_[static_cast<int>(ch)] = dev; }
    // A device that previously reported !dma_ready() can now run its pending transfer.
    void device_ready(DmaChannel ch) { try_start(static_cast<int>(ch)); }

    uint32_t read(uint32_t off, int size);
    void write(uint32_t off, uint32_t value, int size);

    uint32_t madr(DmaChannel ch) const { return c_[static_cast<int>(ch)].madr; }
    uint32_t bcr(DmaChannel ch) const { return c_[static_cast<int>(ch)].bcr; }
    uint32_t chcr(DmaChannel ch) const;
    uint32_t dpcr() const { return dpcr_; }
    uint32_t dicr() const;  // including the computed master flag (bit 31)
    bool busy(DmaChannel ch) const { return (c_[static_cast<int>(ch)].chcr & kStart) != 0; }
    bool in_flight(DmaChannel ch) const { return c_[static_cast<int>(ch)].in_flight; }

    // Transfer cost in CPU cycles for `words` payload words and `nodes` list nodes.
    static int64_t transfer_cycles(uint64_t words, uint64_t nodes, uint32_t cycles_per_word);

    uint64_t loop_guard_trips() const { return loop_guard_trips_; }

    // Called for every RAM range DMA wrote (physical byte address within 2 MiB, length in bytes).
    std::function<void(uint32_t addr, uint32_t len)> on_ram_written;
    // Diagnostics (loop guard, invalid modes). Default: ignored.
    std::function<void(const std::string&)> on_error;

    // Save state: channel registers, DPCR/DICR and the completion event of each running transfer. Transfers move
    // their data when they start, so nothing else is in flight. Attached devices are wiring, not state.
    void io(StateIo& s);

private:
    struct Channel {
        uint32_t madr = 0, bcr = 0, chcr = 0;
        bool in_flight = false;
        EventId done = kNoEvent;
    };

    void write_chcr(int ch, uint32_t v);
    void write_dicr(uint32_t value, int size, uint32_t off);
    bool master_flag() const;
    void try_start(int ch);
    void run(int ch);
    void complete(int ch);
    void note_written(uint32_t first, int32_t step, uint64_t count);
    uint32_t ram_read(uint32_t addr) const;
    void ram_write(uint32_t addr, uint32_t v);

    Bus& bus_;
    Scheduler& sched_;
    IrqController& irq_;
    Channel c_[7];
    DmaDevice* dev_[7] = {};
    uint32_t dpcr_ = 0x07654321u;
    uint32_t dicr_ = 0;  // bits 0-5, 15, 16-23, 24-30 (flags); bit 31 computed
    uint64_t loop_guard_trips_ = 0;
};

}  // namespace dw3
