#pragma once

// Post-processing options of the GPU renderer. They apply to the picture the console would show (the display area of
// VRAM, at the internal resolution) and never touch VRAM itself. With every option at its default the renderer takes
// the plain path and its output is unchanged. See docs/ARCHITECTURE.md ("Post-processing") for the order of the passes.

#include "dw3/hdr.h"

namespace dw3 {

// How the picture is scaled to its place in the window.
enum class OutputScaler {
    Nearest,
    Linear,
    // FidelityFX Super Resolution 1 (edge-adaptive upscale, then sharpening). Where the picture is not enlarged on
    // either axis it falls back to Linear, since there is nothing to reconstruct.
    Fsr,
};

struct GradeParams {
    float saturation = 1.0f;
    float contrast = 1.0f;
    float vibrance = 0.0f;  // extra saturation for dull colours only
    float gamma = 1.0f;
    float brightness = 0.0f;  // added to every channel after gamma (-0.5..0.5)
    float tint[3] = {1.0f, 1.0f, 1.0f};
    bool neutral() const {
        return saturation == 1.0f && contrast == 1.0f && vibrance == 0.0f && gamma == 1.0f && brightness == 0.0f &&
               tint[0] == 1.0f && tint[1] == 1.0f && tint[2] == 1.0f;
    }
};

struct CrtParams {
    float scanlines = 0.0f;   // 0..1: strength of the scanline modulation
    float mask = 0.0f;        // 0..1: strength of the aperture-grille mask
    float curvature = 0.0f;   // 0..1: barrel distortion with rounded corners
    float vignette = 0.0f;    // 0..1: darkening towards the corners
    bool enabled() const { return scanlines > 0.0f || mask > 0.0f || curvature > 0.0f || vignette > 0.0f; }
};

struct PostFx {
    bool smaa = false;         // SMAA 1x on the picture at its internal resolution
    float sharpness = 0.0f;    // 0 off; otherwise 0..1 contrast adaptive sharpening (RCAS strength when scaling with FSR)
    OutputScaler scaler = OutputScaler::Nearest;
    GradeParams grade;
    CrtParams crt;
    // Output transform for an HDR swap chain (hdr.h). A renderer without one ignores it.
    HdrParams hdr;

    // True if anything beyond plain scaling is switched on.
    bool active() const {
        return smaa || sharpness > 0.0f || scaler == OutputScaler::Fsr || !grade.neutral() || crt.enabled() || hdr.mode != HdrMode::Off;
    }
};

}  // namespace dw3
