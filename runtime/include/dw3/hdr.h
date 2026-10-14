#pragma once

// HDR output maths: the conversion from the game's SDR picture (sRGB encoded, Rec. 709) to the values an HDR swap
// chain wants. The GPU shader (render/shaders/post/hdr.glsl) repeats these formulas; the tests pin them down.
//
// The expansion is deliberately mild. Everything up to the knee stays exactly where SDR puts it (reference white
// is `paper_white_nits`), and only the brightest part of the picture is lifted, towards `peak_nits` for white.
// The gain is applied to all three channels at once, so hue and saturation are those of the SDR picture; no gamut
// expansion is done (Rec. 709 colours stay Rec. 709 colours).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace dw3 {

// What the HDR swap chain holds.
enum class HdrMode : uint8_t {
    Off,
    ScRgb,  // linear Rec. 709 floating point, 1.0 = 80 nits (Windows and Vulkan extended sRGB)
    Pq,     // HDR10: Rec. 2020 primaries, SMPTE ST 2084 transfer, 10000 nits = 1.0
    Edr,    // linear Rec. 709 relative to SDR white (1.0 = paper white), headroom above it (Apple EDR)
};

struct HdrParams {
    HdrMode mode = HdrMode::Off;
    float peak_nits = 1000.0f;
    float paper_white_nits = 203.0f;
    bool operator==(const HdrParams&) const = default;
};

constexpr float kScRgbWhiteNits = 80.0f;
constexpr float kHdrKnee = 0.75f;  // relative luminance where the expansion starts

inline float srgb_to_linear(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

inline float luminance709(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// SMPTE ST 2084 inverse EOTF: absolute luminance in nits to the signal in 0..1.
inline float pq_encode(float nits) {
    constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    const float y = std::pow(std::clamp(nits, 0.0f, 10000.0f) / 10000.0f, m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

// SMPTE ST 2084 EOTF: signal in 0..1 to nits.
inline float pq_decode(float signal) {
    constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
    const float p = std::pow(std::clamp(signal, 0.0f, 1.0f), 1.0f / m2);
    const float y = std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
    return y * 10000.0f;
}

// Brightness multiplier for a pixel of relative linear luminance `y` (1.0 = SDR white) on a display that reaches
// `peak_nits` with SDR white at `paper_nits`: 1 up to the knee, rising smoothly to peak/paper for white.
inline float hdr_gain(float y, float peak_nits, float paper_nits) {
    const float headroom = std::max(peak_nits / paper_nits, 1.0f);
    const float t = std::clamp((y - kHdrKnee) / (1.0f - kHdrKnee), 0.0f, 1.0f);
    return 1.0f + (headroom - 1.0f) * t * t;
}

// The mode that drives a swap chain that reported HDR: Apple's extended-range layers take linear values relative to SDR
// white, the Vulkan and Direct3D 12 swap chains scRGB.
constexpr HdrMode swap_chain_hdr_mode(bool extended_range_layer) { return extended_range_layer ? HdrMode::Edr : HdrMode::ScRgb; }

// What a linear SDR value (SDR white = 1.0) is multiplied by to land on the swap chain, for content that is not
// expanded (the on-screen display).
inline float hdr_output_scale(const HdrParams& p) {
    switch (p.mode) {
        case HdrMode::ScRgb: return p.paper_white_nits / kScRgbWhiteNits;
        case HdrMode::Edr: return 1.0f;
        case HdrMode::Pq:
        case HdrMode::Off: break;
    }
    return 0.0f;
}

// An EDR layer can show `display_headroom` times SDR white (what the system grants right now), which paper white and
// the system brightness already fix; the requested peak is cut to what is reachable so the expansion never asks for
// values the display clips.
inline HdrParams edr_limited(HdrParams p, float display_headroom) {
    p.peak_nits = std::min(p.peak_nits, p.paper_white_nits * std::max(display_headroom, 1.0f));
    return p;
}

// Linear Rec. 709 to linear Rec. 2020 (ITU-R BT.2087).
inline std::array<float, 3> rec709_to_rec2020(float r, float g, float b) {
    return {0.6274040f * r + 0.3292820f * g + 0.0433136f * b, 0.0690970f * r + 0.9195400f * g + 0.0113612f * b,
            0.0163916f * r + 0.0880132f * g + 0.8955950f * b};
}

// The swap chain value of an sRGB-encoded SDR pixel for the given output: what the shader writes.
inline std::array<float, 3> hdr_encode_pixel(float sr, float sg, float sb, const HdrParams& p) {
    const float r = srgb_to_linear(sr), g = srgb_to_linear(sg), b = srgb_to_linear(sb);
    const float headroom = std::max(p.peak_nits / p.paper_white_nits, 1.0f);
    const float gain = hdr_gain(luminance709(r, g, b), p.peak_nits, p.paper_white_nits);
    // Relative to paper white, never beyond the display's peak.
    const std::array<float, 3> rel = {std::min(r * gain, headroom), std::min(g * gain, headroom), std::min(b * gain, headroom)};
    switch (p.mode) {
        case HdrMode::ScRgb: {
            const float k = p.paper_white_nits / kScRgbWhiteNits;
            return {rel[0] * k, rel[1] * k, rel[2] * k};
        }
        case HdrMode::Pq: {
            const auto c = rec709_to_rec2020(rel[0] * p.paper_white_nits, rel[1] * p.paper_white_nits, rel[2] * p.paper_white_nits);
            return {pq_encode(c[0]), pq_encode(c[1]), pq_encode(c[2])};
        }
        case HdrMode::Edr: return rel;
        case HdrMode::Off: break;
    }
    return {sr, sg, sb};
}

}  // namespace dw3
