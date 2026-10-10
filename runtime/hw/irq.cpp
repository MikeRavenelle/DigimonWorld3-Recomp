#include "dw3/irq.h"

#include "dw3/hw_util.h"

namespace dw3 {

IrqController::IrqController(Bus& bus) {
    bus.map_io({kBase, 8, [this](uint32_t off, int size) { return read(off, size); },
                [this](uint32_t off, uint32_t v, int size) { write(off, v, size); }});
}

void IrqController::notify(bool before) {
    bool after = pending();
    if (after != before && on_line_change) on_line_change(after);
}

void IrqController::raise_mask(uint32_t bits) {
    bool before = pending();
    stat_ |= bits & kBitsMask;
    notify(before);
}

uint32_t IrqController::read(uint32_t off, int size) const {
    uint32_t reg = (off & 4u) ? mask_ : stat_;
    return lane_read(reg, size, off);
}

void IrqController::write(uint32_t off, uint32_t value, int size) {
    bool before = pending();
    if (off & 4u) {
        mask_ = lane_merge(mask_, value, size, off) & kBitsMask;
    } else {
        // Acknowledge: bits written as 0 are cleared; lanes not covered by the access are untouched.
        stat_ &= lane_shift(value, size, off) | ~lane_mask(size, off);
    }
    notify(before);
}

}  // namespace dw3
