#pragma once

// Helpers for 8/16/32-bit accesses to 32-bit I/O registers (little-endian byte lanes).

#include <cstdint>

namespace dw3 {

// Bits of a 32-bit register covered by an access of `size` bytes at byte offset `off` (low 2 bits used).
inline uint32_t lane_mask(int size, uint32_t off) {
    uint32_t bits = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);
    return bits << ((off & 3u) * 8u);
}

// Value as it appears in the 32-bit register lane.
inline uint32_t lane_shift(uint32_t value, int size, uint32_t off) {
    return (value << ((off & 3u) * 8u)) & lane_mask(size, off);
}

// What a read of `size` bytes at `off` returns from a 32-bit register.
inline uint32_t lane_read(uint32_t reg, int size, uint32_t off) {
    return (reg & lane_mask(size, off)) >> ((off & 3u) * 8u);
}

// Register with the covered lanes replaced by `value`.
inline uint32_t lane_merge(uint32_t reg, uint32_t value, int size, uint32_t off) {
    uint32_t m = lane_mask(size, off);
    return (reg & ~m) | lane_shift(value, size, off);
}

}  // namespace dw3
