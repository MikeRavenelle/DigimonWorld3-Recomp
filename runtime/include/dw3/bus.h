#pragma once

// Memory bus slow path: I/O register dispatch for addresses outside RAM/scratchpad.

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "dw3/recomp.h"

namespace dw3 {

struct IoHandler {
    uint32_t base = 0;  // physical address
    uint32_t size = 0;
    std::function<uint32_t(uint32_t offset, int size)> read;
    std::function<void(uint32_t offset, uint32_t value, int size)> write;
};

class Bus {
public:
    Bus();

    RecompContext& ctx() { return ctx_; }
    uint8_t* scratchpad() { return scratchpad_.data(); }
    uint8_t* ram() { return ram_.data(); }
    const uint8_t* ram() const { return ram_.data(); }

    void map_io(IoHandler h);

    uint32_t read(uint32_t addr, int size);
    void write(uint32_t addr, uint32_t value, int size);

    // Count of accesses that hit no handler (for audit logging / tests).
    uint64_t unmapped_reads() const { return unmapped_reads_; }
    uint64_t unmapped_writes() const { return unmapped_writes_; }

private:
    const IoHandler* find(uint32_t phys) const;

    std::vector<uint8_t> ram_;
    std::array<uint8_t, DW3_SCRATCHPAD_SIZE> scratchpad_{};
    std::vector<IoHandler> io_;
    RecompContext ctx_{};
    uint64_t unmapped_reads_ = 0, unmapped_writes_ = 0;
};

}  // namespace dw3
