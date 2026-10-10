#include "dw3/bus.h"

#include <cstdio>
#include <cstring>

namespace dw3 {

Bus::Bus() : ram_(DW3_RAM_SIZE, 0) {
    ctx_.ram = ram_.data();
    ctx_.scratchpad = scratchpad_.data();
    ctx_.bus = this;
}

void Bus::map_io(IoHandler h) { io_.push_back(std::move(h)); }

const IoHandler* Bus::find(uint32_t phys) const {
    for (const auto& h : io_)
        if (phys >= h.base && phys - h.base < h.size) return &h;
    return nullptr;
}

uint32_t Bus::read(uint32_t addr, int size) {
    uint32_t p = dw3_phys(addr);
    if (p < 0x800000u) {
        uint32_t v = 0;
        std::memcpy(&v, ram_.data() + (p & (DW3_RAM_SIZE - 1)), size);
        return v;
    }
    if ((p & ~(DW3_SCRATCHPAD_SIZE - 1)) == DW3_SCRATCHPAD_BASE) {
        uint32_t v = 0;
        std::memcpy(&v, scratchpad_.data() + (p & (DW3_SCRATCHPAD_SIZE - 1)), size);
        return v;
    }
    if (const auto* h = find(p); h && h->read) return h->read(p - h->base, size);
    ++unmapped_reads_;
    return 0;
}

void Bus::write(uint32_t addr, uint32_t value, int size) {
    uint32_t p = dw3_phys(addr);
    if (p < 0x800000u) {
        std::memcpy(ram_.data() + (p & (DW3_RAM_SIZE - 1)), &value, size);
        return;
    }
    if ((p & ~(DW3_SCRATCHPAD_SIZE - 1)) == DW3_SCRATCHPAD_BASE) {
        std::memcpy(scratchpad_.data() + (p & (DW3_SCRATCHPAD_SIZE - 1)), &value, size);
        return;
    }
    if (const auto* h = find(p); h && h->write) {
        h->write(p - h->base, value, size);
        return;
    }
    ++unmapped_writes_;
}

}  // namespace dw3

extern "C" uint32_t dw3_mem_read(RecompContext* ctx, uint32_t addr, int size) {
    return static_cast<dw3::Bus*>(ctx->bus)->read(addr, size);
}

extern "C" void dw3_mem_write(RecompContext* ctx, uint32_t addr, uint32_t value, int size) {
    static_cast<dw3::Bus*>(ctx->bus)->write(addr, value, size);
}
