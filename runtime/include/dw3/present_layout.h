// Placement of the emulated display inside the host window (backend independent).
#pragma once

#include <cstdint>

namespace dw3 {

enum class ScaleMode {
    Fit,      // largest 4:3 rectangle that fits, centred
    Integer,  // largest whole multiple of the 4:3-corrected image that fits, centred (falls back to Fit)
    Stretch,  // fill the window
};

struct PresentRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const PresentRect&) const = default;
};

// The PS1 output is shown on a 4:3 display whatever the horizontal mode (256..640 pixels wide).
constexpr int kDisplayAspectNum = 4;
constexpr int kDisplayAspectDen = 3;

// Destination rectangle, in window pixels, for a src_w x src_h display area shown at aspect_num:aspect_den (4:3
// unless the picture is widened). Returns an empty rectangle for degenerate sizes.
PresentRect compute_present_rect(int src_w, int src_h, int win_w, int win_h, ScaleMode mode,
                                 int aspect_num = kDisplayAspectNum, int aspect_den = kDisplayAspectDen);

// Parses "fit", "integer" or "stretch". Returns false on anything else.
bool parse_scale_mode(const char* text, ScaleMode& out);

// Row stride in pixels for a texture upload buffer: width rounded up to a multiple of 64 pixels (256 bytes at
// 4 bytes per pixel), the row pitch alignment required by Direct3D 12.
constexpr uint32_t upload_row_pixels(uint32_t width) { return (width + 63u) & ~63u; }

}  // namespace dw3
