#pragma once

// Test-only MIPS I reference interpreter (the oracle for the emitter differential tests).
// Deliberately independent of recomp.h's helpers: memory goes through dw3::Bus and the unaligned
// and divide semantics are implemented separately. Never link this into the runtime or app.

#include <cstdint>
#include <string>

#include "dw3/bus.h"
#include "dw3/recomp.h"

namespace refint {

constexpr uint32_t kReturnSentinel = 0xFFFF0000u;

struct Result {
    bool returned = false;  // reached a jump to kReturnSentinel
    bool faulted = false;   // unsupported instruction / unaligned access
    uint64_t steps = 0;
    uint32_t pc = 0;        // pc at the point of stopping
    std::string error;
};

// Runs from `entry` until control reaches kReturnSentinel (via jr/jalr, delay slot executed first),
// a fault, or `max_steps` instructions. Registers/hi/lo live in `ctx`; memory in `bus`.
Result run(RecompContext& ctx, dw3::Bus& bus, uint32_t entry, uint64_t max_steps = 2'000'000);

}  // namespace refint
