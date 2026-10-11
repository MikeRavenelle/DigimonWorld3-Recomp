#include "dw3/dma.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "dw3/hw_util.h"

namespace dw3 {

namespace {
constexpr uint32_t kChcrWriteMask = 0x71770703u;
constexpr uint32_t kOtcChcrWriteMask = 0x51000000u;
constexpr uint32_t kDicrKeepMask = 0x00FF803Fu;  // R/W bits of DICR (flags and bit 31 handled separately)
constexpr uint32_t kDicrFlags = 0x7F000000u;
}  // namespace

Dma::Dma(Bus& bus, Scheduler& sched, IrqController& irq) : bus_(bus), sched_(sched), irq_(irq) {
    bus.map_io({kBase, 0x80, [this](uint32_t off, int size) { return read(off, size); },
                [this](uint32_t off, uint32_t v, int size) { write(off, v, size); }});
}

int64_t Dma::transfer_cycles(uint64_t words, uint64_t nodes, uint32_t cycles_per_word) {
    return std::max<int64_t>(1, static_cast<int64_t>(words * cycles_per_word + nodes));
}

uint32_t Dma::chcr(DmaChannel ch) const {
    int i = static_cast<int>(ch);
    return i == 6 ? (c_[i].chcr | 2u) : c_[i].chcr;  // OTC always steps backwards
}

bool Dma::master_flag() const {
    bool force = (dicr_ & (1u << 15)) != 0, master_en = (dicr_ & (1u << 23)) != 0;
    return force || (master_en && ((dicr_ >> 16) & (dicr_ >> 24) & 0x7Fu) != 0);
}

uint32_t Dma::dicr() const { return (dicr_ & 0x7FFFFFFFu) | (master_flag() ? 0x80000000u : 0u); }

uint32_t Dma::read(uint32_t off, int size) {
    uint32_t reg = 0;
    if (off < 0x70) {
        const Channel& c = c_[off >> 4];
        switch ((off >> 2) & 3) {
            case 0: reg = c.madr; break;
            case 1: reg = c.bcr; break;
            case 2: reg = chcr(static_cast<DmaChannel>(off >> 4)); break;
            default: break;
        }
    } else if ((off & ~3u) == 0x70) {
        reg = dpcr_;
    } else if ((off & ~3u) == 0x74) {
        reg = dicr();
    }
    return lane_read(reg, size, off);
}

void Dma::write(uint32_t off, uint32_t value, int size) {
    if (off < 0x70) {
        int ch = static_cast<int>(off >> 4);
        Channel& c = c_[ch];
        switch ((off >> 2) & 3) {
            case 0: c.madr = lane_merge(c.madr, value, size, off) & 0xFFFFFFu; break;
            case 1: c.bcr = lane_merge(c.bcr, value, size, off); break;
            case 2: write_chcr(ch, lane_merge(chcr(static_cast<DmaChannel>(ch)), value, size, off)); break;
            default: break;
        }
    } else if ((off & ~3u) == 0x70) {
        dpcr_ = lane_merge(dpcr_, value, size, off);
        for (int ch = 0; ch < 7; ++ch) try_start(ch);
    } else if ((off & ~3u) == 0x74) {
        write_dicr(value, size, off);
    }
}

void Dma::write_dicr(uint32_t value, int size, uint32_t off) {
    bool before = master_flag();
    uint32_t m = lane_mask(size, off), v = lane_shift(value, size, off);
    dicr_ = (dicr_ & ~(m & kDicrKeepMask)) | (v & m & kDicrKeepMask);
    dicr_ &= ~(v & m & kDicrFlags);  // flags: write 1 to clear
    if (!before && master_flag()) irq_.raise(IrqSource::Dma);
}

void Dma::write_chcr(int ch, uint32_t v) {
    Channel& c = c_[ch];
    uint32_t masked = v & (ch == 6 ? kOtcChcrWriteMask : kChcrWriteMask);
    if (c.in_flight) {
        if (masked & kStart) return;  // already running: restart requests are ignored
        sched_.cancel(c.done);        // clearing the start bit aborts
        c.in_flight = false;
        c.done = kNoEvent;
    }
    c.chcr = masked;
    try_start(ch);
}

uint32_t Dma::ram_read(uint32_t addr) const {
    uint32_t v;
    std::memcpy(&v, bus_.ram() + (addr & kRamMask), 4);
    return v;
}

void Dma::ram_write(uint32_t addr, uint32_t v) { std::memcpy(bus_.ram() + (addr & kRamMask), &v, 4); }

void Dma::note_written(uint32_t first, int32_t step, uint64_t count) {
    if (count == 0 || !on_ram_written) return;
    uint64_t len = std::min<uint64_t>(count * 4, DW3_RAM_SIZE);
    uint32_t lo = step > 0 ? (first & kRamMask) : static_cast<uint32_t>((first - 4 * (count - 1)) & kRamMask);
    if (len == DW3_RAM_SIZE) lo = 0;
    if (lo + len <= DW3_RAM_SIZE) {
        on_ram_written(lo, static_cast<uint32_t>(len));
    } else {
        uint32_t head = DW3_RAM_SIZE - lo;
        on_ram_written(lo, head);
        on_ram_written(0, static_cast<uint32_t>(len - head));
    }
}

void Dma::try_start(int ch) {
    Channel& c = c_[ch];
    if (!(c.chcr & kStart) || c.in_flight) return;
    if (!((dpcr_ >> (4 * ch + 3)) & 1u)) return;
    int sync = ch == 6 ? 0 : static_cast<int>((c.chcr >> 9) & 3u);
    if (sync == 0 && !(c.chcr & kTrigger)) return;
    if (ch != 6 && dev_[ch] && !dev_[ch]->dma_ready(!(c.chcr & kFromRam))) return;
    run(ch);
}

void Dma::run(int ch) {
    Channel& c = c_[ch];
    DmaDevice* dev = ch == 6 ? nullptr : dev_[ch];
    uint32_t cpw = dev ? std::max<uint32_t>(1, dev->dma_cycles_per_word()) : 1;
    bool from_ram = (c.chcr & kFromRam) != 0;
    int32_t step = (c.chcr & kStepBack) ? -4 : 4;
    uint32_t addr = c.madr & kRamMask;
    uint64_t words = 0, nodes = 0;
    std::vector<uint32_t> buf;

    // One burst of `n` words between RAM (stepping from addr) and the device.
    auto burst = [&](uint32_t n) {
        buf.resize(n);
        if (from_ram) {
            uint32_t a = addr;
            for (uint32_t i = 0; i < n; ++i, a = (a + step) & kRamMask) buf[i] = ram_read(a);
            if (dev) dev->dma_write(buf);
        } else {
            if (dev) dev->dma_read(buf);
            else std::fill(buf.begin(), buf.end(), 0u);
            uint32_t a = addr;
            for (uint32_t i = 0; i < n; ++i, a = (a + step) & kRamMask) ram_write(a, buf[i]);
            note_written(addr, step, n);
        }
        addr = (addr + step * n) & kRamMask;
        words += n;
    };

    if (ch == 6) {
        // Ordering table: each entry points at the previous word; the last one is the end marker.
        uint32_t n = c.bcr & 0xFFFFu ? c.bcr & 0xFFFFu : 0x10000u;
        uint32_t first = addr;
        for (uint32_t i = 0; i < n; ++i) {
            ram_write(addr, i == n - 1 ? 0x00FFFFFFu : ((addr - 4) & 0xFFFFFFu));
            addr = (addr - 4) & kRamMask;
        }
        note_written(first, -4, n);
        words = n;
    } else {
        switch ((c.chcr >> 9) & 3u) {
            case 0:
                burst(c.bcr & 0xFFFFu ? c.bcr & 0xFFFFu : 0x10000u);
                break;
            case 1: {
                uint32_t bs = c.bcr & 0xFFFFu ? c.bcr & 0xFFFFu : 0x10000u, bc = c.bcr >> 16;
                for (uint32_t b = 0; b < bc; ++b) burst(bs);
                c.bcr &= 0xFFFFu;  // remaining block count
                break;
            }
            case 2: {
                if (!from_ram) {
                    if (on_error) on_error("DMA linked-list transfer must be RAM->device");
                    break;
                }
                uint32_t next = addr;
                for (;;) {
                    if (nodes >= kMaxLinkedNodes) {
                        ++loop_guard_trips_;
                        if (on_error) on_error("DMA linked list exceeded the node limit (loop?)");
                        break;
                    }
                    ++nodes;
                    uint32_t hdr = ram_read(addr), cnt = hdr >> 24;
                    next = hdr & 0xFFFFFFu;
                    if (cnt) {
                        buf.resize(cnt);
                        for (uint32_t i = 0; i < cnt; ++i) buf[i] = ram_read(addr + 4 + 4 * i);
                        if (dev) dev->dma_write(buf);
                        words += cnt;
                    }
                    if (next & 0x800000u) break;  // end marker (normally 0x00FFFFFF)
                    addr = next & kRamMask;
                }
                addr = next;
                break;
            }
            default:
                if (on_error) on_error("DMA sync mode 3 is reserved");
                break;
        }
    }
    c.madr = addr & 0xFFFFFFu;  // after a linked list: the end marker
    c.in_flight = true;
    c.done = sched_.schedule_in(transfer_cycles(words, nodes, cpw), [this, ch](int64_t) { complete(ch); });
}

void Dma::complete(int ch) {
    Channel& c = c_[ch];
    c.in_flight = false;
    c.done = kNoEvent;
    c.chcr &= ~(kStart | kTrigger);
    bool before = master_flag();
    if (dicr_ & (1u << (16 + ch))) dicr_ |= 1u << (24 + ch);
    if (!before && master_flag()) irq_.raise(IrqSource::Dma);
}

void Dma::io(StateIo& s) {
    StateIo::Section sec(s, "DMA0");
    for (int ch = 0; ch < 7; ++ch) {
        Channel& c = c_[ch];
        s.io(c.madr);
        s.io(c.bcr);
        s.io(c.chcr);
        s.io(c.in_flight);
        sched_.io_event(s, c.done, [this, ch] { return Scheduler::Callback([this, ch](int64_t) { complete(ch); }); });
    }
    s.io(dpcr_);
    s.io(dicr_);
    s.io(loop_guard_trips_);
}

}  // namespace dw3
