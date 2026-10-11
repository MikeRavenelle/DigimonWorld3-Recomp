// SPU model. Register semantics and DSP behaviour follow psx-spx "Sound Processing Unit (SPU)"
// (https://psx-spx.consoledev.net/); details psx-spx leaves open follow PCSX-Redux and Beetle PSX / Mednafen
// (both GPL-2.0-or-later).

#include "dw3/spu.h"

#include <algorithm>

namespace dw3 {

using namespace spu_dsp;

Spu::Spu(Bus& bus, IrqController& irq, Dma& dma) : irq_(irq), ram_(kRamSize, 0) {
    bus.map_io({kBase, kSize,
                [this](uint32_t off, int size) -> uint32_t {
                    if (size == 4) return read16(off) | (static_cast<uint32_t>(read16(off + 2)) << 16);
                    uint16_t v = read16(off & ~1u);
                    return size == 1 ? (off & 1 ? v >> 8 : v & 0xFF) : v;
                },
                [this](uint32_t off, uint32_t v, int size) {
                    if (size == 4) {
                        write16(off, static_cast<uint16_t>(v));
                        write16(off + 2, static_cast<uint16_t>(v >> 16));
                    } else {
                        write16(off & ~1u, static_cast<uint16_t>(v));
                    }
                }});
    dma.attach(DmaChannel::Spu, this);
}

uint16_t Spu::read16(uint32_t off) {
    off &= kSize - 1;
    switch (off) {
        case kSpuStat: {
            // Bits 0-5 mirror SPUCNT; bit 7 (DMA R/W request) follows SPUCNT transfer mode; never busy.
            uint16_t cnt = regs_[kSpuCnt >> 1];
            uint16_t mode = (cnt >> 4) & 3;
            uint16_t s = cnt & 0x3F;
            if (mode == 2) s |= 1u << 8;   // DMA write request
            if (mode == 3) s |= 1u << 9;   // DMA read request
            if (mode >= 2) s |= 1u << 7;   // DMA R/W request
            s |= regs_[kSpuStat >> 1] & (1u << 6);               // bit 6: IRQ9 flag
            if (capture_pos_ >= 0x100) s |= 1u << 11;            // capture buffers: second half being written
            return static_cast<uint16_t>(s);
        }
        case kTransferFifo: return read_ram16();
        case kEndx: return static_cast<uint16_t>(endx_ & 0xFFFF);
        case kEndx + 2: return static_cast<uint16_t>((endx_ >> 16) & 0xFF);
        case kMainVolXL: return static_cast<uint16_t>(main_vol_[0].level);
        case kMainVolXR: return static_cast<uint16_t>(main_vol_[1].level);
        default: break;
    }
    if (off < 0x180) {
        const Voice& v = voices_[off >> 4];
        if ((off & 0xF) == 0xC) return static_cast<uint16_t>(v.level);  // ENVX
    } else if (off >= kVoiceVolX && off < kVoiceVolX + kVoices * 4) {
        return static_cast<uint16_t>(voices_[(off - kVoiceVolX) >> 2].vol[(off >> 1) & 1].level);
    }
    return regs_[off >> 1];
}

void Spu::write16(uint32_t off, uint16_t v) {
    off &= kSize - 1;
    regs_[off >> 1] = v;
    switch (off) {
        case kTransferAddr: transfer_addr_ = static_cast<uint32_t>(v) * 8; break;
        case kTransferFifo: write_ram16(v); break;
        case kKeyOn: key_on(v); break;
        case kKeyOn + 2: key_on(static_cast<uint32_t>(v & 0xFF) << 16); break;
        case kKeyOff: key_off(v); break;
        case kKeyOff + 2: key_off(static_cast<uint32_t>(v & 0xFF) << 16); break;
        case kSpuCnt:
            if (!(v & (1u << 6))) regs_[kSpuStat >> 1] &= ~(1u << 6);  // IRQ9 enable cleared -> ack
            break;
        case kMainVolL: main_vol_[0].write(v); break;
        case kMainVolR: main_vol_[1].write(v); break;
        case kReverbStart: reverb_.set_start(static_cast<uint32_t>(v) * 8); break;
        default:
            if (off < 0x180) {
                Voice& vc = voices_[off >> 4];
                switch ((off & 0xF) >> 1) {
                    case 0: vc.vol[0].write(v); break;
                    case 1: vc.vol[1].write(v); break;
                    case 6: vc.level = static_cast<int16_t>(v); break;  // ENVX: jump the envelope
                    case 7: vc.repeat = (static_cast<uint32_t>(v) * 8) & (kRamSize - 1); break;
                    default: break;
                }
            }
            break;
    }
}

void Spu::check_irq(uint32_t addr) {
    uint16_t cnt = regs_[kSpuCnt >> 1];
    if (!(cnt & (1u << 6))) return;  // IRQ9 disabled
    uint32_t irq_addr = static_cast<uint32_t>(regs_[kIrqAddr >> 1]) * 8;
    if ((addr & ~1u) == irq_addr && !(regs_[kSpuStat >> 1] & (1u << 6))) {
        regs_[kSpuStat >> 1] |= 1u << 6;
        irq_.raise(IrqSource::Spu);
    }
}

void Spu::write_ram16(uint16_t v) {
    check_irq(transfer_addr_);
    ram_[transfer_addr_] = static_cast<uint8_t>(v);
    ram_[transfer_addr_ + 1] = static_cast<uint8_t>(v >> 8);
    transfer_addr_ = (transfer_addr_ + 2) & (kRamSize - 1);
}

uint16_t Spu::read_ram16() {
    check_irq(transfer_addr_);
    uint16_t v = static_cast<uint16_t>(ram_[transfer_addr_] | (ram_[transfer_addr_ + 1] << 8));
    transfer_addr_ = (transfer_addr_ + 2) & (kRamSize - 1);
    return v;
}

void Spu::dma_write(std::span<const uint32_t> words) {
    for (uint32_t w : words) {
        write_ram16(static_cast<uint16_t>(w));
        write_ram16(static_cast<uint16_t>(w >> 16));
    }
}

void Spu::dma_read(std::span<uint32_t> out) {
    for (auto& w : out) {
        uint32_t lo = read_ram16();
        w = lo | (static_cast<uint32_t>(read_ram16()) << 16);
    }
}


}  // namespace dw3
