#pragma once

// Software reference rasterizer for the PS1 GPU. Integer-only, allocation-free per primitive.
// Rasterization rules follow psx-spx; fill convention, fixed-point attribute interpolation and the
// dither/blend pipeline are modelled on Beetle PSX / Mednafen (GPL-2+) and PCSX-Redux soft GPU (GPL-2+).

#include "dw3/gpu.h"

namespace dw3 {

class SoftRasterizer final : public Rasterizer {
public:
    void triangle(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f,
                  const GpuVertex v[3]) override;
    void rect(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& v, int width,
              int height) override;
    void line(std::span<uint16_t> vram, const DrawState& s, const PrimitiveFlags& f, const GpuVertex& a,
              const GpuVertex& b) override;
};

}  // namespace dw3
