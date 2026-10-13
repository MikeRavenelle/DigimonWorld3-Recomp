// Maps the post-processing settings to the renderer's options.
#pragma once

#include "dw3/postfx.h"
#include "display.h"
#include "settings.h"

namespace dw3app {

inline dw3::PostFx post_fx_from(const Settings::Graphics& g) {
    dw3::PostFx fx;
    fx.smaa = g.antialiasing == 1;
    constexpr float kSharpness[] = {0.0f, 0.3f, 0.55f, 0.85f};
    fx.sharpness = kSharpness[g.sharpening & 3];
    constexpr dw3::OutputScaler kScalers[] = {dw3::OutputScaler::Nearest, dw3::OutputScaler::Linear, dw3::OutputScaler::Fsr};
    fx.scaler = kScalers[g.output_filter % 3];
    fx.grade.gamma = gamma_from_percent(g.gamma);
    fx.grade.brightness = brightness_from_percent(g.brightness);
    switch (g.color_grade) {
        case 1:  // vivid
            fx.grade.saturation = 1.15f;
            fx.grade.contrast = 1.06f;
            fx.grade.vibrance = 0.25f;
            break;
        case 2:  // warm
            fx.grade.saturation = 1.05f;
            fx.grade.tint[0] = 1.04f;
            fx.grade.tint[2] = 0.94f;
            break;
        case 3:  // cool
            fx.grade.saturation = 1.05f;
            fx.grade.tint[0] = 0.95f;
            fx.grade.tint[2] = 1.05f;
            break;
        default: break;
    }
    if (g.crt == 1) {
        fx.crt.scanlines = 0.8f;
    } else if (g.crt == 2) {
        fx.crt.scanlines = 0.8f;
        fx.crt.mask = 0.45f;
        fx.crt.curvature = 0.35f;
        fx.crt.vignette = 0.5f;
    }
    return fx;
}

}  // namespace dw3app
